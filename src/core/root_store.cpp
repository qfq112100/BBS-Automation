#include "root_store.h"
#include "../../third_party/json/json.hpp"
#include "../common/limits_shared.h"
#include "../common/utf.h"
#include "../common/winutil.h"
#include "file_ops.h"
#include "logger.h"
#include "path_policy.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <wincrypt.h>

namespace core::rootstore {
namespace {
using winutil::ScopedHandle;
using json = nlohmann::json;
constexpr const char* Magic = "LOLBackup managed root record";
// This is scoped application authorization, NOT authority for generic whole-tree MirrorDir.
constexpr const char* Scope = "managed-business-and-snapshot-store";
constexpr const wchar_t* MarkerName = L".lolbackup-owned-v1";
constexpr const char MarkerBytes[] = "LOLBackup dedicated mirror root v1\r\n";
constexpr const wchar_t* LockName = L".~lolbackup-service.lock";
// Diagnostics are best effort, including message allocation, and never consume a caller's Win32 error.
template <typename Build> void Diagnostic(LogLevel level, Build&& build) noexcept {
    const DWORD saved = GetLastError();
    try {
        const auto message = build();
        if (level == LogLevel::Error)
            core::Log().Error(L"RootStore", message);
        else if (level == LogLevel::Warn)
            core::Log().Warn(L"RootStore", message);
        else
            core::Log().Info(L"RootStore", message);
    } catch (...) {
    }
    SetLastError(saved);
}
struct FailureDiagnostic {
    const wchar_t* operation;
    const std::wstring& error;
    const wchar_t* phase = L"preflight";
    bool success = false;
    size_t completed = 0;
    const std::wstring* root = nullptr;
    ~FailureDiagnostic() noexcept {
        if (!success) {
            const DWORD code = GetLastError();
            Diagnostic(LogLevel::Error, [&] {
                return std::wstring(L"event=root_store_failed code=") + phase + L" operation=" + operation +
                       L" win32_observed=" + std::to_wstring(code) + L" completed=" +
                       std::to_wstring(completed) + (root ? L" root=" + *root : L"") + L" detail=" + error;
            });
        }
    }
};
bool Fail(std::wstring& error, const std::wstring& text, DWORD code = ERROR_INVALID_DATA) {
    error = text + L" (Win32=" + std::to_wstring(code) + L")";
    SetLastError(code);
    return false;
}
bool FromFileError(std::wstring& error, const fileops::FileError& e) {
    return Fail(error, e.operation + L": " + e.path + L" — " + e.message, e.code);
}
bool Missing(DWORD code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}
bool Dispose(HANDLE file) {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
}
struct Pending {
    ScopedHandle file;
    bool committed = false;
    ~Pending() {
        const DWORD saved = GetLastError();
        if (!committed && file.valid())
            Dispose(file.get());
        SetLastError(saved);
    }
};
bool Identity(const std::wstring& root, std::wstring& canonical, std::wstring& physical,
              std::wstring& error) {
    // Every caller retains a DirectoryLease for this directory and its ancestors.
    // A direct directory open also supports a drive/share root used only as an ancestor.
    ScopedHandle file(CreateFileW(winutil::LongPath(root).c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid())
        return Fail(error, L"Cannot open pinned root identity", GetLastError());
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info))
        return Fail(error, L"Cannot read root identity", GetLastError());
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return Fail(error, L"Root identity is not an ordinary directory");
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD needed = GetFinalPathNameByHandleW(file.get(), nullptr, 0, flags);
    if (!needed || needed > common::kMaxPathChars)
        return Fail(error, L"Cannot resolve canonical root", GetLastError());
    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1);
    const DWORD copied =
        GetFinalPathNameByHandleW(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (!copied || copied >= buffer.size())
        return Fail(error, L"Cannot read canonical root", GetLastError());
    canonical = winutil::StripLongPrefix(std::wstring(buffer.data(), copied));
    std::wstring normalized;
    if (!pathpolicy::NormalizePath(canonical, normalized, error))
        return false;
    canonical = normalized;
    wchar_t id[40]{};
    swprintf(id, 40, L"%08lx-%08lx%08lx", static_cast<unsigned long>(info.dwVolumeSerialNumber),
             static_cast<unsigned long>(info.nFileIndexHigh), static_cast<unsigned long>(info.nFileIndexLow));
    if (!info.nFileIndexHigh && !info.nFileIndexLow)
        return Fail(error, L"Filesystem does not provide a stable data-root file identity",
                    ERROR_NOT_SUPPORTED);
    physical = id;
    return true;
}
// For a missing root, retain the longest existing ancestor until registration finishes.
// Never create the root merely to discover its logical management key.
bool Candidate(const std::wstring& root, std::wstring& canonical, std::wstring& physical, bool& missing,
               fileops::DirectoryLease& pin, std::wstring& error) {
    std::wstring ancestor = root, suffix;
    while (!pin.Acquire(ancestor, error)) {
        if (!Missing(GetLastError()))
            return false;
        const size_t boundary = pathpolicy::RootLength(ancestor);
        const size_t slash = ancestor.find_last_of(L'\\');
        if (slash == std::wstring::npos || ancestor.size() <= boundary)
            return Fail(error, L"Cannot resolve an existing ancestor of the data root", ERROR_PATH_NOT_FOUND);
        suffix = L"\\" + ancestor.substr(slash + 1) + suffix;
        ancestor.resize((std::max)(boundary, slash));
    }
    if (!Identity(ancestor, canonical, physical, error))
        return false;
    missing = !suffix.empty();
    if (missing) {
        if (!canonical.empty() && canonical.back() == L'\\')
            canonical.pop_back();
        canonical += suffix;
        physical.clear();
    }
    return true;
}
bool KeyFor(const std::wstring& canonical, std::string& key, std::wstring& error) {
    // Invariant folding avoids a path-spelling-only split for ordinary case aliases.
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, canonical.data(),
                                   static_cast<int>(canonical.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!size)
        return Fail(error, L"Cannot canonicalize management key", GetLastError());
    std::wstring folded(static_cast<size_t>(size), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, canonical.data(),
                      static_cast<int>(canonical.size()), &folded[0], size, nullptr, nullptr, 0) != size)
        return Fail(error, L"Cannot fold management key", GetLastError());
    std::string bytes;
    if (!TryWideToUtf8(folded, bytes))
        return Fail(error, L"Invalid root identity encoding");
    key = Digest(bytes, error);
    return !key.empty();
}
bool ReadBytes(HANDLE file, std::string& bytes, std::wstring& error, size_t maximum = 256 * 1024) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size))
        return Fail(error, L"Cannot inspect management metadata size", GetLastError());
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > maximum)
        return Fail(error, L"Management metadata size is invalid");
    bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    char emptyBuffer = 0;
    if (!ReadFile(file, bytes.empty() ? &emptyBuffer : &bytes[0], static_cast<DWORD>(bytes.size()), &read,
                  nullptr) ||
        read != bytes.size())
        return Fail(error, L"Cannot read complete management metadata", GetLastError());
    return true;
}
bool PhysicalIdentityText(const std::wstring& identity) {
    if (identity.size() != 25 || identity[8] != L'-' || identity.substr(9) == L"0000000000000000")
        return false;
    for (size_t i = 0; i < identity.size(); ++i)
        if (i != 8 &&
            !((identity[i] >= L'0' && identity[i] <= L'9') || (identity[i] >= L'a' && identity[i] <= L'f')))
            return false;
    return true;
}
bool OpenOwner(const Record& record, ScopedHandle& owner, std::wstring& error,
               const wchar_t** reason = nullptr, std::wstring* registeredIdentity = nullptr) {
    if (reason)
        *reason = L"owner_invalid";
    HANDLE raw = INVALID_HANDLE_VALUE;
    fileops::FileError e;
    if (!fileops::OpenFileChecked(record.directory + L"\\owner.json", raw, GENERIC_READ, FILE_SHARE_READ,
                                  OPEN_EXISTING, e))
        return Fail(error, L"[E_ROOT_OWNER_INVALID] 无法安全读取目录授权记录，原记录已保留。", e.code);
    ScopedHandle next(raw);
    std::string bytes;
    if (!ReadBytes(next.get(), bytes, error))
        return Fail(error, L"[E_ROOT_OWNER_INVALID] 目录授权记录无法完整读取，原记录已保留。",
                    GetLastError());
    std::wstring root, physical;
    try {
        const auto j = json::parse(bytes);
        if (!j.is_object() || j.size() != 5 || j.at("magic") != Magic ||
            !j.at("version").is_number_integer() || j.at("version") != 1 || j.at("scope") != Scope ||
            !j.at("root").is_string() || !TryUtf8ToWide(j.at("root").get<std::string>(), root) ||
            !j.at("physicalIdentity").is_string() ||
            !TryUtf8ToWide(j.at("physicalIdentity").get<std::string>(), physical) ||
            !PhysicalIdentityText(physical))
            return Fail(error,
                        L"[E_ROOT_OWNER_INVALID] 目录授权记录格式、版本或授权范围无效，禁止自动重绑。 ");
    } catch (...) {
        return Fail(error, L"[E_ROOT_OWNER_INVALID] 目录授权记录损坏或版本未知，禁止自动重绑。 ");
    }
    if (!pathpolicy::EqualPath(root, record.root)) {
        if (reason)
            *reason = L"owner_path_mismatch";
        return Fail(error, L"[E_ROOT_PATH_MISMATCH] 目录授权记录的路径不匹配，禁止自动重绑。旧身份=" +
                               physical + L"，当前身份=" + record.identity);
    }
    if (registeredIdentity)
        *registeredIdentity = physical;
    if (physical != record.identity) {
        if (reason)
            *reason = L"physical_identity_mismatch";
        // Keep the validated old owner immutable throughout explicit variant registration.
        owner.reset(next.release());
        return Fail(error,
                    L"[E_ROOT_IDENTITY_CHANGED] 此路径对应的物理目录已变化，请明确确认是否作为新环境使用；"
                    L"旧记录及删除状态不会继承。旧身份=" +
                        physical + L"，当前身份=" + record.identity);
    }
    owner.reset(next.release());
    return true;
}
bool CreateOwner(const Record& record, std::wstring& error) {
    std::string root, identity;
    if (!TryWideToUtf8(record.root, root) || !TryWideToUtf8(record.identity, identity))
        return Fail(error, L"Invalid owner identity encoding");
    const std::string bytes = json({{"magic", Magic},
                                    {"version", 1},
                                    {"root", root},
                                    {"physicalIdentity", identity},
                                    {"scope", Scope}})
                                  .dump();
    static std::atomic<unsigned long> serial{0};
    const auto temporary = record.directory + L"\\.pending-owner-" + std::to_wstring(GetCurrentProcessId()) +
                           L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial);
    fileops::FileError e;
    HANDLE raw = INVALID_HANDLE_VALUE;
    if (!fileops::OpenFileChecked(temporary, raw, GENERIC_WRITE | DELETE, 0, CREATE_NEW, e))
        return FromFileError(error, e);
    Pending pending;
    pending.file.reset(raw);
    DWORD written = 0;
    if (!WriteFile(raw, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size() || !FlushFileBuffers(raw))
        return Fail(error, L"Cannot durably write root owner", GetLastError());
    if (!fileops::RenameHandleChecked(raw, record.directory + L"\\owner.json", e, false))
        return FromFileError(error, e);
    pending.committed = true;
    return true;
}
// Caller pins the management parent throughout initialization and publication.
// Never create the final name before BOTH durable files have been verified.
// An interrupted staging directory is recognizable garbage, not a restore/ownership candidate.
bool PublishRecord(const Record& destination, std::wstring& error) {
    const auto slash = destination.directory.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return Fail(error, L"[E_ROOT_OWNER_INVALID] 新授权记录缺少固定管理父目录。");
    const auto parent = destination.directory.substr(0, slash);
    static std::atomic<unsigned long> directorySerial{0};
    Record staging = destination;
    bool created = false;
    for (unsigned attempt = 0; attempt != 128; ++attempt) {
        staging.directory = parent + L"\\.pending-root-record-" + std::to_wstring(GetCurrentProcessId()) +
                            L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                            std::to_wstring(++directorySerial);
        if (CreateDirectoryW(winutil::LongPath(staging.directory).c_str(), nullptr)) {
            created = true;
            break;
        }
        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS)
            return Fail(error, L"[E_ROOT_OWNER_INVALID] 无法创建私有授权暂存目录，未创建最终记录。", code);
    }
    if (!created)
        return Fail(error, L"[E_ROOT_OWNER_INVALID] 无法分配唯一授权暂存目录。", ERROR_ALREADY_EXISTS);
    // On any failure retain this exact private directory. Do not recursively erase unknown
    // entries or reuse another invocation's pending directory; pending owner temp cleanup
    // itself remains exact-handle RAII in CreateOwner.
    if (!fileops::PrepareDedicatedRoot(staging.directory, error))
        return false;
    fileops::DirectoryLease stagingPin;
    if (!stagingPin.Acquire(staging.directory, error, true) || !CreateOwner(staging, error))
        return false;
    ScopedHandle owner;
    if (!OpenOwner(staging, owner, error))
        return false;
    HANDLE raw = INVALID_HANDLE_VALUE;
    fileops::FileError e;
    if (!fileops::OpenDirectoryChecked(staging.directory, raw, FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, e))
        return FromFileError(error, e);
    ScopedHandle directory(raw);
    BY_HANDLE_FILE_INFORMATION expected{};
    if (!GetFileInformationByHandle(directory.get(), &expected))
        return Fail(error, L"[E_ROOT_OWNER_INVALID] 无法固定授权暂存目录身份。", GetLastError());
    // Release no-delete pins only for the deliberate directory rename. The checked move
    // reacquires DELETE on the exact expected identity and commits to the fixed parent.
    directory.reset();
    owner.reset();
    stagingPin.Reset();
    if (!fileops::MoveDirectoryChecked(staging.directory, destination.directory, e, &expected))
        return FromFileError(error, e);
    Diagnostic(LogLevel::Info, [] {
        return L"event=record_directory_published code=atomic_no_replace pending_initialized=1 "
               L"owner_verified=1";
    });
    return true;
}
bool ExclusiveObject(const std::wstring& identity, const wchar_t* suffix, ScopedHandle& object,
                     std::wstring& error, const wchar_t** reason = nullptr) {
    const auto name = L"Global\\BBSManagedRoot-" + identity + suffix;
    // Only existence matters: no thread-owned mutex and no semaphore count release.
    // Default ACL failures (including a different user's existing object) fail closed.
    SetLastError(ERROR_SUCCESS);
    HANDLE raw = CreateSemaphoreW(nullptr, 1, 1, name.c_str());
    const DWORD code = GetLastError();
    ScopedHandle next(raw);
    if (!next.valid()) {
        if (reason)
            *reason = L"global_exclusion_create_failed";
        return Fail(error, L"Cannot acquire global data-root exclusion", code);
    }
    if (code == ERROR_ALREADY_EXISTS) {
        if (reason)
            *reason = L"global_exclusion_busy";
        return Fail(error, L"Data root is already leased by another operation", ERROR_SHARING_VIOLATION);
    }
    object.reset(next.release());
    return true;
}
} // namespace

