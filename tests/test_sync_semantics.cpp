// Direct engine semantics: full empty mirrors, real Win32 failures, cancellation and managed scope.
#include "../src/core/file_ops.h"
#include "../src/core/logger.h"
#include "../src/core/sync_engine.h"
#include "test_isolation.h"
#include "test_fixture.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
#include <winioctl.h>

namespace {
int passed = 0, failed = 0;

// Keep release assertions active and report each intended policy expectation.
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    condition ? ++passed : ++failed;
}

// Ordinary fixture creation deliberately does not claim cache ownership.
void Put(const std::wstring& path, const std::string& bytes) {
    if (!fileops::EnsureDir(path.substr(0, path.find_last_of(L'\\'))))
        throw std::runtime_error("Cannot create fixture parent");
    winutil::ScopedHandle file(CreateFileW(testutil::Native(path).c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    if (!file.valid() || !WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                                    &written, nullptr) || written != bytes.size())
        throw std::runtime_error("Cannot write fixture bytes");
}

// Both roots are isolated ordinary directories, not dedicated/owned file_ops roots.
core::SyncEngine Engine(const std::wstring& root) {
    core::SyncEngine engine;
    engine.gameRoot = root + L"\\game";
    engine.cacheRoot = root + L"\\cache";
    if (!fileops::EnsureDir(engine.gameRoot) || !fileops::EnsureDir(engine.cacheRoot))
        throw std::runtime_error("Cannot create ordinary roots");
    return engine;
}

// Junctions need no symlink privilege; both link and target stay inside this test's isolated root.
bool SetJunction(const std::wstring& directory, const std::wstring& target) {
    const std::wstring substitute = L"\\??\\" + target;
    const size_t textBytes = (substitute.size() + 1 + target.size() + 1) * sizeof(wchar_t);
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
        testutil::Native(directory).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    DWORD returned = 0;
    return writer.valid() && DeviceIoControl(writer.get(), FSCTL_SET_REPARSE_POINT, bytes.data(),
                                             static_cast<DWORD>(bytes.size()), nullptr, 0,
                                             &returned, nullptr) != FALSE;
}

// Absence groups and complete empty Game are intentional deletions, not protective no-ops.
void TestFullAndEmptyMirrors() {
    testutil::Directory directory(L"direct-full-empty");
    auto engine = Engine(directory.path());
    Put(engine.gameRoot + L"\\config.ini", "NEW-CONFIG");
    Put(engine.cacheRoot + L"\\saves\\old.dat", "OLD-SAVE");
    Put(engine.cacheRoot + L"\\shards\\old.dat", "OLD-SHARD");
    Put(engine.cacheRoot + L"\\user.ini", "OLD-ROOT");
    Put(engine.cacheRoot + L"\\unrelated\\note.txt", "KEEP");
    const auto first = engine.RunOnce(true, false, false, false);
    Check(first.success && first.copied == 1 && first.deleted == 3 && first.sourceNonempty,
          "default non-copyOnly mirror deletes all cache-only groups, not merely active groups");
    Check(DeleteFileW(testutil::Native(engine.gameRoot + L"\\config.ini").c_str()) != FALSE,
          "Game becomes physically empty before destructive empty-source regression");
    const wchar_t* names[] = {
        L"bot_5.dll", L"config.ini", L"hc.dat", L"HID.dll", L"shards.json", L"user.ini"};
    for (const auto* name : names)
        Put(engine.cacheRoot + L"\\" + name, "OLD-ROOT");
    Put(engine.cacheRoot + L"\\saves\\old.dat", "OLD-SAVE");
    Put(engine.cacheRoot + L"\\shards\\old.dat", "OLD-SHARD");
    const auto empty = engine.RunOnce(true, false, false, true);
    const auto cache = fileops::ListBusinessFilesChecked(engine.cacheRoot);
    Check(empty.ran && empty.success && !empty.sourceNonempty && empty.copied == 0 &&
              empty.deleted == 8 && cache.complete && cache.files.empty(),
          "complete empty Game deletes every managed cached file including all six root names");
    Check(testutil::ReadBytes(engine.cacheRoot + L"\\unrelated\\note.txt") == "KEEP",
          "complete empty mirror never deletes outside the managed whitelist");
    Put(engine.cacheRoot + L"\\saves\\manual-preserve.dat", "PRESERVE");
    const auto backup = engine.RunOnce(false, true, true, true);
    Check(backup.success && !backup.sourceNonempty && backup.copied == 0 && backup.deleted == 0 &&
              testutil::ReadBytes(engine.cacheRoot + L"\\saves\\manual-preserve.dat") == "PRESERVE",
          "manual copyOnly backup of complete empty Game succeeds without deleting any cache file");
}

// Native copy/delete errors are counted independently; later operations still run.
void TestErrorsContinue() {
    testutil::Directory directory(L"direct-error-continuation");
    auto engine = Engine(directory.path());
    Put(engine.gameRoot + L"\\config.ini", "NEW-CONFIG");
    Put(engine.gameRoot + L"\\hc.dat", "NEW-HC");
    Put(engine.cacheRoot + L"\\config.ini", "OLD-CONFIG");
    Put(engine.cacheRoot + L"\\hc.dat", "OLD-HC");
    Put(engine.cacheRoot + L"\\saves\\stale.dat", "DELETE-ME");
    Put(engine.cacheRoot + L"\\user.ini", "READONLY-DELETE");
    Check(SetFileAttributesW(testutil::Native(engine.cacheRoot + L"\\config.ini").c_str(),
                             FILE_ATTRIBUTE_READONLY) != FALSE &&
              SetFileAttributesW(testutil::Native(engine.cacheRoot + L"\\user.ini").c_str(),
                                 FILE_ATTRIBUTE_READONLY) != FALSE,
          "read-only native copy and delete failures are installed");
    const auto result = engine.RunOnce(true);
    Check(!result.success && !result.cancelled && result.partial && result.copied == 1 &&
              result.deleted == 1 && result.errors == 2 && result.errorDetails.size() == 2,
          "failed copy and failed deletion count exactly, without suppressing other work");
    Check(testutil::ReadBytes(engine.cacheRoot + L"\\config.ini") == "OLD-CONFIG" &&
              testutil::ReadBytes(engine.cacheRoot + L"\\hc.dat") == "NEW-HC" &&
              !fileops::Exists(engine.cacheRoot + L"\\saves\\stale.dat") &&
              testutil::ReadBytes(engine.cacheRoot + L"\\user.ini") == "READONLY-DELETE",
          "copy error still allows remaining native copy and independent managed deletion");
    Check(result.summary.find(L"expected=") != std::wstring::npos &&
              result.summary.find(L"actual=Win32 5") != std::wstring::npos &&
              result.summary.find(L"location=") != std::wstring::npos &&
              engine.progressDone < engine.progressTotal,
          "error summary reports expected/actual/location and failure never manufactures 100 percent");
}

// Sharing violations are real errors during restore, not silently skipped identical/busy files.
void TestRestoreBusyContinues() {
    testutil::Directory directory(L"direct-restore-busy");
    auto engine = Engine(directory.path());
    Put(engine.cacheRoot + L"\\config.ini", "NEW-CONFIG");
    Put(engine.cacheRoot + L"\\hc.dat", "NEW-HC");
    Put(engine.gameRoot + L"\\config.ini", "OLD-CONFIG");
    winutil::ScopedHandle locked(CreateFileW(testutil::Native(engine.gameRoot + L"\\config.ini").c_str(),
                                            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(locked.valid(), "Game destination denies native write sharing");
    const auto result = engine.RestoreDetailed();
    Check(!result.success && result.partial && result.errors == 1 && result.copied == 1 &&
              result.summary.find(L"Win32 32") != std::wstring::npos &&
              testutil::ReadBytes(engine.gameRoot + L"\\hc.dat") == "NEW-HC",
          "native sharing violation is reported and restore still copies the remaining source file");
}

// Cancellation at a per-file label preserves earlier native successes and stops later work/deletes.
void TestPhaseCancellation() {
    testutil::Directory directory(L"direct-phase-cancel");
    auto engine = Engine(directory.path());
    Put(engine.gameRoot + L"\\config.ini", "NEW-CONFIG");
    Put(engine.gameRoot + L"\\hc.dat", "NEW-HC");
    Put(engine.cacheRoot + L"\\hc.dat", "OLD-HC");
    Put(engine.cacheRoot + L"\\saves\\stale.dat", "KEEP-ON-CANCEL");
    std::atomic<bool> cancel{false};
    engine.cancelFlag = &cancel;
    engine.onProgress = [&](int, int, const std::wstring& label) {
        if (label == L"mirror/hc.dat")
            cancel.store(true);
    };
    const auto first = engine.RunOnce(true);
    Check(first.cancelled && !first.success && first.partial && first.copied == 1 && first.deleted == 0 &&
              testutil::ReadBytes(engine.cacheRoot + L"\\config.ini") == "NEW-CONFIG" &&
              testutil::ReadBytes(engine.cacheRoot + L"\\hc.dat") == "OLD-HC" &&
              fileops::Exists(engine.cacheRoot + L"\\saves\\stale.dat"),
          "mirror per-file cancellation leaves completed native copy and stops later copies/deletions");
    cancel.store(false);
    engine.onProgress = [&](int, int, const std::wstring& label) {
        if (label == L"mirror/verified")
            cancel.store(true);
    };
    const auto boundary = engine.RunOnce(true);
    Check(boundary.cancelled && boundary.copied == 2 && boundary.deleted == 0 &&
              fileops::Exists(engine.cacheRoot + L"\\saves\\stale.dat"),
          "legacy native-copy completion label remains a cancellable boundary before mirror deletion");
    cancel.store(false);
    engine.onProgress = [&](int, int, const std::wstring& label) {
        if (label == L"restore/hc.dat")
            cancel.store(true);
    };
    Put(engine.gameRoot + L"\\hc.dat", "RESTORE-NOT-YET");
    const auto restore = engine.RestoreDetailed();
    Check(restore.cancelled && restore.copied == 1 && restore.deleted == 0 &&
              testutil::ReadBytes(engine.gameRoot + L"\\hc.dat") == "RESTORE-NOT-YET",
          "restore per-file cancellation stops remaining physical cache copies");
}

// Arm after enumeration: the third cancellation poll is the CopyFileExW progress callback.
void TestNativeCopyCancellation() {
    testutil::Directory directory(L"direct-native-cancel");
    auto engine = Engine(directory.path());
    Put(engine.cacheRoot + L"\\saves\\a-large.bin", std::string(4 * 1024 * 1024, 'L'));
    Put(engine.cacheRoot + L"\\saves\\z-next.dat", "MUST-NOT-COPY");
    bool armed = false;
    int polls = 0;
    engine.onProgress = [&](int, int, const std::wstring& label) {
        armed = armed || label == L"restore/saves\\a-large.bin";
    };
    engine.shouldCancel = [&] { return armed && ++polls >= 3; };
    const auto result = engine.RestoreDetailed();
    Check(result.cancelled && !result.success && result.copied == 0 && polls >= 3 &&
              !fileops::Exists(engine.gameRoot + L"\\saves\\z-next.dat"),
          "CopyFileExW callback checks engine cancellation and never starts the next cached file");
}

// No metadata may filter a physically present managed file out of restore.
void TestPhysicalCacheTruth() {
    testutil::Directory directory(L"direct-physical-truth");
    auto engine = Engine(directory.path());
    Put(engine.cacheRoot + L"\\saves\\physically-present.dat", "PHYSICAL");
    Put(engine.cacheRoot + L"\\.lolbackup-sync-state-v2.json", "obsolete or corrupt absence metadata");
    Put(engine.cacheRoot + L"\\.lolbackup-owned-v1", "foreign marker bytes");
    Put(engine.gameRoot + L"\\saves\\physically-present.dat", "PHYSICAL");
    const auto result = engine.RestoreDetailed();
    Check(result.success && result.copied == 1 && result.skipped == 0 &&
              testutil::ReadBytes(engine.gameRoot + L"\\saves\\physically-present.dat") == "PHYSICAL" &&
              testutil::ReadBytes(engine.cacheRoot + L"\\.lolbackup-owned-v1") == "foreign marker bytes" &&
              testutil::ReadBytes(engine.cacheRoot + L"\\.lolbackup-sync-state-v2.json") ==
                  "obsolete or corrupt absence metadata",
          "current physical cache wins over obsolete metadata and identical destination bytes");
}

// Incomplete managed inventories never define a destructive plan, even when partially nonempty.
void TestIncompleteAndScope() {
    testutil::Directory directory(L"direct-scope");
    auto engine = Engine(directory.path());
    Put(engine.gameRoot + L"\\config.ini", "VISIBLE");
    Put(engine.gameRoot + L"\\saves\\blocked\\hidden.dat", "HIDDEN");
    Put(engine.cacheRoot + L"\\user.ini", "KEEP-WHEN-INCOMPLETE");
    {
        winutil::ScopedHandle blocked(CreateFileW(
            testutil::Native(engine.gameRoot + L"\\saves\\blocked").c_str(), GENERIC_READ, 0,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        Check(blocked.valid(), "exclusive managed subtree fixture is installed");
        const auto incomplete = engine.RunOnce(true);
        Check(!incomplete.success && incomplete.errors > 0 && incomplete.copied == 0 &&
                  incomplete.deleted == 0 && !incomplete.sourceNonempty &&
                  testutil::ReadBytes(engine.cacheRoot + L"\\user.ini") == "KEEP-WHEN-INCOMPLETE",
              "incomplete Game enumeration returns a real error without copying or planning deletes");
    }
    auto alias = engine;
    alias.cacheRoot = alias.gameRoot;
    for (auto& character : alias.cacheRoot)
        character = static_cast<wchar_t>(std::towupper(character));
    const auto equal = alias.RunOnce(true);
    Check(equal.ran && !equal.success && equal.errors > 0 && equal.copied == 0 && equal.deleted == 0,
          "case alias of the same physical root fails the one-time disjoint scope check");
    alias.cacheRoot = engine.gameRoot + L"\\nested-cache";
    const auto nested = alias.RestoreDetailed();
    Check(!nested.success && nested.errors > 0 && nested.copied == 0,
          "nested Game/cache roots are rejected before direct restore writes");
}

// Managed reparse entries fail enumeration without traversal; unrelated reparse entries are ignored.
void TestReparseNonTraversal() {
    testutil::Directory directory(L"direct-reparse");
    auto engine = Engine(directory.path());
    const auto outside = directory.path() + L"\\outside";
    const auto managedLink = engine.gameRoot + L"\\saves\\link";
    Put(outside + L"\\external.dat", "EXTERNAL-UNCHANGED");
    Put(engine.gameRoot + L"\\config.ini", "GAME");
    Put(engine.cacheRoot + L"\\user.ini", "KEEP-UNTIL-COMPLETE");
    const bool installed = fileops::EnsureDir(managedLink) && SetJunction(managedLink, outside);
    Check(installed, "isolated managed junction fixture is installed without symlink privilege");
    if (!installed)
        return;
    const auto rejected = engine.RunOnce(true);
    Check(!rejected.success && rejected.errors > 0 && rejected.copied == 0 && rejected.deleted == 0 &&
              testutil::ReadBytes(outside + L"\\external.dat") == "EXTERNAL-UNCHANGED" &&
              fileops::Exists(engine.cacheRoot + L"\\user.ini"),
          "managed source junction is never traversed and incomplete inventory cannot delete cache");
    Check(RemoveDirectoryW(testutil::Native(managedLink).c_str()) != FALSE,
          "remove only the managed junction object, not its external target");
    const auto unrelated = engine.gameRoot + L"\\projects\\link";
    const bool unrelatedInstalled = fileops::EnsureDir(unrelated) && SetJunction(unrelated, outside);
    Check(unrelatedInstalled, "isolated unrelated junction fixture is installed");
    if (!unrelatedInstalled)
        return;
    const auto copied = engine.RunOnce(true);
    Check(copied.success && copied.copied == 1 && copied.deleted == 1 &&
              !fileops::Exists(engine.cacheRoot + L"\\projects") &&
              testutil::ReadBytes(outside + L"\\external.dat") == "EXTERNAL-UNCHANGED",
          "unrelated junction is not inventoried, copied, traversed or used to block native mirror");
}
} // namespace

int main() {
    try {
        TestFullAndEmptyMirrors();
        TestErrorsContinue();
        TestRestoreBusyContinues();
        TestPhaseCancellation();
        TestNativeCopyCancellation();
        TestPhysicalCacheTruth();
        TestIncompleteAndScope();
        TestReparseNonTraversal();
        core::Log().Close();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Fixture failure: %s\n", error.what());
        ++failed;
    }
    std::printf("Direct sync semantics: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
