// Isolated Win32 regression suite. Never runs the app, installer or any shell.
// g++ -std=c++17 -D_WIN32_WINNT=0x0A00 -DNOMINMAX test_file_safety.cpp
//     ../src/core/file_ops.cpp ../src/core/path_policy.cpp -o test_file_safety.exe
#include "../src/core/file_ops.h"
#include "../src/core/path_policy.h"
#include "../src/core/root_store.h"
#include "../src/core/sync_engine.h"
#include "../src/common/utf.h"
#include <json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <winioctl.h>
static_assert(!std::is_copy_constructible<fileops::DirectoryLease>::value, "leases cannot copy");
static_assert(std::is_nothrow_move_constructible<fileops::DirectoryLease>::value, "lease move cannot throw");
static_assert(std::is_nothrow_move_assignable<fileops::DirectoryLease>::value,
              "lease move assignment cannot throw");

namespace {
int passed = 0, failed = 0, skipped = 0;
#define CHECK(c, text)                                                                                       \
    do {                                                                                                     \
        if (c) {                                                                                             \
            ++passed;                                                                                        \
            std::printf("[PASS] %s\n", text);                                                                \
        } else {                                                                                             \
            ++failed;                                                                                        \
            std::printf("[FAIL] %s (Win32=%lu)\n", text, GetLastError());                                    \
        }                                                                                                    \
    } while (0)
std::wstring L(const std::wstring& p) {
    return pathpolicy::LongPath(p);
}
void Put(const std::wstring& path, const std::string& content) {
    if (!fileops::EnsureDir(path.substr(0, path.find_last_of(L'\\'))))
        throw std::runtime_error("fixture directory");
    winutil::ScopedHandle h(CreateFileW(L(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.valid())
        throw std::runtime_error("fixture open");
    DWORD written = 0;
    if (!WriteFile(h.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr) ||
        written != content.size())
        throw std::runtime_error("fixture write");
}
std::string Get(const std::wstring& path) {
    winutil::ScopedHandle h(CreateFileW(L(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid())
        return "<open-error>";
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.get(), &size) || size.QuadPart > 16 * 1024 * 1024)
        return "<size-error>";
    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    if (!ReadFile(h.get(), out.data(), static_cast<DWORD>(out.size()), &got, nullptr) || got != out.size())
        return "<read-error>";
    return out;
}
// Cleanup is deliberately NOT ListFiles(): internal markers must also be removed.
// Never traverse reparse points, and only accept descendants of the unique root
// created by this process. It cannot delete any existing user directory.
bool RemoveOwned(const std::wstring& path, const std::wstring& owned) {
    if (path != owned &&
        (path.size() <= owned.size() || path.compare(0, owned.size(), owned) || path[owned.size()] != L'\\'))
        return false;
    DWORD attrs = GetFileAttributesW(L(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return true;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(L(path).c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(L(path).c_str()) != FALSE;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        return RemoveDirectoryW(L(path).c_str()) != FALSE;
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW(L(path + L"\\*").c_str(), &fd);
    bool ok = true;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            if (!RemoveOwned(path + L"\\" + fd.cFileName, owned))
                ok = false;
        } while (FindNextFileW(f, &fd));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            ok = false;
        FindClose(f);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
        ok = false;
    return RemoveDirectoryW(L(path).c_str()) != FALSE && ok;
}
struct Sandbox {
    std::wstring root;
    bool created = false;
    Sandbox() {
        std::vector<wchar_t> temp(32768);
        DWORD n = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (!n || n >= temp.size())
            throw std::runtime_error("GetTempPathW");
        std::wstring base(temp.data(), n);
        while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
            base.pop_back();
        for (int attempt = 0; attempt != 100; ++attempt) {
            root = base + L"\\LOLBackup-file-safety-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                   std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
            std::wstring error;
            if (!pathpolicy::ValidateRoot(root, error))
                throw std::runtime_error("unsafe temp root");
            if (CreateDirectoryW(L(root).c_str(), nullptr)) {
                created = true;
                return;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                throw std::runtime_error("create unique sandbox");
        }
        throw std::runtime_error("unique sandbox exhausted");
    }
    ~Sandbox() {
        if (created && !RemoveOwned(root, root))
            std::printf("[WARN] cleanup incomplete: %ls\n", root.c_str());
    }
};
std::wstring Child(const Sandbox& env, const wchar_t* leaf) {
    return env.root + L"\\" + leaf;
}
void Own(const std::wstring& dir) {
    std::wstring error;
    if (!fileops::PrepareDedicatedRoot(dir, error)) {
        std::printf("fixture ownership failed: %ls\n", error.c_str());
        throw std::runtime_error("ownership fixture");
    }
}
int RawTempCount(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW(L(dir + L"\\.~lolbackup-*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    int count = 0;
    do {
        ++count;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return count;
}
bool SetJunction(const std::wstring& directory, const std::wstring& target) {
    std::wstring substitute = L"\\??\\" + target;
    size_t textBytes = (substitute.size() + 1 + target.size() + 1) * sizeof(wchar_t);
    struct Header {
        DWORD tag;
        WORD length, reserved, subOffset, subLength, printOffset, printLength;
    };
    std::vector<unsigned char> bytes(sizeof(Header) + textBytes, 0);
    auto* header = reinterpret_cast<Header*>(bytes.data());
    header->tag = IO_REPARSE_TAG_MOUNT_POINT;
    header->length = static_cast<WORD>(8 + textBytes);
    header->subLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    header->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    header->printLength = static_cast<WORD>(target.size() * sizeof(wchar_t));
    auto* text = bytes.data() + sizeof(Header);
    std::memcpy(text, substitute.data(), header->subLength);
    std::memcpy(text + header->printOffset, target.data(), header->printLength);
    winutil::ScopedHandle writer(CreateFileW(
        L(directory).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!writer.valid())
        return false;
    DWORD returned = 0;
    return DeviceIoControl(writer.get(), FSCTL_SET_REPARSE_POINT, bytes.data(),
                           static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr) != FALSE;
}
// Explicit selection is a different authority from the strict dedicated-root API above.
void TestBusinessInventory(const Sandbox& env) {
    const auto root = Child(env, L"mixed-business-inventory");
    Put(root + L"\\config.ini", "CONFIG");
    Put(root + L"\\saves\\a.dat", "SAVE");
    Put(root + L"\\shards\\b.dat", "SHARD");
    Put(root + L"\\projects\\locked.txt", "LOCKED-UNRELATED");
    const auto target = Child(env, L"outside-link-target.dat");
    Put(target, "LINKED-UNRELATED");
    CHECK(CreateHardLinkW(L(root + L"\\projects\\alias.txt").c_str(), L(target).c_str(), nullptr),
          "unrelated hardlink fixture is created outside managed names");
    winutil::ScopedHandle lock(CreateFileW(L(root + L"\\projects\\locked.txt").c_str(), GENERIC_READ,
                                           0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(lock.valid(), "unrelated file is exclusively locked during inventory");
    auto generic = fileops::ListFilesChecked(root);
    CHECK(!generic.complete, "whole-root scan would fail on unrelated locked/hardlinked content");
    auto business = fileops::ListBusinessFilesChecked(root);
    CHECK(business.complete && !business.cancelled && business.errors.empty() &&
              business.files == std::vector<std::wstring>({L"config.ini", L"saves\\a.dat", L"shards\\b.dat"}),
          "managed inventory lists exactly two business subtrees and six allowed root names");
    auto cancelled = fileops::ListBusinessFilesChecked(root, [] { return true; });
    CHECK(!cancelled.complete && cancelled.cancelled && cancelled.files.empty(),
          "business inventory keeps cancellation fail-closed without a partial-success flag");
    lock.reset();
    CHECK(Get(root + L"\\projects\\locked.txt") == "LOCKED-UNRELATED" && Get(target) == "LINKED-UNRELATED",
          "business inventory preserves every unrelated byte");
    const auto bad = Child(env, L"business-inventory-linked-managed");
    CHECK(fileops::EnsureDir(bad) &&
              CreateHardLinkW(L(bad + L"\\config.ini").c_str(), L(target).c_str(), nullptr),
          "managed hardlink safety fixture is created");
    auto refused = fileops::ListBusinessFilesChecked(bad);
    CHECK(!refused.complete && !refused.errors.empty() && Get(target) == "LINKED-UNRELATED",
          "same hardlink protection is still enforced for managed names");
    const auto malformed = Child(env, L"business-inventory-type-conflict");
    CHECK(fileops::EnsureDir(malformed + L"\\config.ini"), "managed root file type-conflict fixture exists");
    auto type = fileops::ListBusinessFilesChecked(malformed);
    CHECK(!type.complete && !type.errors.empty(), "managed directory/file type conflicts cannot look empty");
}
void TestSelectedRoot(const Sandbox& env) {
    std::wstring error;
    for (bool business : {false, true}) {
        const auto root = Child(env, business ? L"selected-mixed" : L"selected-documents");
        Put(root + L"\\notes.txt", "PERSONAL-NOTES");
        Put(root + L"\\projects\\nested\\work.bin", "PERSONAL-WORK");
        CHECK(fileops::EnsureDir(root + L"\\LOLBackupV3Cache"), "user-created empty wrapper fixture exists");
        if (business)
            Put(root + L"\\saves\\one.dat", "EXISTING-SAVE");
        CHECK(!fileops::PrepareDedicatedRoot(root, error),
              "strict dedicated preparation still refuses an unowned ordinary nonempty root");
        CHECK(fileops::PrepareSelectedRoot(root, error),
              "explicit selected preparation accepts ordinary documents with or without managed files");
        const auto marker = root + L"\\.lolbackup-owned-v1";
        const DWORD attributes = GetFileAttributesW(L(marker).c_str());
        CHECK(fileops::IsDedicatedRoot(root, error) && attributes != INVALID_FILE_ATTRIBUTES &&
                  (attributes & FILE_ATTRIBUTE_HIDDEN) &&
                  Get(marker) == "LOLBackup dedicated mirror root v1\r\n",
              "selection writes a valid hidden ownership marker in the selected root itself");
        const auto markerBytes = Get(marker);
        // Marker validation normalizes the OS archive bit to HIDDEN|SYSTEM. Compare
        // repeated preparation with that verified baseline, not pre-validation CREATE flags.
        const DWORD verifiedAttributes = GetFileAttributesW(L(marker).c_str());
        CHECK(fileops::PrepareSelectedRoot(root, error) && Get(marker) == markerBytes &&
                  GetFileAttributesW(L(marker).c_str()) == verifiedAttributes,
              "selected preparation is idempotent for an existing valid marker");
        CHECK(Get(root + L"\\notes.txt") == "PERSONAL-NOTES" &&
                  Get(root + L"\\projects\\nested\\work.bin") == "PERSONAL-WORK" &&
                  (!business || Get(root + L"\\saves\\one.dat") == "EXISTING-SAVE") &&
                  fileops::IsDir(root + L"\\LOLBackupV3Cache") &&
                  !fileops::Exists(root + L"\\LOLBackupV3Cache\\.lolbackup-owned-v1") &&
                  !fileops::Exists(root + L"\\LOLBackupV3Snapshots") && RawTempCount(root) == 0,
              "selection preserves user bytes and preexisting empty wrapper without creating another root");
        CHECK(!fileops::RemoveIfEmptyDedicatedRoot(root, error) &&
                  Get(root + L"\\projects\\nested\\work.bin") == "PERSONAL-WORK",
              "a selected marker does not authorize empty-root cleanup of mixed user contents");
    }
    const auto conflict = Child(env, L"selected-forged-marker");
    Put(conflict + L"\\.lolbackup-owned-v1", "USER-MARKER-CONFLICT");
    Put(conflict + L"\\notes.txt", "KEEP-CONFLICT");
    const DWORD conflictAttrs = GetFileAttributesW(L(conflict + L"\\.lolbackup-owned-v1").c_str());
    CHECK(!fileops::PrepareSelectedRoot(conflict, error) &&
              Get(conflict + L"\\.lolbackup-owned-v1") == "USER-MARKER-CONFLICT" &&
              GetFileAttributesW(L(conflict + L"\\.lolbackup-owned-v1").c_str()) == conflictAttrs &&
              Get(conflict + L"\\notes.txt") == "KEEP-CONFLICT",
          "explicit selection rejects marker content conflict without changing bytes or attributes");
    const auto directoryMarker = Child(env, L"selected-directory-marker");
    Put(directoryMarker + L"\\.lolbackup-owned-v1\\personal.txt", "KEEP-DIRECTORY-MARKER");
    CHECK(!fileops::PrepareSelectedRoot(directoryMarker, error) &&
              Get(directoryMarker + L"\\.lolbackup-owned-v1\\personal.txt") == "KEEP-DIRECTORY-MARKER",
          "explicit selection cannot replace a conflicting directory marker");
    const auto linked = Child(env, L"selected-hardlinked-marker"), external = Child(env, L"selected-marker-target");
    Put(external, "LOLBackup dedicated mirror root v1\r\n");
    CHECK(fileops::EnsureDir(linked) &&
              CreateHardLinkW(L(linked + L"\\.lolbackup-owned-v1").c_str(), L(external).c_str(), nullptr),
          "selected hardlinked marker fixture exists entirely inside sandbox");
    const DWORD externalAttrs = GetFileAttributesW(L(external).c_str());
    CHECK(!fileops::PrepareSelectedRoot(linked, error) &&
              Get(external) == "LOLBackup dedicated mirror root v1\r\n" &&
              GetFileAttributesW(L(external).c_str()) == externalAttrs,
          "explicit selection rejects a hardlinked marker without mutating its other alias");
    const auto outside = Child(env, L"selected-link-target"), junction = Child(env, L"selected-junction");
    Put(outside + L"\\personal.txt", "KEEP-LINK-TARGET");
    CHECK(fileops::EnsureDir(junction) && SetJunction(junction, outside), "selected junction fixture installed");
    CHECK(!fileops::PrepareSelectedRoot(junction, error) &&
              !fileops::PrepareSelectedRoot(junction + L"\\new-child", error) &&
              Get(outside + L"\\personal.txt") == "KEEP-LINK-TARGET" &&
              !fileops::Exists(outside + L"\\.lolbackup-owned-v1") &&
              !fileops::Exists(outside + L"\\new-child"),
          "explicit selection refuses root and ancestor links without creating anything at their target");
    const auto linkedMarker = Child(env, L"selected-junction-marker");
    CHECK(fileops::EnsureDir(linkedMarker + L"\\.lolbackup-owned-v1") &&
              SetJunction(linkedMarker + L"\\.lolbackup-owned-v1", outside),
          "selected marker junction fixture installed");
    CHECK(!fileops::PrepareSelectedRoot(linkedMarker, error) &&
              Get(outside + L"\\personal.txt") == "KEEP-LINK-TARGET",
          "explicit selection rejects a marker junction without traversing its target");

    // A real write-denying DACL on our own temporary directory, not a read-only attribute
    // or a guessed sharing failure. Restore the original descriptor before sandbox cleanup.
    const auto denied = Child(env, L"selected-permission-denied");
    Put(denied + L"\\personal.txt", "KEEP-PERMISSIONS");
    DWORD needed = 0;
    GetFileSecurityW(L(denied).c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
    if (!needed)
        throw std::runtime_error("query isolated directory DACL");
    std::vector<BYTE> original(needed);
    if (!GetFileSecurityW(L(denied).c_str(), DACL_SECURITY_INFORMATION,
                          reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()), needed, &needed))
        throw std::runtime_error("read isolated directory DACL");
    struct RestoreDacl {
        std::wstring path;
        std::vector<BYTE>& bytes;
        ~RestoreDacl() {
            CHECK(SetFileSecurityW(L(path).c_str(), DACL_SECURITY_INFORMATION,
                                   reinterpret_cast<PSECURITY_DESCRIPTOR>(bytes.data())),
                  "restore temporary directory DACL after permission regression");
        }
    } restore{denied, original};
    BYTE sid[SECURITY_MAX_SID_SIZE]{};
    DWORD sidSize = sizeof(sid);
    std::vector<BYTE> aclBytes(sizeof(ACL) + 2 * (sizeof(ACCESS_DENIED_ACE) + SECURITY_MAX_SID_SIZE));
    auto acl = reinterpret_cast<PACL>(aclBytes.data());
    SECURITY_DESCRIPTOR descriptor{};
    if (!CreateWellKnownSid(WinWorldSid, nullptr, sid, &sidSize) ||
        !InitializeAcl(acl, static_cast<DWORD>(aclBytes.size()), ACL_REVISION) ||
        !AddAccessDeniedAce(acl, ACL_REVISION, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY, sid) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, sid) ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetFileSecurityW(L(denied).c_str(), DACL_SECURITY_INFORMATION, &descriptor))
        throw std::runtime_error("install isolated write-denying DACL");
    CHECK(!fileops::PrepareSelectedRoot(denied, error) && GetLastError() == ERROR_ACCESS_DENIED &&
              !fileops::Exists(denied + L"\\.lolbackup-owned-v1") &&
              Get(denied + L"\\personal.txt") == "KEEP-PERMISSIONS",
          "explicit selection still refuses denied create permissions without claiming or altering user files");
}
void TestExplicitForeignIdentityAuthorization(const Sandbox& env) {
    const auto cache = Child(env, L"foreign-current"), original = Child(env, L"foreign-original"),
               management = Child(env, L"foreign-management");
    Put(cache + L"\\saves\\old.dat", "OLD-CACHE-OBJECT");
    core::rootstore::Record first, selected;
    std::wstring error;
    CHECK(core::rootstore::Resolve(management, cache, first, error), "register original physical cache identity");
    const auto owner = first.directory + L"\\owner.json", journal = first.directory + L"\\mirror-state-v2.json";
    const auto oldOwner = Get(owner);
    Put(journal, "OLD-DELETION-INTENT-BYTES");
    CHECK(cache.rfind(env.root + L"\\", 0) == 0 && original.rfind(env.root + L"\\", 0) == 0 &&
              !fileops::Exists(original), "identity replacement moves only known children of the exclusive sandbox");
    CHECK(MoveFileW(L(cache).c_str(), L(original).c_str()) && fileops::EnsureDir(cache),
          "replace physical cache object without deleting the original fixture");
    Put(cache + L"\\personal.txt", "NEW-CACHE-OBJECT");
    CHECK(!core::rootstore::Resolve(management, cache, selected, error) &&
              error.find(L"[E_ROOT_IDENTITY_CHANGED]") != std::wstring::npos,
          "default physical identity mismatch is actionable and never auto-authorized");
    CHECK(Get(owner) == oldOwner && Get(journal) == "OLD-DELETION-INTENT-BYTES" &&
              Get(original + L"\\saves\\old.dat") == "OLD-CACHE-OBJECT",
          "identity refusal preserves old owner, journal and original cache object");
    CHECK(core::rootstore::Resolve(management, cache, selected, error, true, true) &&
              selected.identityVariant && selected.identity != first.identity &&
              selected.directory == first.directory + L"-" + selected.identity,
          "explicit confirmation creates a separate current-identity authorization variant");
    CHECK(Get(owner) == oldOwner && Get(journal) == "OLD-DELETION-INTENT-BYTES" &&
              !fileops::Exists(selected.directory + L"\\mirror-state-v2.json") &&
              Get(cache + L"\\personal.txt") == "NEW-CACHE-OBJECT",
          "new environment never inherits, overwrites or deletes prior intent or user bytes");
    core::rootstore::Record reopened;
    CHECK(core::rootstore::Resolve(management, cache, reopened, error, false) &&
              reopened.identityVariant && reopened.directory == selected.directory && reopened.identity == selected.identity,
          "confirmed variant is reusable after restart without repeated confirmation");
    core::rootstore::Lease held;
    CHECK(held.Acquire(reopened, error) && held.Matches(reopened) && held.FinishLegacyCleanup(error),
          "confirmed environment still requires physical exclusion and pinned authority");
    const nlohmann::json foreignLegacy = {{"magic", "LOLBackupV2 mirror absences"}, {"version", 2},
                                         {"root", WideToUtf8(cache)}, {"absent", {"saves\\old-intent.dat"}}};
    const auto legacyBytes = foreignLegacy.dump();
    Put(cache + L"\\.lolbackup-sync-state-v2.json", legacyBytes);
    core::SyncEngine engine;
    engine.cacheRoot = cache;
    engine.gameRoot = Child(env, L"foreign-game");
    engine.managementRoot = management;
    engine.cacheAuthority = &held;
    Put(cache + L"\\saves\\old-intent.dat", "CURRENT-PHYSICAL-CACHE");
    Put(engine.gameRoot + L"\\saves\\old-intent.dat", "CURRENT-PHYSICAL-CACHE");
    CHECK(engine.PrepareManagement(error),
          "direct preparation validates ordinary cache without metadata migration");
    const auto restored = engine.RestoreDetailed();
    CHECK(restored.success && restored.copied == 1 && restored.skipped == 0 &&
              Get(engine.gameRoot + L"\\saves\\old-intent.dat") == "CURRENT-PHYSICAL-CACHE",
          "physical cached file is copied even when old absence metadata lists it and Game bytes match");
    CHECK(Get(cache + L"\\.lolbackup-sync-state-v2.json") == legacyBytes &&
              !fileops::Exists(selected.directory + L"\\mirror-state-v2.json") &&
              !fileops::Exists(selected.directory + L"\\foreign-legacy-sync-state-v2.json") &&
              !fileops::Exists(cache + L"\\.lolbackup-owned-v1") &&
              Get(journal) == "OLD-DELETION-INTENT-BYTES" &&
              Get(cache + L"\\personal.txt") == "NEW-CACHE-OBJECT",
          "direct engine ignores old markers in place and creates no owner, journal or archived metadata");
    held.Reset();
    Put(owner, "BROKEN-OR-UNKNOWN-OWNER");
    CHECK(!core::rootstore::Resolve(management, cache, reopened, error, true, true) &&
              error.find(L"[E_ROOT_OWNER_INVALID]") != std::wstring::npos &&
              Get(owner) == "BROKEN-OR-UNKNOWN-OWNER" && Get(journal) == "OLD-DELETION-INTENT-BYTES",
          "confirmation cannot bypass a malformed base owner or silently overwrite it");
}
void TestLocalRootAndOrphanRecordPublication(const Sandbox& env) {
    std::wstring error;
    CHECK(core::rootstore::ValidateLocalRoot(Child(env, L"local-selected"), error),
          "local fixed or removable-volume directories remain supported without fixed folder names");
    CHECK(!core::rootstore::ValidateLocalRoot(L"\\\\invalid-server\\invalid-share\\cache", error) &&
              error.find(L"[E_ROOT_LOCAL_ONLY]") == 0,
          "network cache is explicitly rejected before network IO instead of claiming cross-PC exclusion");
    const auto cache = Child(env, L"pending-record-cache"), management = Child(env, L"pending-record-management"),
               orphan = management + L"\\.pending-root-record-crashed-fixture";
    Put(cache + L"\\notes.txt", "USER-CONTENT");
    CHECK(fileops::PrepareDedicatedRoot(management, error) && fileops::PrepareDedicatedRoot(orphan, error),
          "isolated crash-orphan marker directory is installed without a final owner");
    Put(orphan + L"\\owner.json", "PARTIAL-CRASH-OWNER");
    core::rootstore::Record record;
    CHECK(core::rootstore::Resolve(management, cache, record, error) &&
              Get(orphan + L"\\owner.json") == "PARTIAL-CRASH-OWNER" &&
              Get(record.directory + L"\\owner.json").find("physicalIdentity") != std::string::npos &&
              Get(cache + L"\\notes.txt") == "USER-CONTENT",
          "orphan pending records never poison or get adopted by a fresh verified atomic final publication");
    const auto displaced = management + L"\\.pending-root-record-completed-fixture";
    CHECK(displaced.rfind(env.root + L"\\", 0) == 0 && record.directory.rfind(env.root + L"\\", 0) == 0 &&
              MoveFileW(L(record.directory).c_str(), L(displaced).c_str()) &&
              fileops::PrepareDedicatedRoot(record.directory, error),
          "simulate only a known sandbox final marker without owner and retain full displaced record");
    core::rootstore::Record denied;
    CHECK(!core::rootstore::Resolve(management, cache, denied, error, true, true) &&
              !fileops::Exists(record.directory + L"\\owner.json") &&
              Get(displaced + L"\\owner.json").find("physicalIdentity") != std::string::npos,
          "unknown final owner is never silently initialized or repaired by explicit identity confirmation");
}
} // namespace

int main() {
    try {
        Sandbox env;
        std::printf("Dedicated test sandbox: %ls\n", env.root.c_str());
        TestSelectedRoot(env);
        TestExplicitForeignIdentityAuthorization(env);
        TestLocalRootAndOrphanRecordPublication(env);
        TestBusinessInventory(env);
        std::wstring error, normalized;
        CHECK(!pathpolicy::ValidateRoot(L"", error), "empty root rejected");
        CHECK(!pathpolicy::ValidateRoot(L"relative\\path", error), "relative root rejected");
        CHECK(!pathpolicy::ValidateRoot(L"C:relative", error), "drive-relative root rejected");
        CHECK(!pathpolicy::ValidateRoot(L"C:\\", error), "drive root rejected");
        CHECK(!pathpolicy::ValidateRoot(L"\\\\server\\share", error),
              "UNC share root rejected without network I/O");
        CHECK(!pathpolicy::ValidateRoot(L"\\\\.\\PhysicalDrive0", error), "device namespace rejected");
        CHECK(!pathpolicy::ValidateRoot(Child(env, L"ambiguous. "), error), "trailing dot/space rejected");
        CHECK(!pathpolicy::ValidateRoot(Child(env, L"a\\..\\b"), error), "parent traversal rejected");
        CHECK(!pathpolicy::ValidateRoot(Child(env, L"NUL.txt"), error), "reserved DOS name rejected");
        std::wstring nul = Child(env, L"prefix");
        nul.push_back(L'\0');
        nul += L"\\suffix";
        CHECK(!pathpolicy::ValidateRoot(nul, error), "embedded NUL rejected by root policy");
        CHECK(!fileops::EnsureDir(nul) && !fileops::Exists(nul) && fileops::FileSize(nul) == -1,
              "legacy I/O rejects NUL too");
        std::wstring slash = Child(env, L"forward\\new");
        std::replace(slash.begin(), slash.end(), L'\\', L'/');
        CHECK(pathpolicy::NormalizeRoot(slash + L"/", normalized, error) &&
                  normalized == Child(env, L"forward\\new"),
              "forward slash and trailing slash normalize");
        auto inPlace = Child(env, L"in-place");
        CHECK(pathpolicy::NormalizeRoot(inPlace, inPlace, error) && !inPlace.empty(),
              "normalization supports in-place output");
        CHECK(pathpolicy::AreDisjoint(Child(env, L"a"), Child(env, L"ab"), error),
              "component boundary distinguishes sibling prefix");
        CHECK(!pathpolicy::AreDisjoint(Child(env, L"a"), Child(env, L"a\\b"), error),
              "nonexistent ancestor overlap rejected");
        CHECK(!pathpolicy::AreDisjoint(env.root, env.root, error), "same directory rejected");

        auto leasedRoot = Child(env, L"leased-root"), movedRoot = leasedRoot + L"-moved";
        Own(leasedRoot);
        fileops::DirectoryLease lease;
        CHECK(lease.Acquire(leasedRoot, error, true), "persistent directory lease pins an owned root");
        CHECK(!MoveFileExW(L(leasedRoot).c_str(), L(movedRoot).c_str(), 0),
              "held directory lease blocks root rename");
        CHECK(!DeleteFileW(L(leasedRoot + L"\\.lolbackup-owned-v1").c_str()),
              "dedicated lease also pins ownership marker");
        CHECK(!lease.Acquire(L"relative", error), "failed lease acquisition does not replace existing lease");
        fileops::DirectoryLease moved(std::move(lease));
        lease.Reset();
        CHECK(!MoveFileExW(L(leasedRoot).c_str(), L(movedRoot).c_str(), 0),
              "move retains pin and failed Acquire preserved old root");
        fileops::DirectoryLease assigned;
        assigned = std::move(moved);
        assigned.Reset();
        CHECK(MoveFileExW(L(leasedRoot).c_str(), L(movedRoot).c_str(), 0),
              "Reset releases pins for deliberate directory rename");

        // No marker or open child file may accidentally make these assertions
        // pass. Attributes-only directory handles did NOT stop MoveFileW.
        auto plainParent = Child(env, L"plain-lease-parent");
        auto plainNested = plainParent + L"\\nested";
        auto renamedNested = plainNested + L"-renamed";
        auto renamedParent = plainParent + L"-renamed";
        CHECK(fileops::EnsureDir(plainNested), "unmarked nested directory fixture exists");
        CHECK(MoveFileW(L(plainNested).c_str(), L(renamedNested).c_str()),
              "unmarked nested directory is normally renamable before lease");
        if (!MoveFileW(L(renamedNested).c_str(), L(plainNested).c_str()))
            throw std::runtime_error("restore plain rename baseline");
        fileops::DirectoryLease plainLease;
        CHECK(plainLease.Acquire(plainNested, error), "lease pins a nested directory without any marker");
        BOOL nestedRenamed = MoveFileW(L(plainNested).c_str(), L(renamedNested).c_str());
        CHECK(!nestedRenamed, "unmarked nested directory rename is denied by directory handle alone");
        if (nestedRenamed && !MoveFileW(L(renamedNested).c_str(), L(plainNested).c_str()))
            throw std::runtime_error("restore failed nested pin fixture");
        BOOL parentRenamed = MoveFileW(L(plainParent).c_str(), L(renamedParent).c_str());
        CHECK(!parentRenamed, "unmarked ancestor rename is denied by full-chain pins alone");
        if (parentRenamed && !MoveFileW(L(renamedParent).c_str(), L(plainParent).c_str()))
            throw std::runtime_error("restore failed ancestor pin fixture");
        {
            winutil::ScopedHandle writer(CreateFileW(
                L(plainNested).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            CHECK(writer.valid(),
                  "directory lease allows legitimate parent writers; reparse safety is handle-relative");
        }
        plainLease.Reset();
        CHECK(MoveFileW(L(plainNested).c_str(), L(renamedNested).c_str()),
              "unmarked nested directory becomes renamable after lease reset");

        // A real callback mutates an already-pinned EMPTY destination into a
        // junction, rather than merely checking that a writable open is denied.
        auto attackParent = Child(env, L"callback-target");
        auto attackRenamed = attackParent + L"-renamed";
        auto attackOutside = Child(env, L"callback-external");
        auto attackSource = Child(env, L"callback-source\\data");
        Put(attackSource, "ATTACK-INPUT");
        Put(attackOutside + L"\\data", "EXTERNAL-SENTINEL");
        CHECK(fileops::EnsureDir(attackParent), "callback attack fixture created");
        bool attackAttempted = false, injected = false;
        fileops::FileError attackError;
        auto attack = [&] {
            if (attackAttempted)
                return false;
            if (MoveFileW(L(attackParent).c_str(), L(attackRenamed).c_str())) {
                if (!MoveFileW(L(attackRenamed).c_str(), L(attackParent).c_str()))
                    throw std::runtime_error("restore callback probe rename");
                return false;
            }
            if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_ACCESS_DENIED)
                return false;
            attackAttempted = true;
            injected = SetJunction(attackParent, attackOutside);
            return false;
        };
        bool attackCopied =
            fileops::CopyFileChecked(attackSource, attackParent + L"\\data", attackError, attack);
        CHECK(attackAttempted && injected,
              "callback actually installs a junction after destination pin acquisition");
        CHECK(!attackCopied && attackError.code != ERROR_SUCCESS,
              "copy observes mutated parent tag and refuses commit");
        CHECK(Get(attackOutside + L"\\data") == "EXTERNAL-SENTINEL" && RawTempCount(attackOutside) == 0,
              "callback junction cannot redirect staging or overwrite external sentinel");
        if (injected && !RemoveDirectoryW(L(attackParent).c_str()))
            throw std::runtime_error("remove callback junction");

        auto metadata = Child(env, L"metadata");
        CHECK(fileops::EnsureDir(metadata), "metadata helper directory created");
        HANDLE raw = INVALID_HANDLE_VALUE;
        fileops::FileError metadataError;
        CHECK(fileops::OpenFileChecked(metadata + L"\\pending", raw, GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                                       CREATE_NEW, metadataError),
              "checked metadata create transfers handle");
        winutil::ScopedHandle pending(raw);
        DWORD metaWritten = 0;
        CHECK(pending.valid() && WriteFile(pending.get(), "META", 4, &metaWritten, nullptr) &&
                  metaWritten == 4 && FlushFileBuffers(pending.get()),
              "metadata staged bytes flush successfully");
        CHECK(fileops::RenameHandleChecked(pending.get(), metadata + L"\\committed", metadataError, false),
              "checked metadata commit uses fixed parent handle");
        pending.reset();
        CHECK(Get(metadata + L"\\committed") == "META", "metadata commit preserves staged contents");
        CHECK(!fileops::OpenFileChecked(metadata + L"\\committed", raw, GENERIC_WRITE, FILE_SHARE_READ,
                                        CREATE_ALWAYS, metadataError) &&
                  Get(metadata + L"\\committed") == "META",
              "checked metadata open refuses truncate-before-validation");
        CHECK(fileops::EnsureDir(metadata + L"\\stage"), "directory commit staging fixture created");
        CHECK(fileops::MoveDirectoryChecked(metadata + L"\\stage", metadata + L"\\generation", metadataError),
              "checked directory commit moves only the intended directory");
        CHECK(fileops::RemoveEmptyDirectoryChecked(metadata + L"\\generation", metadataError),
              "checked empty-directory cleanup succeeds by handle");
        CHECK(!fileops::RemoveEmptyDirectoryChecked(metadata, metadataError) &&
                  metadataError.code == ERROR_DIR_NOT_EMPTY,
              "checked directory cleanup never removes a nonempty tree");

        const auto src = Child(env, L"source"), dst = Child(env, L"target");
        Put(src + L"\\foo.txt", "NEW");
        Own(dst);
        Put(dst + L"\\FOO.TXT", "OLD");
        Put(dst + L"\\extra.txt", "STALE");
        auto mirrored = fileops::MirrorDir(src, dst);
        CHECK(mirrored.complete && mirrored.copied == 1 && mirrored.deleted == 1 && mirrored.errors.empty(),
              "case-insensitive mirror copies and deletes exact counts");
        CHECK(Get(dst + L"\\FOO.TXT") == "NEW", "case-only destination spelling is not deleted");
        CHECK(fileops::CountFiles(dst) == 1 && fileops::ListFiles(dst).size() == 1,
              "ownership marker is not business content");
        CHECK(fileops::IsDedicatedRoot(dst, error), "mirror preserves ownership marker");
        CHECK(fileops::ProbeWritable(dst, error) && RawTempCount(dst) == 0,
              "write flush close delete probe leaves no temp");
        CHECK(fileops::PrepareDedicatedRoot(dst, error), "existing owned nonempty root can be reused");
        auto unknown = Child(env, L"unknown");
        Put(unknown + L"\\personal.txt", "PERSONAL");
        CHECK(!fileops::PrepareDedicatedRoot(unknown, error) && GetLastError() == ERROR_DIR_NOT_EMPTY,
              "nonempty unowned directory cannot be claimed");
        auto refused = fileops::MirrorDir(src, unknown);
        CHECK(!refused.complete && !refused.errors.empty() && refused.deleted == 0 &&
                  Get(unknown + L"\\personal.txt") == "PERSONAL",
              "mirror refuses arbitrary target without destruction");
        auto empty = Child(env, L"empty");
        fileops::EnsureDir(empty);
        auto skippedEmpty = fileops::MirrorDir(empty, dst);
        CHECK(skippedEmpty.skipped && skippedEmpty.deleted == 0 && Get(dst + L"\\foo.txt") == "NEW",
              "complete empty source retains cache");
        auto missing = fileops::ListFilesChecked(Child(env, L"missing"));
        CHECK(!missing.complete && !missing.errors.empty(),
              "missing source differs from complete empty enumeration");
        CHECK(fileops::ListFilesChecked(empty).complete, "real empty directory is complete");
        CHECK(!fileops::MirrorDir(src, src).complete && !fileops::MirrorDir(src, src + L"\\child").complete,
              "same and descendant mirror roots rejected");

        // Cancellation before any work and at the destructive boundary.
        Put(dst + L"\\cancel-keep.txt", "KEEP");
        auto cancelled = fileops::MirrorDir(src, dst, [](int, int, const std::wstring&) { return false; });
        CHECK(cancelled.cancelled && cancelled.copied == 0 && cancelled.deleted == 0,
              "immediate cancellation performs no work");
        CHECK(Get(dst + L"\\cancel-keep.txt") == "KEEP", "copy cancellation never deletes target-only file");
        auto deleteCancelled = fileops::MirrorDir(
            src, dst, [](int, int, const std::wstring& cur) { return cur.compare(0, 9, L"[delete] ") != 0; });
        CHECK(deleteCancelled.cancelled && deleteCancelled.deleted == 0 &&
                  Get(dst + L"\\cancel-keep.txt") == "KEEP",
              "deletion-phase cancellation stops before first deletion");
        auto callbackThrows = fileops::MirrorDir(
            src, dst, [](int, int, const std::wstring&) -> bool { throw std::runtime_error("callback"); });
        CHECK(!callbackThrows.complete && !callbackThrows.errors.empty() && callbackThrows.deleted == 0,
              "callback exceptions become observable error, never escape");
        auto listCancelled = fileops::ListFilesChecked(src, [] { return true; });
        CHECK(listCancelled.cancelled && !listCancelled.complete, "typed enumeration reports cancellation");

        // An exclusive child directory handle simulates unreadable/inaccessible
        // subtree without touching machine ACLs or requiring administrator rights.
        auto partialSrc = Child(env, L"partial-source"), partialDst = Child(env, L"partial-target");
        Put(partialSrc + L"\\visible.dat", "VISIBLE");
        Put(partialSrc + L"\\blocked\\hidden.dat", "HIDDEN");
        Own(partialDst);
        Put(partialDst + L"\\old.dat", "OLD");
        {
            winutil::ScopedHandle block(CreateFileW(L(partialSrc + L"\\blocked").c_str(), GENERIC_READ, 0,
                                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                                    nullptr));
            CHECK(block.valid(), "exclusive subtree fixture acquired");
            auto partial = fileops::ListFilesChecked(partialSrc);
            CHECK(!partial.complete && !partial.errors.empty() && !partial.files.empty(),
                  "partial nonempty inventory is explicitly incomplete");
            auto r = fileops::MirrorDir(partialSrc, partialDst);
            CHECK(!r.complete && r.deleted == 0 && Get(partialDst + L"\\old.dat") == "OLD",
                  "incomplete source inventory suppresses all deletion");
        }
        {
            winutil::ScopedHandle block(CreateFileW(L(dst + L"\\foo.txt").c_str(), GENERIC_READ,
                                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                                    FILE_ATTRIBUTE_NORMAL, nullptr));
            Put(src + L"\\foo.txt", "REPLACEMENT");
            fileops::FileError e;
            CHECK(!fileops::CopyFileChecked(src + L"\\foo.txt", dst + L"\\foo.txt", e) &&
                      e.code != ERROR_SUCCESS,
                  "locked atomic commit reports exact Win32 failure");
            CHECK(Get(dst + L"\\foo.txt") == "NEW", "failed atomic replacement preserves complete old file");
            auto r = fileops::MirrorDir(src, dst);
            CHECK(!r.complete && r.deleted == 0 && !r.errors.empty(), "copy failure prevents deletion phase");
        }
        CHECK(RawTempCount(dst) == 0, "failed copies remove their temporary file by handle");
        Put(src + L"\\large.bin", std::string(2 * 1024 * 1024, 'Z'));
        Put(dst + L"\\large.bin", "OLD-LARGE");
        int polls = 0;
        fileops::FileError copyError;
        CHECK(!fileops::CopyFileChecked(src + L"\\large.bin", dst + L"\\large.bin", copyError,
                                        [&] { return ++polls >= 7; }) &&
                  copyError.code == ERROR_CANCELLED,
              "large-file cancellation is polled between I/O chunks");
        CHECK(Get(dst + L"\\large.bin") == "OLD-LARGE" && RawTempCount(dst) == 0,
              "cancelled staged copy preserves old target and cleans staging");
        Put(Child(env, L"prefix"), "NUL-PROTECTED");
        CHECK(!fileops::CopyFileChecked(src + L"\\foo.txt", nul, copyError) &&
                  Get(Child(env, L"prefix")) == "NUL-PROTECTED",
              "NUL never truncates destination to prefix file");
        CHECK(!fileops::DeleteFileChecked(nul, copyError) && Get(Child(env, L"prefix")) == "NUL-PROTECTED",
              "NUL never truncates delete target");
        CHECK(fileops::DeleteFileChecked(Child(env, L"absent-file"), copyError),
              "deleting absent file succeeds");

        auto conflictSrc = Child(env, L"conflict-source"), conflictDst = Child(env, L"conflict-target");
        Put(conflictSrc + L"\\a", "FILE");
        Own(conflictDst);
        Put(conflictDst + L"\\a\\precious", "KEEP");
        auto conflict = fileops::MirrorDir(conflictSrc, conflictDst);
        CHECK(!conflict.complete && conflict.busy == 0 && !conflict.errors.empty(),
              "directory-to-file conflict is not mislabeled busy");
        CHECK(conflict.copied == 0 && conflict.deleted == 0 && Get(conflictDst + L"\\a\\precious") == "KEEP",
              "directory-to-file conflict preserves entire old subtree");
        fileops::FileError e;
        CHECK(!fileops::CopyFileChecked(conflictSrc + L"\\a", conflictDst + L"\\a", e),
              "single-file copy refuses directory target");
        auto restore = Child(env, L"restore");
        Put(restore + L"\\unrelated", "KEEP");
        auto copied = fileops::CopyTreeChecked(conflictSrc, restore);
        CHECK(copied.complete && copied.copied == 1 && copied.deleted == 0 &&
                  Get(restore + L"\\unrelated") == "KEEP",
              "CopyTreeChecked restores to existing unmarked directory without deletion");

        auto longSrc = Child(env, L"long-source"), longDst = Child(env, L"long-target");
        std::wstring relative;
        for (int i = 0; i != 8; ++i)
            relative += L"segment-012345678901234567890123456789\\";
        relative += L"data.bin";
        Put(longSrc + L"\\" + relative, "LONG-PATH");
        Own(longDst);
        auto longResult = fileops::MirrorDir(longSrc, longDst);
        CHECK(longResult.complete && Get(longDst + L"\\" + relative) == "LONG-PATH",
              "all mirror I/O supports paths beyond MAX_PATH");
        CHECK(fileops::FileReadable(longDst + L"\\" + relative) &&
                  fileops::FileSize(longDst + L"\\" + relative) == 9,
              "long-path size and full readability use native long paths");
        CHECK(fileops::DeleteFileChecked(longDst + L"\\" + relative, e), "long-path deletion works");
        std::vector<wchar_t> shortBuffer(32768);
        DWORD shortN =
            GetShortPathNameW(L(src).c_str(), shortBuffer.data(), static_cast<DWORD>(shortBuffer.size()));
        if (shortN && shortN < shortBuffer.size()) {
            CHECK(!pathpolicy::AreDisjoint(std::wstring(shortBuffer.data(), shortN), src, error),
                  "short-name alias cannot bypass root equality check");
        } else {
            ++skipped;
            std::printf("[SKIP] filesystem has no short-name alias\n");
        }

        // Reparse points target ONLY other directories in this same sandbox.
        auto outside = Child(env, L"external"), reparseSrc = Child(env, L"reparse-source"),
             reparseDst = Child(env, L"reparse-target");
        Put(outside + L"\\protected.txt", "EXTERNAL");
        Put(reparseSrc + L"\\ok", "OK");
        Own(reparseDst);
        std::wstring link = reparseSrc + L"\\loop";
        BOOL linked =
            CreateSymbolicLinkW(L(link).c_str(), L(reparseSrc).c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2);
        if (!linked && GetLastError() == ERROR_INVALID_PARAMETER)
            linked =
                CreateSymbolicLinkW(L(link).c_str(), L(reparseSrc).c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY);
        if (linked) {
            auto r = fileops::ListFilesChecked(reparseSrc);
            CHECK(!r.complete && !r.errors.empty(),
                  "self-referential reparse directory is refused without recursion");
            Put(reparseDst + L"\\keep", "KEEP");
            CHECK(fileops::MirrorDir(reparseSrc, reparseDst).deleted == 0 &&
                      Get(reparseDst + L"\\keep") == "KEEP",
                  "source reparse failure cannot trigger deletion");
            CHECK(!pathpolicy::ValidateRoot(link, error), "root symlink is rejected");
            RemoveDirectoryW(L(link).c_str());
            std::wstring externalLink = reparseDst + L"\\external-link";
            linked = CreateSymbolicLinkW(L(externalLink).c_str(), L(outside).c_str(),
                                         SYMBOLIC_LINK_FLAG_DIRECTORY);
            if (linked) {
                auto mirror = fileops::MirrorDir(reparseSrc, reparseDst);
                CHECK(!mirror.complete && mirror.copied == 0 &&
                          Get(outside + L"\\protected.txt") == "EXTERNAL",
                      "target external link is rejected before copying or deletion");
                CHECK(!fileops::CopyFileChecked(src + L"\\foo.txt", externalLink + L"\\protected.txt", e) &&
                          Get(outside + L"\\protected.txt") == "EXTERNAL",
                      "copy cannot follow reparse ancestor to external content");
            }
        } else {
            ++skipped;
            std::printf("[SKIP] symlink privilege unavailable (Win32=%lu)\n", GetLastError());
        }
        auto unknownCleanup = Child(env, L"unknown-cleanup");
        Put(unknownCleanup + L"\\.~lolbackup-not-ours.txt", "PERSONAL");
        CHECK(!fileops::RemoveIfEmptyDedicatedRoot(unknownCleanup, error) &&
                  Get(unknownCleanup + L"\\.~lolbackup-not-ours.txt") == "PERSONAL",
              "cleanup prefix is never proof of ownership for unknown user files");
        Put(unknownCleanup + L"\\config.ini", "BUSINESS");
        CHECK(!fileops::ClaimBusinessCacheRoot(unknownCleanup, error) &&
                  !fileops::Exists(unknownCleanup + L"\\.lolbackup-owned-v1") &&
                  Get(unknownCleanup + L"\\.~lolbackup-not-ours.txt") == "PERSONAL",
              "business claim rechecks physical unknown metadata instead of trusting a filtered caller list");
        auto forgedCleanup = Child(env, L"forged-cleanup");
        Put(forgedCleanup + L"\\.lolbackup-owned-v1", "FORGED");
        Put(forgedCleanup + L"\\.~lolbackup-service.lock", "");
        CHECK(!fileops::RemoveIfEmptyDedicatedRoot(forgedCleanup, error) &&
                  Get(forgedCleanup + L"\\.lolbackup-owned-v1") == "FORGED" &&
                  fileops::Exists(forgedCleanup + L"\\.~lolbackup-service.lock"),
              "cleanup validates every entry and marker before any mutation");
        auto trueCleanup = Child(env, L"true-cleanup");
        Own(trueCleanup);
        Put(trueCleanup + L"\\.~lolbackup-service.lock", "");
        CHECK(fileops::RemoveIfEmptyDedicatedRoot(trueCleanup, error) && !fileops::IsDir(trueCleanup),
              "genuine empty owned leftover removes the exact pinned object by handle");
        auto linkedCleanup = Child(env, L"linked-cleanup");
        auto externalMarker = Child(env, L"external-marker");
        Put(externalMarker, "LOLBackup dedicated mirror root v1\r\n");
        CHECK(fileops::EnsureDir(linkedCleanup) &&
                  CreateHardLinkW(L(linkedCleanup + L"\\.lolbackup-owned-v1").c_str(),
                                  L(externalMarker).c_str(), nullptr),
              "create isolated hardlinked cleanup marker");
        const DWORD markerAttrs = GetFileAttributesW(L(externalMarker).c_str());
        CHECK(!fileops::RemoveIfEmptyDedicatedRoot(linkedCleanup, error) &&
                  Get(externalMarker) == "LOLBackup dedicated mirror root v1\r\n" &&
                  GetFileAttributesW(L(externalMarker).c_str()) == markerAttrs,
              "hardlinked ownership marker is refused without external content or attribute mutation");
        auto clearRoot = Child(env, L"managed-clear");
        Put(clearRoot + L"\\saves\\a.dat", "A");
        Put(clearRoot + L"\\saves\\.~lolbackup-personal", "UNKNOWN");
        auto clearResult = fileops::ClearTreeEntriesChecked(clearRoot, {L"saves\\a.dat"}, {L"saves"});
        CHECK(!clearResult.complete && clearResult.deleted == 0 &&
                  Get(clearRoot + L"\\saves\\a.dat") == "A" &&
                  Get(clearRoot + L"\\saves\\.~lolbackup-personal") == "UNKNOWN",
              "managed cleanup refuses filtered unknown content before deleting planned files");
        DeleteFileW(L(clearRoot + L"\\saves\\.~lolbackup-personal").c_str());
        Put(clearRoot + L"\\saves\\nested\\b.dat", "B");
        bool swapAttempted = false, swapped = false;
        clearResult = fileops::ClearTreeEntriesChecked(
            clearRoot, {L"saves\\a.dat", L"saves\\nested\\b.dat"}, {L"saves", L"saves\\nested"}, nullptr,
            [&](const std::wstring&) {
                if (!swapAttempted) {
                    swapAttempted = true;
                    swapped = MoveFileW(L(clearRoot + L"\\saves\\nested").c_str(),
                                        L(clearRoot + L"\\moved").c_str()) != FALSE;
                }
            });
        CHECK(clearResult.complete && clearResult.deleted == 2 && swapAttempted && !swapped &&
                  !fileops::IsDir(clearRoot + L"\\saves"),
              "whole cleanup pins every ordinary directory identity through exact-object file and directory "
              "deletion");
        auto externalLinked = Child(env, L"external-hardlink-file");
        auto managedLinked = Child(env, L"managed-hardlink-file");
        Put(externalLinked, "EXTERNAL-LINK");
        CHECK(CreateHardLinkW(L(managedLinked).c_str(), L(externalLinked).c_str(), nullptr),
              "create isolated managed/external hardlink aliases");
        SetFileAttributesW(L(externalLinked).c_str(), FILE_ATTRIBUTE_READONLY);
        const DWORD linkedAttrs = GetFileAttributesW(L(externalLinked).c_str());
        CHECK(!fileops::DeleteFileChecked(managedLinked, e) && fileops::Exists(managedLinked) &&
                  GetFileAttributesW(L(externalLinked).c_str()) == linkedAttrs &&
                  Get(externalLinked) == "EXTERNAL-LINK",
              "hardlinked deletion fails before clearing shared read-only attributes");
        CHECK(!fileops::CopyFileChecked(externalLinked, Child(env, L"hardlink-copy-target"), e),
              "hardlinked source cannot import outside shared content into managed data");
        auto invalidMarker = Child(env, L"invalid-marker");
        Put(invalidMarker + L"\\.lolbackup-owned-v1", "FORGED");
        CHECK(!fileops::PrepareDedicatedRoot(invalidMarker, error),
              "malformed ownership marker is never accepted");
    } catch (const std::exception& ex) {
        ++failed;
        std::printf("[FAIL] fixture exception: %s (Win32=%lu)\n", ex.what(), GetLastError());
    } catch (...) {
        ++failed;
        std::printf("[FAIL] unexpected test exception\n");
    }
    std::printf("file safety: %d passed, %d failed, %d conditional skips\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