std::string Digest(const std::string& bytes, std::wstring& error) {
    error.clear();
    struct Crypto {
        HCRYPTPROV provider = 0;
        HCRYPTHASH hash = 0;
        ~Crypto() {
            if (hash)
                CryptDestroyHash(hash);
            if (provider)
                CryptReleaseContext(provider, 0);
        }
    } crypto;
    try {
        if (!CryptAcquireContextW(&crypto.provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
            !CryptCreateHash(crypto.provider, CALG_SHA_256, 0, 0, &crypto.hash)) {
            Fail(error, L"Cannot initialize SHA-256", GetLastError());
            return {};
        }
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD count = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t(1024 * 1024)));
            if (!CryptHashData(crypto.hash, reinterpret_cast<const BYTE*>(bytes.data() + offset), count, 0)) {
                Fail(error, L"Cannot hash management bytes", GetLastError());
                return {};
            }
            offset += count;
        }
        BYTE hash[32]{};
        DWORD size = sizeof(hash);
        if (!CryptGetHashParam(crypto.hash, HP_HASHVAL, hash, &size, 0) || size != sizeof(hash)) {
            Fail(error, L"Cannot obtain SHA-256 result", GetLastError());
            return {};
        }
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (BYTE byte : hash) {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 15]);
        }
        return result;
    } catch (...) {
        Fail(error, L"SHA-256 allocation failure", ERROR_NOT_ENOUGH_MEMORY);
        return {};
    }
}

bool ValidateLocalRoot(const std::wstring& path, std::wstring& error) {
    try {
        // Spelling only: reject a remote path before probing or opening its ancestors.
        std::wstring normalized;
        if (!pathpolicy::NormalizePath(path, normalized, error))
            return false;
        if (normalized.size() < 3 || normalized[1] != L':' || normalized[2] != L'\\')
            return Fail(
                error,
                L"[E_ROOT_LOCAL_ONLY] 仅支持本机固定盘或可移动卷中的普通文件夹；不支持网络共享或映射网络盘。",
                ERROR_NOT_SUPPORTED);
        const auto volume = normalized.substr(0, 3);
        const UINT type = GetDriveTypeW(volume.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            return Fail(error,
                        L"[E_ROOT_LOCAL_ONLY] "
                        L"此路径不是可确认的本机固定盘或可移动卷；不支持网络盘、未知卷或不可用卷。",
                        ERROR_NOT_SUPPORTED);
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return Fail(error, L"[E_ROOT_LOCAL_ONLY] 无法确认本机卷类型，已停止操作。",
                    ERROR_UNHANDLED_EXCEPTION);
    }
}

bool Resolve(const std::wstring& managementRoot, const std::wstring& dataRoot, Record& record,
             std::wstring& error, bool create, bool allowIdentityRebind) {
    FailureDiagnostic diagnostic{L"resolve", error};
    diagnostic.root = &dataRoot;
    try {
        std::wstring management, root;
        if (!ValidateLocalRoot(managementRoot, error) || !ValidateLocalRoot(dataRoot, error))
            return false;
        if (!pathpolicy::NormalizeRoot(managementRoot, management, error) ||
            !pathpolicy::NormalizeRoot(dataRoot, root, error) ||
            !pathpolicy::AreDisjoint(management, root, error))
            return false;
        fileops::DirectoryLease rootPin;
        Record next;
        bool missing = false;
        diagnostic.phase = L"data_root_identity";
        if (!Candidate(root, next.root, next.identity, missing, rootPin, error) ||
            !ValidateLocalRoot(next.root, error))
            return false;
        if (missing && !create) {
            diagnostic.phase = L"registered_root_missing";
            return Fail(error, L"Registered data root is missing; controlled recovery is required",
                        ERROR_PATH_NOT_FOUND);
        }
        std::string key;
        if (!KeyFor(next.root, key, error))
            return false;
        ScopedHandle initialization;
        diagnostic.phase = L"global_initialization_exclusion";
        if (create &&
            !ExclusiveObject(Utf8ToWide(key), L"-initialize", initialization, error, &diagnostic.phase))
            return false;
        diagnostic.phase = L"management_boundary";
        if (create ? !fileops::PrepareDedicatedRoot(management, error)
                   : !fileops::IsDedicatedRoot(management, error))
            return false;
        fileops::DirectoryLease managementPin;
        if (!managementPin.Acquire(management, error, true))
            return false;
        next.directory = management + L"\\root-" + Utf8ToWide(key);
        diagnostic.phase = L"registered_root_missing";
        if (missing) {
            const DWORD attributes = GetFileAttributesW(winutil::LongPath(next.directory).c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
                return Fail(error, L"[E_ROOT_MISSING] "
                                   L"原目录当前不存在，但其管理记录仍保留。请接回原磁盘后重试，或明确选择本机"
                                   L"目录；程序不会用新建空目录替代旧数据。");
            if (!Missing(GetLastError()))
                return Fail(error, L"Cannot inspect prior management record", GetLastError());
            // Never recreate a registered missing root or silently discard its tombstones.
            if (!fileops::EnsureDir(root) || !rootPin.Acquire(root, error))
                return Fail(error, L"Cannot create initial unregistered data root", GetLastError());
            std::wstring canonical;
            if (!Identity(root, canonical, next.identity, error))
                return false;
            if (!pathpolicy::EqualPath(canonical, next.root))
                return Fail(error, L"Data root canonical identity changed during registration");
            next.root = canonical;
        }
        diagnostic.phase = L"record_boundary";
        bool created = false;
        const DWORD recordAttributes = GetFileAttributesW(winutil::LongPath(next.directory).c_str());
        if (recordAttributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD code = GetLastError();
            if (!Missing(code))
                return Fail(error, L"[E_ROOT_OWNER_INVALID] 无法检查目录授权记录。", code);
            if (create) {
                if (!PublishRecord(next, error))
                    return false;
                created = true;
            }
        }
        // Existing final names must already be complete. Never claim an unknown empty
        // or half-initialized final record, including remnants from older versions.
        if (!fileops::IsDedicatedRoot(next.directory, error))
            return false;
        fileops::DirectoryLease recordPin;
        if (!recordPin.Acquire(next.directory, error, true))
            return false;
        // A new final record has already passed staged owner verification before publication.
        // Existing unknown/interrupted final records and changed identities still fail closed.
        diagnostic.phase = L"record_owner_binding";
        ScopedHandle owner, selectedVariantOwner;
        fileops::DirectoryLease selectedVariantPin;
        std::wstring registeredIdentity;
        if (!OpenOwner(next, owner, error, &diagnostic.phase, &registeredIdentity)) {
            // Only a fully valid base owner for this exact canonical path can authorize
            // the confirmation flow. Invalid metadata/path never gets this exception.
            if (wcscmp(diagnostic.phase, L"physical_identity_mismatch") != 0)
                return false;
            const auto identityError = error;
            Diagnostic(LogLevel::Warn, [&] {
                return L"event=foreign_identity_detected code=E_ROOT_IDENTITY_CHANGED old_identity=" +
                       registeredIdentity + L" current_identity=" + next.identity;
            });
            Record variant = next;
            variant.directory += L"-" + next.identity;
            const DWORD attributes = GetFileAttributesW(winutil::LongPath(variant.directory).c_str());
            bool createdVariant = false;
            ScopedHandle rebindExclusion;
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD code = GetLastError();
                if (!Missing(code))
                    return Fail(error, L"[E_ROOT_OWNER_INVALID] 无法检查新环境授权记录，未修改原记录。",
                                code);
                if (!create || !allowIdentityRebind) {
                    error = identityError;
                    SetLastError(ERROR_INVALID_DATA);
                    return false;
                }
                Diagnostic(LogLevel::Info, [&] {
                    return L"event=identity_rebind_confirmation intent=register_fresh_environment "
                           L"confirmed=1 old_identity=" +
                           registeredIdentity + L" current_identity=" + next.identity;
                });
                diagnostic.phase = L"variant_global_physical_root_exclusion";
                if (!ExclusiveObject(next.identity, L"", rebindExclusion, error, &diagnostic.phase))
                    return false;
                diagnostic.phase = L"variant_record_boundary";
                if (!PublishRecord(variant, error))
                    return false;
                createdVariant = true;
            }
            if (!fileops::IsDedicatedRoot(variant.directory, error))
                return Fail(error,
                            L"[E_ROOT_OWNER_INVALID] 新环境授权目录未通过安全校验，禁止覆盖或自动修复。",
                            GetLastError());
            if (!selectedVariantPin.Acquire(variant.directory, error, true))
                return false;
            diagnostic.phase = L"variant_owner_binding";
            if (!OpenOwner(variant, selectedVariantOwner, error, &diagnostic.phase))
                return false;
            variant.identityVariant = true;
            next = std::move(variant);
            Diagnostic(LogLevel::Info, [&] {
                return std::wstring(L"event=identity_variant_selected createdVariant=") +
                       (createdVariant ? L"1" : L"0") + L" current_identity=" + next.identity +
                       L" prior_state_inherited=0 prior_record_modified=0";
            });
        }
        record = std::move(next);
        diagnostic.success = true;
        if (created)
            Diagnostic(LogLevel::Info, [] {
                return L"event=record_created code=new_registration owner_verified=1 "
                       L"global_initialization_exclusion=held";
            });
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return Fail(error, L"Cannot resolve external root record", ERROR_UNHANDLED_EXCEPTION);
    }
}

struct Lease::Impl {
    // Destruction order releases legacy files and directory pins before global exclusion.
    ScopedHandle exclusion;
    fileops::DirectoryLease rootPin, recordPin;
    ScopedHandle owner;
    Record record;
    std::vector<std::unique_ptr<fileops::DirectoryLease>> subtreePins;
    std::vector<std::unique_ptr<ScopedHandle>> legacy;

    bool HoldLegacy(const std::wstring& path, bool marker, std::wstring& error) {
        HANDLE raw = INVALID_HANDLE_VALUE;
        fileops::FileError e;
        if (!fileops::OpenFileChecked(path, raw, GENERIC_READ | DELETE, FILE_SHARE_READ, OPEN_EXISTING, e)) {
            if (Missing(e.code))
                return true;
            return FromFileError(error, e);
        }
        ScopedHandle opened(raw);
        auto file = std::make_unique<ScopedHandle>();
        file->reset(opened.release());
        std::string bytes;
        if (!ReadBytes(file->get(), bytes, error, sizeof(MarkerBytes)))
            return false;
        if ((marker && bytes != MarkerBytes) || (!marker && !bytes.empty()))
            return Fail(error, L"Unknown legacy marker or lock contents: " + path);
        legacy.push_back(std::move(file));
        return true;
    }
};
Lease::Lease() = default;
Lease::~Lease() = default;
Lease::Lease(Lease&&) noexcept = default;
Lease& Lease::operator=(Lease&&) noexcept = default;
void Lease::Reset() noexcept {
    impl_.reset();
}
bool Lease::Matches(const Record& record) const noexcept {
    return impl_ && impl_->exclusion.valid() && impl_->owner.valid() &&
           pathpolicy::EqualPath(impl_->record.root, record.root) &&
           pathpolicy::EqualPath(impl_->record.directory, record.directory) &&
           impl_->record.identity == record.identity;
}
bool Lease::Acquire(const Record& record, std::wstring& error, bool businessCache) {
    FailureDiagnostic diagnostic{L"lease_acquire", error};
    diagnostic.root = &record.root;
    try {
        auto next = std::make_unique<Impl>();
        if (!ValidateLocalRoot(record.root, error) || !ValidateLocalRoot(record.directory, error) ||
            !pathpolicy::AreDisjoint(record.root, record.directory, error) ||
            !next->rootPin.Acquire(record.root, error) ||
            !next->recordPin.Acquire(record.directory, error, true))
            return false;
        diagnostic.phase = L"root_identity";
        std::wstring canonical, physical;
        if (!Identity(record.root, canonical, physical, error))
            return false;
        if (!pathpolicy::EqualPath(canonical, record.root) || physical != record.identity) {
            diagnostic.phase = L"physical_identity_mismatch";
            return Fail(error, L"[E_ROOT_IDENTITY_CHANGED] 授权检查后物理目录发生变化，已停止操作。旧身份=" +
                                   record.identity + L"，当前身份=" + physical);
        }
        diagnostic.phase = L"record_owner_binding";
        if (!OpenOwner(record, next->owner, error, &diagnostic.phase))
            return false;
        diagnostic.phase = L"global_physical_root_exclusion";
        if (!ExclusiveObject(physical, L"", next->exclusion, error, &diagnostic.phase))
            return false;
        diagnostic.phase = L"legacy_controls_exclusion";
        // Legacy exclusion must precede any migration or business mutation.
        if (!next->HoldLegacy(record.root + L"\\" + LockName, false, error) ||
            !next->HoldLegacy(record.root + L"\\" + MarkerName, true, error))
            return false;
        if (businessCache) {
            for (const wchar_t* child : {L"saves", L"shards"}) {
                const auto subtree = record.root + L"\\" + child;
                auto pin = std::make_unique<fileops::DirectoryLease>();
                if (!pin->Acquire(subtree, error)) {
                    if (Missing(GetLastError())) {
                        error.clear();
                        continue;
                    }
                    return false;
                }
                if (!next->HoldLegacy(subtree + L"\\" + MarkerName, true, error))
                    return false;
                next->subtreePins.push_back(std::move(pin));
            }
        }
        next->record = record;
        impl_ = std::move(next); // Failed acquisition preserves the previous lease.
        diagnostic.success = true;
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return Fail(error, L"Cannot acquire managed root lease", ERROR_UNHANDLED_EXCEPTION);
    }
}
bool Lease::FinishLegacyCleanup(std::wstring& error) {
    FailureDiagnostic diagnostic{L"legacy_controls_cleanup", error, L"exact_handle_disposal"};
    size_t removed = 0;
    if (impl_)
        diagnostic.root = &impl_->record.root;
    if (!impl_)
        return Fail(error, L"No managed root lease is held", ERROR_INVALID_HANDLE);
    try {
        for (auto& file : impl_->legacy) {
            if (!file->valid())
                continue;
            if (!Dispose(file->get()))
                return Fail(error, L"Cannot dispose verified legacy management file", GetLastError());
            file->reset();
            ++removed;
            diagnostic.completed = removed;
        }
        impl_->legacy.clear();
        diagnostic.success = true;
        if (removed)
            Diagnostic(LogLevel::Info, [&] {
                return L"event=legacy_controls_cleanup code=exact_handles_disposed removed=" +
                       std::to_wstring(removed) + L" global_exclusion=held";
            });
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return Fail(error, L"Cannot finish legacy management cleanup", ERROR_UNHANDLED_EXCEPTION);
    }
}
} // namespace core::rootstore
