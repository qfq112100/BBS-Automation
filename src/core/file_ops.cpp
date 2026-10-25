// All filesystem I/O is Win32 library calls; no subprocesses or shell operations.
#include "file_ops.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <winternl.h>

namespace fileops {
namespace {
using winutil::ScopedHandle;
const wchar_t* const MarkerName = L".lolbackup-owned-v1";
const char MarkerData[] = "LOLBackup dedicated mirror root v1\r\n";
const wchar_t* const TempPrefix = L".~lolbackup-";
std::wstring LP(const std::wstring& p) {
    return pathpolicy::LongPath(p);
}
bool Missing(DWORD e) {
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}
bool Busy(DWORD e) {
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
}
bool SetError(FileError& e, const wchar_t* op, const std::wstring& path, DWORD code) {
    e.code = code ? code : ERROR_GEN_FAILURE;
    try {
        e.operation = op;
        e.path = path;
        wchar_t text[1024]{};
        DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e.code,
                                 0, text, 1024, nullptr);
        e.message = n ? std::wstring(text, n) : L"Win32 error " + std::to_wstring(e.code);
    } catch (...) {
    }
    SetLastError(e.code);
    return false;
}
bool TextError(std::wstring& out, const FileError& e) {
    try {
        out = e.operation + L": " + e.path + L": " + e.message + L" (" + std::to_wstring(e.code) + L")";
    } catch (...) {
    }
    SetLastError(e.code);
    return false;
}
void AddError(MirrorResult& r, const FileError& e) {
    r.complete = false;
    if (Busy(e.code))
        ++r.busy;
    try {
        r.errors.push_back(e);
    } catch (...) {
    }
    SetLastError(e.code);
}
void Summary(MirrorResult& r) {
    try {
        r.summary = r.cancelled  ? L"Cancelled; no further changes"
                    : r.skipped  ? L"Empty source; target unchanged"
                    : r.complete ? L"Complete"
                                 : L"Incomplete; stopped without further changes";
        r.summary += L"; copied=" + std::to_wstring(r.copied) + L", deleted=" + std::to_wstring(r.deleted) +
                     L", errors=" + std::to_wstring(r.errors.size());
    } catch (...) {
    }
    if (r.cancelled)
        SetLastError(ERROR_CANCELLED);
    else if (!r.errors.empty())
        SetLastError(r.errors.front().code);
    else
        SetLastError(r.complete || r.skipped ? ERROR_SUCCESS : ERROR_GEN_FAILURE);
}
std::wstring Parent(const std::wstring& p) {
    size_t slash = p.find_last_of(L'\\');
    return slash == 2 ? p.substr(0, 3) : p.substr(0, slash);
}
bool Internal(const std::wstring& name) {
    return pathpolicy::EqualPath(name, MarkerName) ||
           (name.size() >= wcslen(TempPrefix) &&
            pathpolicy::EqualPath(name.substr(0, wcslen(TempPrefix)), TempPrefix));
}
bool Info(HANDLE h, BY_HANDLE_FILE_INFORMATION& info, const std::wstring& p, FileError& e, bool directory) {
    if (!GetFileInformationByHandle(h, &info))
        return SetError(e, L"GetFileInformationByHandle", p, GetLastError());
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return SetError(e, L"RejectReparsePoint", p, ERROR_CANT_ACCESS_FILE);
    if (!directory && info.nNumberOfLinks != 1)
        return SetError(e, L"RejectHardlinkedFile", p, ERROR_INVALID_DATA);
    if (!!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
        return SetError(e, L"TypeConflict", p, directory ? ERROR_DIRECTORY : ERROR_ALREADY_EXISTS);
    return true;
}
struct Pins {
    std::vector<std::unique_ptr<ScopedHandle>> handles;
    HANDLE last() const {
        return handles.empty() ? INVALID_HANDLE_VALUE : handles.back()->get();
    }
};
// FILE_LIST_DIRECTORY is essential for effective rename sharing constraints.
// Directory handles share READ/WRITE but NOT DELETE: ordinary rename is blocked
// without breaking the parent access needed by atomic file replacement.
// Reparse safety does not rely on denying WRITE: all child opens and mutation
// commits are relative to these handles, with reparse tags checked before use.
HANDLE OpenRelative(HANDLE parent, const std::wstring& name, ACCESS_MASK access, ULONG sharing,
                    ULONG disposition, ULONG attributes, ULONG options) {
    using NtCreateFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using NtErrorFn = ULONG(WINAPI*)(NTSTATUS);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    auto create = reinterpret_cast<NtCreateFn>(GetProcAddress(module, "NtCreateFile"));
    auto convert = reinterpret_cast<NtErrorFn>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!create || !convert) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (name.empty() || name.find_first_of(L"\\/:") != std::wstring::npos || name.size() > 255) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    UNICODE_STRING text{};
    text.Buffer = const_cast<PWSTR>(name.data());
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    OBJECT_ATTRIBUTES object{};
    InitializeObjectAttributes(&object, &text, OBJ_CASE_INSENSITIVE, parent, nullptr);
    IO_STATUS_BLOCK io{};
    HANDLE result = INVALID_HANDLE_VALUE;
    NTSTATUS status =
        create(&result, access | SYNCHRONIZE, &object, &io, nullptr, attributes, sharing, disposition,
               options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (status < 0) {
        SetLastError(convert(status));
        return INVALID_HANDLE_VALUE;
    }
    SetLastError(ERROR_SUCCESS);
    return result;
}
bool VerifyPins(const Pins& pins, const std::wstring& path, FileError& error) {
    for (const auto& handle : pins.handles) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!Info(handle->get(), info, path, error, true))
            return false;
    }
    return true;
}
bool PinDirs(const std::wstring& p, bool create, Pins& pins, FileError& e, const CancelFn& cancel = nullptr) {
    size_t root = pathpolicy::RootLength(p);
    for (size_t end = root;;) {
        if (cancel && cancel())
            return SetError(e, L"CancelDirectoryTraversal", p, ERROR_CANCELLED);
        std::wstring cur = p.substr(0, end);
        if (!VerifyPins(pins, cur, e))
            return false;
        ScopedHandle h;
        if (pins.handles.empty()) {
            h.reset(CreateFileW(LP(cur).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        } else {
            std::wstring name = cur.substr(cur.find_last_of(L'\\') + 1);
            h.reset(OpenRelative(pins.last(), name, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, create ? FILE_OPEN_IF : FILE_OPEN,
                                 FILE_ATTRIBUTE_NORMAL, FILE_DIRECTORY_FILE));
        }
        if (!h.valid())
            return SetError(e, L"OpenDirectory", cur, GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!Info(h.get(), info, cur, e, true))
            return false;
        // Linux/WSL-style case-sensitive directories are outside this library's
        // NTFS case-insensitive contract. Old Windows returns INVALID_PARAMETER.
        struct CaseFlags {
            ULONG Flags;
        } flags{};
        if (GetFileInformationByHandleEx(h.get(), static_cast<FILE_INFO_BY_HANDLE_CLASS>(23), &flags,
                                         sizeof(flags)) &&
            (flags.Flags & 1))
            return SetError(e, L"CaseSensitiveDirectoryUnsupported", cur, ERROR_NOT_SUPPORTED);
        auto owner = std::unique_ptr<ScopedHandle>(new ScopedHandle());
        owner->reset(h.release());
        pins.handles.push_back(std::move(owner));
        if (end == p.size())
            break;
        size_t next = p.find(L'\\', end == root && p[root] != L'\\' ? root : end + 1);
        end = next == std::wstring::npos ? p.size() : next;
    }
    return true;
}
bool Normalize(const std::wstring& p, std::wstring& n, FileError& e) {
    std::wstring text;
    if (!pathpolicy::NormalizePath(p, n, text))
        return SetError(e, L"ValidatePath", p, GetLastError());
    return true;
}
bool Attributes(const std::wstring& p, DWORD& a, FileError& e, bool missingOK, HANDLE parent) {
    ScopedHandle file(OpenRelative(parent, p.substr(p.find_last_of(L'\\') + 1), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                   FILE_ATTRIBUTE_NORMAL, 0));
    BY_HANDLE_FILE_INFORMATION info{};
    a = INVALID_FILE_ATTRIBUTES;
    if (file.valid()) {
        if (!GetFileInformationByHandle(file.get(), &info))
            return SetError(e, L"GetFileAttributesByHandle", p, GetLastError());
        a = info.dwFileAttributes;
    }
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (missingOK && Missing(code))
            return true;
        return SetError(e, L"GetFileAttributesW", p, code);
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT)
        return SetError(e, L"RejectReparsePoint", p, ERROR_CANT_ACCESS_FILE);
    return true;
}
std::wstring TempName(const std::wstring& dir) {
    static volatile LONG64 sequence = 0;
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return dir + L"\\" + TempPrefix + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetCurrentThreadId()) + L"-" + std::to_wstring(ticks.QuadPart) + L"-" +
           std::to_wstring(InterlockedIncrement64(&sequence)) + L".tmp";
}
// Deletes only the object represented by this handle, not a raced path name.
bool Dispose(HANDLE h) {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(h, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
}
struct TempFile {
    ScopedHandle handle;
    bool committed = false;
    ~TempFile() {
        DWORD saved = GetLastError();
        if (handle.valid() && !committed)
            Dispose(handle.get());
        SetLastError(saved);
    }
};
bool MakeTemp(const std::wstring& dir, TempFile& temp, std::wstring& name, FileError& e, HANDLE parent) {
    for (int i = 0; i != 128; ++i) {
        name = TempName(dir);
        temp.handle.reset(OpenRelative(
            parent, name.substr(name.find_last_of(L'\\') + 1), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
            0, FILE_CREATE, FILE_ATTRIBUTE_NORMAL, FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY));
        if (temp.handle.valid())
            return true;
        DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
            return SetError(e, L"CreateTemporaryFile", name, code);
    }
    return SetError(e, L"CreateTemporaryFile", dir, ERROR_FILE_EXISTS);
}
bool Marker(const std::wstring& dir, ScopedHandle& h, FileError& e, HANDLE parent) {
    std::wstring p = dir + L"\\" + MarkerName;
    h.reset(OpenRelative(parent, MarkerName, GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, FILE_ATTRIBUTE_NORMAL,
                         FILE_NON_DIRECTORY_FILE));
    if (!h.valid())
        return SetError(e, L"OpenOwnershipMarker", p, GetLastError());
    BY_HANDLE_FILE_INFORMATION info{};
    if (!Info(h.get(), info, p, e, false))
        return false;
    if (info.nNumberOfLinks != 1)
        return SetError(e, L"OwnershipMarkerHardlink", p, ERROR_INVALID_DATA);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.get(), &size))
        return SetError(e, L"ReadOwnershipMarker", p, GetLastError());
    if (size.QuadPart != sizeof(MarkerData) - 1)
        return SetError(e, L"InvalidOwnershipMarker", p, ERROR_INVALID_DATA);
    char buf[sizeof(MarkerData)]{};
    DWORD rd = 0;
    if (!ReadFile(h.get(), buf, sizeof(MarkerData) - 1, &rd, nullptr))
        return SetError(e, L"ReadOwnershipMarker", p, GetLastError());
    if (rd != sizeof(MarkerData) - 1 || std::memcmp(buf, MarkerData, rd))
        return SetError(e, L"InvalidOwnershipMarker", p, ERROR_INVALID_DATA);
    // Keep the internal marker invisible even when Explorer shows hidden items.
    SetFileAttributesW(LP(p).c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    SetLastError(ERROR_SUCCESS);
    return true;
}
struct DirectoryEntry {
    std::wstring name;
    DWORD attributes;
};
bool ReadDirectoryEntries(HANDLE directory, const std::wstring& path, std::vector<DirectoryEntry>& entries,
                          FileError& error, const CancelFn& cancel = nullptr) {
    std::vector<unsigned char> buffer(64 * 1024);
    bool first = true;
    for (;;) {
        if (cancel && cancel())
            return SetError(error, L"CancelDirectoryRead", path, ERROR_CANCELLED);
        BY_HANDLE_FILE_INFORMATION identity{};
        if (!Info(directory, identity, path, error, true))
            return false;
        std::fill(buffer.begin(), buffer.end(), 0);
        if (!GetFileInformationByHandleEx(directory,
                                          first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                                          buffer.data(), static_cast<DWORD>(buffer.size()))) {
            DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_FILES)
                return true;
            return SetError(error, L"ReadDirectoryByHandle", path, code);
        }
        first = false;
        size_t offset = 0;
        for (;;) {
            if (cancel && cancel())
                return SetError(error, L"CancelDirectoryRead", path, ERROR_CANCELLED);
            if (buffer.size() - offset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName))
                return SetError(error, L"InvalidDirectoryRecord", path, ERROR_INVALID_DATA);
            auto* item = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
            size_t available = buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            if ((item->FileNameLength % sizeof(wchar_t)) || item->FileNameLength > available)
                return SetError(error, L"InvalidDirectoryRecord", path, ERROR_INVALID_DATA);
            std::wstring name(item->FileName, item->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                if (entries.size() >= 200000)
                    return SetError(error, L"EnumerationLimit", path, ERROR_NOT_ENOUGH_QUOTA);
                entries.push_back({std::move(name), item->FileAttributes});
            }
            if (!item->NextEntryOffset)
                break;
            if (item->NextEntryOffset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) ||
                item->NextEntryOffset >= buffer.size() - offset)
                return SetError(error, L"InvalidDirectoryRecord", path, ERROR_INVALID_DATA);
            offset += item->NextEntryOffset;
        }
    }
}
bool CheckCancel(const CancelFn& cancel) {
    return cancel && cancel();
}
using Identity = std::tuple<DWORD, DWORD, DWORD>;
Identity Id(const BY_HANDLE_FILE_INFORMATION& i) {
    return {i.dwVolumeSerialNumber, i.nFileIndexHigh, i.nFileIndexLow};
}

bool GetPathInfo(const std::wstring& path, BY_HANDLE_FILE_INFORMATION& info, FileError& e) {
    std::wstring n;
    if (!Normalize(path, n, e))
        return false;
    Pins parents;
    if (!PinDirs(Parent(n), false, parents, e))
        return false;
    ScopedHandle h(OpenRelative(parents.last(), n.substr(n.find_last_of(L'\\') + 1), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_ATTRIBUTE_NORMAL, 0));
    if (!h.valid())
        return SetError(e, L"OpenPath", n, GetLastError());
    if (!GetFileInformationByHandle(h.get(), &info))
        return SetError(e, L"GetFileInformationByHandle", n, GetLastError());
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return SetError(e, L"RejectReparsePoint", n, ERROR_CANT_ACCESS_FILE);
    SetLastError(ERROR_SUCCESS);
    return true;
}
} // namespace

struct DirectoryLease::Impl {
    Pins pins;
    ScopedHandle marker;
};
DirectoryLease::DirectoryLease() = default;
DirectoryLease::~DirectoryLease() = default;
DirectoryLease::DirectoryLease(DirectoryLease&&) noexcept = default;
DirectoryLease& DirectoryLease::operator=(DirectoryLease&&) noexcept = default;
void DirectoryLease::Reset() noexcept {
    impl_.reset();
}
bool DirectoryLease::Acquire(const std::wstring& dir, std::wstring& error, bool requireDedicated) {
    try {
        FileError e;
        std::wstring normalized;
        auto next = std::unique_ptr<Impl>(new Impl());
        if (!Normalize(dir, normalized, e) || !PinDirs(normalized, false, next->pins, e) ||
            (requireDedicated && !Marker(normalized, next->marker, e, next->pins.last())))
            return TextError(error, e);
        impl_ = std::move(next);
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        FileError e;
        SetError(e, L"AcquireDirectoryLease", dir, ERROR_UNHANDLED_EXCEPTION);
        return TextError(error, e);
    }
}

ListResult ListFilesChecked(const std::wstring& dir, const CancelFn& cancel) {
    ListResult r;
    try {
        FileError e;
        std::wstring base;
        auto fail = [&](const FileError& err) {
            r.errors.push_back(err);
            SetLastError(err.code);
        };
        if (CheckCancel(cancel)) {
            r.cancelled = true;
            SetLastError(ERROR_CANCELLED);
            return r;
        }
        if (!Normalize(dir, base, e)) {
            fail(e);
            return r;
        }
        Pins root;
        if (!PinDirs(base, false, root, e, cancel)) {
            r.cancelled = e.code == ERROR_CANCELLED;
            fail(e);
            return r;
        }
        struct Node {
            std::wstring relative;
            size_t depth;
        };
        std::vector<Node> stack{{L"", 0}};
        std::set<Identity> visited;
        std::set<std::wstring, pathpolicy::CaseInsensitiveLess> names;
        size_t entries = 0;
        while (!stack.empty()) {
            if (CheckCancel(cancel)) {
                r.cancelled = true;
                SetLastError(ERROR_CANCELLED);
                return r;
            }
            Node node = std::move(stack.back());
            stack.pop_back();
            std::wstring current = base + (node.relative.empty() ? L"" : L"\\" + node.relative);
            Pins pins;
            if (!PinDirs(current, false, pins, e, cancel)) {
                r.cancelled = e.code == ERROR_CANCELLED;
                fail(e);
                return r;
            }
            BY_HANDLE_FILE_INFORMATION identity{};
            if (!Info(pins.last(), identity, current, e, true)) {
                fail(e);
                return r;
            }
            if (!visited.insert(Id(identity)).second) {
                SetError(e, L"DirectoryCycleOrAlias", current, ERROR_CIRCULAR_DEPENDENCY);
                fail(e);
                return r;
            }
            std::vector<DirectoryEntry> children;
            if (!ReadDirectoryEntries(pins.last(), current, children, e, cancel)) {
                r.cancelled = e.code == ERROR_CANCELLED;
                fail(e);
                return r;
            }
            for (const auto& item : children) {
                if (CheckCancel(cancel)) {
                    r.cancelled = true;
                    SetLastError(ERROR_CANCELLED);
                    return r;
                }
                const std::wstring& name = item.name;
                std::wstring rel = node.relative.empty() ? name : node.relative + L"\\" + name;
                std::wstring full = base + L"\\" + rel;
                if (++entries > 200000 || node.depth >= 256) {
                    SetError(e, L"EnumerationLimit", full, ERROR_NOT_ENOUGH_QUOTA);
                    fail(e);
                    return r;
                }
                if (item.attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    SetError(e, L"RejectReparsePoint", full, ERROR_CANT_ACCESS_FILE);
                    fail(e);
                    return r;
                }
                std::wstring normalized;
                if (!Normalize(full, normalized, e)) {
                    fail(e);
                    return r;
                }
                if (!names.insert(rel).second) {
                    SetError(e, L"CaseCollidingNames", full, ERROR_DUP_NAME);
                    fail(e);
                    return r;
                }
                if (!(item.attributes & FILE_ATTRIBUTE_DIRECTORY) && !Internal(name)) {
                    ScopedHandle child(OpenRelative(pins.last(), name, FILE_READ_ATTRIBUTES,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                    FILE_OPEN, FILE_ATTRIBUTE_NORMAL,
                                                    FILE_NON_DIRECTORY_FILE));
                    BY_HANDLE_FILE_INFORMATION childInfo{};
                    if (!child.valid() || !Info(child.get(), childInfo, full, e, false)) {
                        if (!child.valid())
                            SetError(e, L"InspectListedFile", full, GetLastError());
                        fail(e);
                        return r;
                    }
                }
                if (Internal(name)) {
                    if (item.attributes & FILE_ATTRIBUTE_DIRECTORY) {
                        SetError(e, L"InternalNameTypeConflict", full, ERROR_DIRECTORY);
                        fail(e);
                        return r;
                    }
                } else if (item.attributes & FILE_ATTRIBUTE_DIRECTORY) {
                    r.directories.push_back(rel);
                    stack.push_back({rel, node.depth + 1});
                } else
                    r.files.push_back(rel);
            }
        }
        std::sort(r.files.begin(), r.files.end(), pathpolicy::CaseInsensitiveLess{});
        std::sort(r.directories.begin(), r.directories.end(), pathpolicy::CaseInsensitiveLess{});
        r.complete = true;
        SetLastError(ERROR_SUCCESS);
    } catch (...) {
        r.complete = false;
        FileError e;
        SetError(e, L"EnumerationExceptionOrCallback", dir, ERROR_UNHANDLED_EXCEPTION);
        try {
            r.errors.push_back(e);
        } catch (...) {
        }
        SetLastError(e.code);
    }
    return r;
}
ListResult ListBusinessFilesChecked(const std::wstring& dir, const CancelFn& cancel) {
    ListResult result;
    try {
        FileError error;
        std::wstring root;
        Pins boundary;
        auto fail = [&](const FileError& e) {
            result.cancelled = e.code == ERROR_CANCELLED;
            result.errors.push_back(e);
            SetLastError(e.code);
        };
        if (CheckCancel(cancel)) {
            SetError(error, L"CancelBusinessRead", dir, ERROR_CANCELLED);
            fail(error);
            return result;
        }
        if (!Normalize(dir, root, error) || !PinDirs(root, false, boundary, error, cancel)) {
            fail(error);
            return result;
        }
        const wchar_t* const names[] = {L"saves",  L"shards",  L"bot_5.dll",   L"config.ini",
                                        L"hc.dat", L"HID.dll", L"shards.json", L"user.ini"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (CheckCancel(cancel)) {
                SetError(error, L"CancelBusinessRead", root, ERROR_CANCELLED);
                fail(error);
                return result;
            }
            const bool directory = i < 2;
            const auto full = root + L"\\" + names[i];
            // Open only this fixed managed name through the pinned root. In particular,
            // inaccessible/linked/huge unrelated subtrees never enter the inventory.
            ScopedHandle entry(OpenRelative(
                boundary.last(), names[i], FILE_READ_ATTRIBUTES | (directory ? FILE_LIST_DIRECTORY : 0),
                directory ? FILE_SHARE_READ | FILE_SHARE_WRITE
                          : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                FILE_OPEN, FILE_ATTRIBUTE_NORMAL, directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE));
            if (!entry.valid()) {
                const DWORD code = GetLastError();
                if (Missing(code))
                    continue;
                SetError(error, L"OpenManagedEntry", full, code);
                fail(error);
                return result;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!Info(entry.get(), info, full, error, directory)) {
                fail(error);
                return result;
            }
            if (!directory) {
                result.files.push_back(names[i]);
                continue;
            }
            auto tree = ListFilesChecked(full, cancel);
            if (!tree.complete) {
                result.cancelled = tree.cancelled;
                result.errors.insert(result.errors.end(), tree.errors.begin(), tree.errors.end());
                if (result.errors.empty()) {
                    SetError(error, L"IncompleteManagedInventory", full,
                             tree.cancelled ? ERROR_CANCELLED : ERROR_GEN_FAILURE);
                    fail(error);
                }
                return result;
            }
            if (result.files.size() + result.directories.size() + tree.files.size() +
                    tree.directories.size() + 1 >
                200000) {
                SetError(error, L"ManagedEnumerationLimit", full, ERROR_NOT_ENOUGH_QUOTA);
                fail(error);
                return result;
            }
            result.directories.push_back(names[i]);
            const std::wstring prefix = std::wstring(names[i]) + L"\\";
            for (const auto& file : tree.files)
                result.files.push_back(prefix + file);
            for (const auto& sub : tree.directories)
                result.directories.push_back(prefix + sub);
        }
        if (CheckCancel(cancel)) {
            SetError(error, L"CancelBusinessRead", root, ERROR_CANCELLED);
            fail(error);
            return result;
        }
        std::sort(result.files.begin(), result.files.end(), pathpolicy::CaseInsensitiveLess{});
        std::sort(result.directories.begin(), result.directories.end(), pathpolicy::CaseInsensitiveLess{});
        result.complete = true;
        SetLastError(ERROR_SUCCESS);
    } catch (...) {
        FileError error;
        SetError(error, L"BusinessEnumerationException", dir, ERROR_UNHANDLED_EXCEPTION);
        try {
            result.errors.push_back(error);
        } catch (...) {
        }
        result.complete = false;
        SetLastError(error.code);
    }
    return result;
}
std::vector<std::wstring> ListFiles(const std::wstring& dir) {
    auto r = ListFilesChecked(dir);
    return r.complete ? std::move(r.files) : std::vector<std::wstring>{};
}
int CountFiles(const std::wstring& dir) {
    auto r = ListFilesChecked(dir);
    if (!r.complete)
        return 0;
    if (r.files.size() > INT_MAX) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return 0;
    }
    return static_cast<int>(r.files.size());
}
bool EnsureDir(const std::wstring& dir) {
    try {
        FileError e;
        std::wstring n;
        Pins pins;
        if (!Normalize(dir, n, e) || !PinDirs(n, true, pins, e))
            return false;
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
}
bool IsDedicatedRoot(const std::wstring& dir, std::wstring& error) {
    try {
        FileError e;
        std::wstring n;
        Pins pins;
        ScopedHandle marker;
        if (!Normalize(dir, n, e) || !PinDirs(n, false, pins, e) || !Marker(n, marker, e, pins.last()))
            return TextError(error, e);
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        FileError e;
        SetError(e, L"OwnershipException", dir, ERROR_UNHANDLED_EXCEPTION);
        return TextError(error, e);
    }
}
static bool PrepareRoot(const std::wstring& dir, std::wstring& error, bool userSelected) {
    try {
        FileError e;
        std::wstring n;
        Pins pins;
        if (!Normalize(dir, n, e) || !PinDirs(n, true, pins, e))
            return TextError(error, e);
        ScopedHandle marker;
        if (Marker(n, marker, e, pins.last()))
            return ProbeWritable(n, error);
        if (!Missing(e.code))
            return TextError(error, e);
        // A user explicitly selecting a mixed folder authorizes managed files only, not
        // a whole-directory mirror. Preserve unrelated entries without even traversing them.
        // Internal callers keep the historical empty-or-owned requirement.
        if (!userSelected) {
            std::vector<DirectoryEntry> existing;
            if (!ReadDirectoryEntries(pins.last(), n, existing, e))
                return TextError(error, e);
            if (!existing.empty()) {
                SetError(e, L"RefuseUnownedNonemptyDirectory", n, ERROR_DIR_NOT_EMPTY);
                return TextError(error, e);
            }
        }
        std::wstring mp = n + L"\\" + MarkerName;
        TempFile created;
        created.handle.reset(OpenRelative(pins.last(), MarkerName, GENERIC_WRITE | DELETE, 0, FILE_CREATE,
                                          FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM,
                                          FILE_NON_DIRECTORY_FILE));
        if (!created.handle.valid()) {
            SetError(e, L"CreateOwnershipMarker", mp, GetLastError());
            return TextError(error, e);
        }
        DWORD written = 0;
        if (!WriteFile(created.handle.get(), MarkerData, sizeof(MarkerData) - 1, &written, nullptr)) {
            SetError(e, L"WriteOwnershipMarker", mp, GetLastError());
            return TextError(error, e);
        }
        if (written != sizeof(MarkerData) - 1) {
            SetError(e, L"WriteOwnershipMarker", mp, ERROR_WRITE_FAULT);
            return TextError(error, e);
        }
        if (!FlushFileBuffers(created.handle.get())) {
            SetError(e, L"FlushOwnershipMarker", mp, GetLastError());
            return TextError(error, e);
        }
        created.committed = true;
        created.handle.reset();
        return ProbeWritable(n, error);
    } catch (...) {
        FileError e;
        SetError(e, L"PrepareRootException", dir, ERROR_UNHANDLED_EXCEPTION);
        return TextError(error, e);
    }
}
bool PrepareDedicatedRoot(const std::wstring& dir, std::wstring& error) {
    return PrepareRoot(dir, error, false);
}
bool PrepareSelectedRoot(const std::wstring& dir, std::wstring& error) {
    return PrepareRoot(dir, error, true);
}
bool RemoveIfEmptyDedicatedRoot(const std::wstring& dir, std::wstring& error) {
    try {
        FileError e;
        std::wstring n;
        Pins parents;
        if (!Normalize(dir, n, e) || !PinDirs(Parent(n), false, parents, e))
            return TextError(error, e);
        // Pin the exact DELETE-capable object through disposition, not a raced path.
        ScopedHandle directory(OpenRelative(parents.last(), n.substr(n.find_last_of(L'\\') + 1),
                                            DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ, FILE_OPEN, FILE_ATTRIBUTE_NORMAL,
                                            FILE_DIRECTORY_FILE));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!directory.valid()) {
            SetError(e, L"OpenCleanupRoot", n, GetLastError());
            return TextError(error, e);
        }
        if (!Info(directory.get(), info, n, e, true))
            return TextError(error, e);
        std::vector<DirectoryEntry> entries;
        if (!ReadDirectoryEntries(directory.get(), n, entries, e))
            return TextError(error, e);
        std::vector<std::unique_ptr<ScopedHandle>> held;
        bool marker = false;
        for (const auto& entry : entries) {
            const bool isMarker = _wcsicmp(entry.name.c_str(), MarkerName) == 0;
            const bool isLock = _wcsicmp(entry.name.c_str(), L".~lolbackup-service.lock") == 0;
            if ((!isMarker && !isLock) ||
                (entry.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                SetError(e, L"RefuseUnknownCleanupEntry", n + L"\\" + entry.name, ERROR_DIR_NOT_EMPTY);
                return TextError(error, e);
            }
            auto file = std::make_unique<ScopedHandle>(
                OpenRelative(directory.get(), entry.name, GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ, FILE_OPEN, FILE_ATTRIBUTE_NORMAL, FILE_NON_DIRECTORY_FILE));
            if (!file->valid()) {
                SetError(e, L"OpenCleanupEntry", n + L"\\" + entry.name, GetLastError());
                return TextError(error, e);
            }
            if (!Info(file->get(), info, n + L"\\" + entry.name, e, false))
                return TextError(error, e);
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file->get(), &size)) {
                SetError(e, L"SizeCleanupEntry", n, GetLastError());
                return TextError(error, e);
            }
            if (isMarker) {
                char data[sizeof(MarkerData)]{};
                DWORD read = 0;
                if (size.QuadPart != sizeof(MarkerData) - 1 ||
                    !ReadFile(file->get(), data, sizeof(MarkerData) - 1, &read, nullptr) ||
                    read != sizeof(MarkerData) - 1 || std::memcmp(data, MarkerData, read)) {
                    SetError(e, L"InvalidCleanupOwnership", n, ERROR_INVALID_DATA);
                    return TextError(error, e);
                }
                marker = true;
            } else if (size.QuadPart != 0) {
                SetError(e, L"UnknownCleanupLock", n, ERROR_INVALID_DATA);
                return TextError(error, e);
            }
            held.push_back(std::move(file));
        }
        if (!entries.empty() && !marker) {
            SetError(e, L"UnownedCleanupRoot", n, ERROR_INVALID_DATA);
            return TextError(error, e);
        }
        // No mutation until every entry has been validated and pinned.
        for (auto& file : held) {
            if (!Dispose(file->get())) {
                SetError(e, L"DisposeCleanupEntry", n, GetLastError());
                return TextError(error, e);
            }
        }
        held.clear();
        if (!Dispose(directory.get())) {
            SetError(e, L"DisposeCleanupRoot", n, GetLastError());
            return TextError(error, e);
        }
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        FileError e;
        SetError(e, L"RemoveEmptyRootException", dir, ERROR_UNHANDLED_EXCEPTION);
        return TextError(error, e);
    }
}
bool ClaimBusinessCacheRoot(const std::wstring& dir, std::wstring& error) {
    try {
        FileError e;
        std::wstring n;
        Pins pins;
        if (!Normalize(dir, n, e) || !PinDirs(n, true, pins, e))
            return TextError(error, e);
        ScopedHandle marker;
        if (Marker(n, marker, e, pins.last()))
            return ProbeWritable(n, error);
        if (!Missing(e.code))
            return TextError(error, e);
        // Revalidate physical entries inside the pinned claim transaction.
        struct Node {
            HANDLE handle;
            std::wstring path;
            bool leftover;
        };
        std::vector<Node> pending{{pins.last(), n, false}};
        std::vector<std::unique_ptr<ScopedHandle>> claimPins;
        size_t inspected = 0;
        const wchar_t* bot[] = {L"bot_5.dll", L"config.ini",  L"hc.dat",
                                L"HID.dll",   L"shards.json", L"user.ini"};
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            std::vector<DirectoryEntry> entries;
            if (!ReadDirectoryEntries(node.handle, node.path, entries, e))
                return TextError(error, e);
            bool owned = false, lockSeen = false;
            for (const auto& entry : entries) {
                const auto full = node.path + L"\\" + entry.name;
                if (++inspected > 200000) {
                    SetError(e, L"BusinessClaimLimit", full, ERROR_NOT_ENOUGH_QUOTA);
                    return TextError(error, e);
                }
                const bool root = node.path == n,
                           directory = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool ownerEntry = _wcsicmp(entry.name.c_str(), MarkerName) == 0;
                const bool lockEntry = _wcsicmp(entry.name.c_str(), L".~lolbackup-service.lock") == 0;
                const bool businessDir = _wcsicmp(entry.name.c_str(), L"saves") == 0 ||
                                         _wcsicmp(entry.name.c_str(), L"shards") == 0;
                const bool leftover = root && (_wcsicmp(entry.name.c_str(), L"LOLBackupV3Cache") == 0 ||
                                               _wcsicmp(entry.name.c_str(), L"LOLBackupV3Snapshots") == 0);
                bool botEntry = false;
                for (const auto* name : bot)
                    botEntry = botEntry || _wcsicmp(entry.name.c_str(), name) == 0;
                if ((entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    (root && !((directory && (businessDir || leftover)) || (!directory && botEntry))) ||
                    (node.leftover && !(ownerEntry || lockEntry)) ||
                    (!root && Internal(entry.name) && !ownerEntry && !lockEntry)) {
                    SetError(e, L"UnknownBusinessClaimContent", full, ERROR_INVALID_DATA);
                    return TextError(error, e);
                }
                if (ownerEntry) {
                    auto owner = std::make_unique<ScopedHandle>();
                    if (!Marker(node.path, *owner, e, node.handle))
                        return TextError(error, e);
                    owned = true;
                    claimPins.push_back(std::move(owner));
                    continue;
                }
                auto child = std::make_unique<ScopedHandle>(OpenRelative(
                    node.handle, entry.name,
                    directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ,
                    directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, FILE_OPEN,
                    FILE_ATTRIBUTE_NORMAL, directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE));
                BY_HANDLE_FILE_INFORMATION info{};
                if (!child->valid() || !Info(child->get(), info, full, e, directory)) {
                    if (!child->valid())
                        SetError(e, L"OpenBusinessClaimEntry", full, GetLastError());
                    return TextError(error, e);
                }
                if (lockEntry) {
                    LARGE_INTEGER size{};
                    if (!GetFileSizeEx(child->get(), &size) || size.QuadPart != 0) {
                        SetError(e, L"UnknownBusinessClaimLock", full, ERROR_INVALID_DATA);
                        return TextError(error, e);
                    }
                    lockSeen = true;
                }
                if (directory)
                    pending.push_back({child->get(), full, leftover || node.leftover});
                claimPins.push_back(std::move(child));
            }
            if (node.leftover && !entries.empty() && (!owned || entries.size() > (lockSeen ? 2u : 1u))) {
                SetError(e, L"UnownedBusinessClaimLeftover", node.path, ERROR_INVALID_DATA);
                return TextError(error, e);
            }
        }
        std::wstring mp = n + L"\\" + MarkerName;
        TempFile created;
        created.handle.reset(OpenRelative(pins.last(), MarkerName, GENERIC_WRITE | DELETE, 0, FILE_CREATE,
                                          FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM,
                                          FILE_NON_DIRECTORY_FILE));
        if (!created.handle.valid()) {
            SetError(e, L"CreateOwnershipMarker", mp, GetLastError());
            return TextError(error, e);
        }
        DWORD written = 0;
        if (!WriteFile(created.handle.get(), MarkerData, sizeof(MarkerData) - 1, &written, nullptr) ||
            written != sizeof(MarkerData) - 1) {
            SetError(e, L"WriteOwnershipMarker", mp, GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
            return TextError(error, e);
        }
        if (!FlushFileBuffers(created.handle.get())) {
            SetError(e, L"FlushOwnershipMarker", mp, GetLastError());
            return TextError(error, e);
        }
        created.committed = true;
        created.handle.reset();
        return ProbeWritable(n, error);
    } catch (...) {
        FileError e;
        SetError(e, L"ClaimBusinessRootException", dir, ERROR_UNHANDLED_EXCEPTION);
        return TextError(error, e);
    }
}
bool ProbeWritable(const std::wstring& dir, std::wstring& error) {
    try {
        FileError e;
        std::wstring n, name;
        Pins pins;
        TempFile temp;
        if (!Normalize(dir, n, e) || !PinDirs(n, false, pins, e) || !MakeTemp(n, temp, name, e, pins.last()))
            return TextError(error, e);
        DWORD written = 0;
        const char value = 'P';
        if (!WriteFile(temp.handle.get(), &value, 1, &written, nullptr)) {
            SetError(e, L"WriteProbe", name, GetLastError());
            return TextError(error, e);
        }
        if (written != 1) {
            SetError(e, L"WriteProbe", name, ERROR_WRITE_FAULT);
            return TextError(error, e);
        }
        if (!FlushFileBuffers(temp.handle.get())) {
            SetError(e, L"FlushProbe", name, GetLastError());
            return TextError(error, e);
        }
        if (!Dispose(temp.handle.get())) {
            SetError(e, L"DeleteProbe", name, GetLastError());
            return TextError(error, e);
        }
        temp.committed = true;
        temp.handle.reset();
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        FileError e;
        SetError(e, L"WriteProbeException", dir, ERROR_UNHANDLED_EXCEPTION);
        return TextError(error, e);
    }
}

bool CopyFileChecked(const std::wstring& src, const std::wstring& dst, FileError& error,
                     const CancelFn& cancel) {
    try {
        error = FileError{};
        std::wstring s, d;
        Pins sp, dp;
        if (CheckCancel(cancel))
            return SetError(error, L"CancelCopy", dst, ERROR_CANCELLED);
        if (!Normalize(src, s, error) || !Normalize(dst, d, error))
            return false;
        if (pathpolicy::EqualPath(s, d))
            return SetError(error, L"SameSourceAndTarget", d, ERROR_INVALID_PARAMETER);
        if (!PinDirs(Parent(s), false, sp, error, cancel))
            return false;
        // Never create destination directories until the source has been opened.
        ScopedHandle source(OpenRelative(sp.last(), s.substr(s.find_last_of(L'\\') + 1), GENERIC_READ,
                                         FILE_SHARE_READ, FILE_OPEN, FILE_ATTRIBUTE_NORMAL,
                                         FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY));
        if (!source.valid())
            return SetError(error, L"OpenSource", s, GetLastError());
        BY_HANDLE_FILE_INFORMATION sourceInfo{};
        if (!Info(source.get(), sourceInfo, s, error, false))
            return false;
        if (CheckCancel(cancel))
            return SetError(error, L"CancelCopy", dst, ERROR_CANCELLED);
        if (!PinDirs(Parent(d), true, dp, error, cancel))
            return false;
        if (CheckCancel(cancel))
            return SetError(error, L"CancelBeforeStaging", d, ERROR_CANCELLED);
        if (!VerifyPins(sp, s, error) || !VerifyPins(dp, d, error))
            return false;
        DWORD attrs;
        if (!Attributes(d, attrs, error, true, dp.last()))
            return false;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            return SetError(error, L"TypeConflictTargetDirectory", d, ERROR_ALREADY_EXISTS);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            ScopedHandle target(OpenRelative(dp.last(), d.substr(d.find_last_of(L'\\') + 1),
                                             FILE_READ_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             FILE_OPEN, FILE_ATTRIBUTE_NORMAL, FILE_NON_DIRECTORY_FILE));
            if (!target.valid())
                return SetError(error, L"InspectTarget", d, GetLastError());
            BY_HANDLE_FILE_INFORMATION targetInfo{};
            if (!Info(target.get(), targetInfo, d, error, false))
                return false;
            if (Id(sourceInfo) == Id(targetInfo))
                return SetError(error, L"SameFileIdentity", d, ERROR_INVALID_PARAMETER);
        }
        TempFile temp;
        std::wstring tempName;
        if (!MakeTemp(Parent(d), temp, tempName, error, dp.last()))
            return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(source.get(), &size))
            return SetError(error, L"GetSourceSize", s, GetLastError());
        if (size.QuadPart < 0)
            return SetError(error, L"InvalidSourceSize", s, ERROR_INVALID_DATA);
        std::vector<char> buf(256 * 1024);
        LONGLONG readTotal = 0;
        while (readTotal < size.QuadPart) {
            if (CheckCancel(cancel))
                return SetError(error, L"CancelCopy", d, ERROR_CANCELLED);
            DWORD request = static_cast<DWORD>((size.QuadPart - readTotal) < static_cast<LONGLONG>(buf.size())
                                                   ? size.QuadPart - readTotal
                                                   : buf.size());
            DWORD got = 0;
            if (!ReadFile(source.get(), buf.data(), request, &got, nullptr))
                return SetError(error, L"ReadSource", s, GetLastError());
            if (got == 0)
                return SetError(error, L"ReadSourceShort", s, ERROR_HANDLE_EOF);
            DWORD offset = 0;
            while (offset < got) {
                if (CheckCancel(cancel))
                    return SetError(error, L"CancelCopy", d, ERROR_CANCELLED);
                DWORD written = 0;
                if (!WriteFile(temp.handle.get(), buf.data() + offset, got - offset, &written, nullptr))
                    return SetError(error, L"WriteTemporaryFile", tempName, GetLastError());
                if (!written)
                    return SetError(error, L"WriteTemporaryFileShort", tempName, ERROR_WRITE_FAULT);
                offset += written;
            }
            readTotal += got;
        }
        LARGE_INTEGER after{};
        if (!GetFileSizeEx(source.get(), &after))
            return SetError(error, L"RecheckSourceSize", s, GetLastError());
        if (after.QuadPart != size.QuadPart)
            return SetError(error, L"SourceChanged", s, ERROR_FILE_INVALID);
        if (!SetFileTime(temp.handle.get(), &sourceInfo.ftCreationTime, nullptr, &sourceInfo.ftLastWriteTime))
            return SetError(error, L"SetTemporaryFileTime", tempName, GetLastError());
        if (!FlushFileBuffers(temp.handle.get()))
            return SetError(error, L"FlushTemporaryFile", tempName, GetLastError());
        if (CheckCancel(cancel))
            return SetError(error, L"CancelBeforeCommit", d, ERROR_CANCELLED);
        if (!VerifyPins(sp, s, error) || !VerifyPins(dp, d, error))
            return false;
        // Recheck type immediately before commit. A raced reparse file is never
        // followed by the rename, but is still refused when observed.
        if (!Attributes(d, attrs, error, true, dp.last()))
            return false;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            return SetError(error, L"TypeConflictTargetDirectory", d, ERROR_ALREADY_EXISTS);
        if (!RenameHandleChecked(temp.handle.get(), d, error, true))
            return false;
        temp.committed = true;
        error = FileError{};
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return SetError(error, L"CopyExceptionOrCallback", dst, ERROR_UNHANDLED_EXCEPTION);
    }
}
bool OpenFileChecked(const std::wstring& path, HANDLE& out, DWORD access, DWORD share, DWORD creation,
                     FileError& error) {
    out = INVALID_HANDLE_VALUE;
    try {
        error = FileError{};
        ULONG disposition;
        if (creation == OPEN_EXISTING)
            disposition = FILE_OPEN;
        else if (creation == CREATE_NEW)
            disposition = FILE_CREATE;
        else if (creation == OPEN_ALWAYS)
            disposition = FILE_OPEN_IF;
        else
            return SetError(error, L"RefuseTruncatingOpen", path, ERROR_INVALID_PARAMETER);
        std::wstring normalized;
        Pins parent;
        if (!Normalize(path, normalized, error) || !PinDirs(Parent(normalized), false, parent, error))
            return false;
        ScopedHandle file(OpenRelative(parent.last(), normalized.substr(normalized.find_last_of(L'\\') + 1),
                                       access | FILE_READ_ATTRIBUTES, share, disposition,
                                       FILE_ATTRIBUTE_NORMAL, FILE_NON_DIRECTORY_FILE));
        if (!file.valid())
            return SetError(error, L"OpenFileRelative", path, GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!Info(file.get(), info, path, error, false) || !VerifyPins(parent, path, error))
            return false;
        out = file.release();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return SetError(error, L"OpenFileCheckedException", path, ERROR_UNHANDLED_EXCEPTION);
    }
}
bool RenameHandleChecked(HANDLE source, const std::wstring& destination, FileError& error, bool replace) {
    try {
        error = FileError{};
        std::wstring normalized;
        Pins parent;
        if (!Normalize(destination, normalized, error) || !PinDirs(Parent(normalized), false, parent, error))
            return false;
        BY_HANDLE_FILE_INFORMATION sourceInfo{};
        if (!GetFileInformationByHandle(source, &sourceInfo))
            return SetError(error, L"InspectRenameSource", destination, GetLastError());
        if (sourceInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return SetError(error, L"RejectReparseRenameSource", destination, ERROR_CANT_ACCESS_FILE);
        DWORD attributes;
        if (!Attributes(normalized, attributes, error, true, parent.last()))
            return false;
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (!replace || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (sourceInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return SetError(error, L"RenameTargetExistsOrTypeConflict", destination,
                                ERROR_ALREADY_EXISTS);
        }
        if (!VerifyPins(parent, destination, error))
            return false;
        std::wstring name = normalized.substr(normalized.find_last_of(L'\\') + 1);
        size_t bytes = sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t);
        std::vector<unsigned char> buffer(bytes, 0);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = replace ? TRUE : FALSE;
        rename->RootDirectory = parent.last();
        rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
        using NtSetInfoFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
        using NtErrorFn = ULONG(WINAPI*)(NTSTATUS);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto setInfo = reinterpret_cast<NtSetInfoFn>(GetProcAddress(ntdll, "NtSetInformationFile"));
        auto convert = reinterpret_cast<NtErrorFn>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        if (!setInfo || !convert)
            return SetError(error, L"RenameApiUnavailable", destination, ERROR_PROC_NOT_FOUND);
        IO_STATUS_BLOCK statusBlock{};
        NTSTATUS status = setInfo(source, &statusBlock, rename, static_cast<ULONG>(bytes),
                                  static_cast<FILE_INFORMATION_CLASS>(10));
        if (status < 0)
            return SetError(error, L"RenameRelativeCommit", destination, convert(status));
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return SetError(error, L"RenameHandleException", destination, ERROR_UNHANDLED_EXCEPTION);
    }
}
bool OpenDirectoryChecked(const std::wstring& path, HANDLE& opened, DWORD access, DWORD share,
                          FileError& error) {
    opened = INVALID_HANDLE_VALUE;
    try {
        std::wstring normalized;
        Pins parents;
        if (!Normalize(path, normalized, error) || !PinDirs(Parent(normalized), false, parents, error))
            return false;
        ScopedHandle directory(OpenRelative(parents.last(),
                                            normalized.substr(normalized.find_last_of(L'\\') + 1), access,
                                            share, FILE_OPEN, FILE_ATTRIBUTE_NORMAL, FILE_DIRECTORY_FILE));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!directory.valid())
            return SetError(error, L"OpenCheckedDirectory", path, GetLastError());
        if (!Info(directory.get(), info, path, error, true))
            return false;
        opened = directory.release();
        return true;
    } catch (...) {
        return SetError(error, L"OpenDirectoryException", path, ERROR_UNHANDLED_EXCEPTION);
    }
}
bool MoveDirectoryChecked(const std::wstring& src, const std::wstring& dst, FileError& error,
                          const BY_HANDLE_FILE_INFORMATION* expected) {
    try {
        error = FileError{};
        std::wstring normalized;
        Pins parent;
        if (!Normalize(src, normalized, error) || !PinDirs(Parent(normalized), false, parent, error))
            return false;
        ScopedHandle directory(OpenRelative(
            parent.last(), normalized.substr(normalized.find_last_of(L'\\') + 1),
            DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OPEN, FILE_ATTRIBUTE_NORMAL, FILE_DIRECTORY_FILE));
        if (!directory.valid())
            return SetError(error, L"OpenDirectoryMoveSource", src, GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!Info(directory.get(), info, src, error, true))
            return false;
        if (expected && (info.dwVolumeSerialNumber != expected->dwVolumeSerialNumber ||
                         info.nFileIndexHigh != expected->nFileIndexHigh ||
                         info.nFileIndexLow != expected->nFileIndexLow))
            return SetError(error, L"MoveSourceIdentityChanged", src, ERROR_INVALID_DATA);
        return RenameHandleChecked(directory.get(), dst, error, false);
    } catch (...) {
        return SetError(error, L"MoveDirectoryException", src, ERROR_UNHANDLED_EXCEPTION);
    }
}
bool RemoveEmptyDirectoryChecked(const std::wstring& path, FileError& error) {
    try {
        error = FileError{};
        std::wstring normalized;
        Pins parent;
        if (!Normalize(path, normalized, error) || !PinDirs(Parent(normalized), false, parent, error))
            return false;
        ScopedHandle directory(OpenRelative(parent.last(),
                                            normalized.substr(normalized.find_last_of(L'\\') + 1),
                                            DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                            FILE_ATTRIBUTE_NORMAL, FILE_DIRECTORY_FILE));
        if (!directory.valid()) {
            DWORD code = GetLastError();
            if (Missing(code)) {
                SetLastError(ERROR_SUCCESS);
                return true;
            }
            return SetError(error, L"OpenEmptyDirectory", path, code);
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!Info(directory.get(), info, path, error, true))
            return false;
        std::vector<DirectoryEntry> entries;
        if (!ReadDirectoryEntries(directory.get(), path, entries, error))
            return false;
        if (!entries.empty())
            return SetError(error, L"RefuseNonemptyDirectoryRemoval", path, ERROR_DIR_NOT_EMPTY);
        if (!Dispose(directory.get()))
            return SetError(error, L"RemoveDirectoryByHandle", path, GetLastError());
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return SetError(error, L"RemoveDirectoryException", path, ERROR_UNHANDLED_EXCEPTION);
    }
}
bool CopyFileForce(const std::wstring& src, const std::wstring& dst) {
    FileError e;
    return CopyFileChecked(src, dst, e);
}
MirrorResult ClearTreeEntriesChecked(const std::wstring& root, const std::vector<std::wstring>& files,
                                     const std::vector<std::wstring>& directories, const CancelFn& cancel,
                                     const std::function<void(const std::wstring&)>& beforeDelete,
                                     bool removeRoot, const ValidatePinnedTree& validator) {
    MirrorResult result;
    try {
        FileError error;
        std::wstring base;
        Pins boundary;
        auto stopped = [&] {
            if (CheckCancel(cancel)) {
                result.cancelled = true;
                return true;
            }
            return false;
        };
        if (stopped()) {
            Summary(result);
            return result;
        }
        if (!Normalize(root, base, error) ||
            !PinDirs(removeRoot ? Parent(base) : base, false, boundary, error)) {
            AddError(result, error);
            Summary(result);
            return result;
        }
        ScopedHandle rootObject;
        HANDLE rootHandle = boundary.last();
        if (removeRoot) {
            rootObject.reset(OpenRelative(boundary.last(), base.substr(base.find_last_of(L'\\') + 1),
                                          DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ, FILE_OPEN, FILE_ATTRIBUTE_NORMAL,
                                          FILE_DIRECTORY_FILE));
            if (!rootObject.valid()) {
                SetError(error, L"PinCleanupRoot", root, GetLastError());
                AddError(result, error);
                Summary(result);
                return result;
            }
            rootHandle = rootObject.get();
        }
        BY_HANDLE_FILE_INFORMATION rootIdentity{};
        if (!Info(rootHandle, rootIdentity, root, error, true)) {
            AddError(result, error);
            Summary(result);
            return result;
        }
        using Less = pathpolicy::CaseInsensitiveLess;
        std::map<std::wstring, std::unique_ptr<ScopedHandle>, Less> heldDirectories;
        std::vector<std::pair<std::wstring, std::unique_ptr<ScopedHandle>>> heldFiles;
        auto parentOf = [](const std::wstring& relative) {
            const auto slash = relative.find_last_of(L'\\');
            return slash == std::wstring::npos ? std::wstring() : relative.substr(0, slash);
        };
        auto parentHandle = [&](const std::wstring& relative) {
            const auto parent = parentOf(relative);
            auto it = heldDirectories.find(parent);
            return parent.empty()                ? rootHandle
                   : it == heldDirectories.end() ? INVALID_HANDLE_VALUE
                                                 : it->second->get();
        };
        auto safeRelative = [&](const std::wstring& relative) {
            std::wstring normalized;
            return !relative.empty() && Normalize(base + L"\\" + relative, normalized, error) &&
                   pathpolicy::EqualPath(normalized, base + L"\\" + relative);
        };
        auto ordered = directories;
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) { return a.size() < b.size(); });
        for (const auto& relative : ordered) {
            if (stopped()) {
                Summary(result);
                return result;
            }
            if (!safeRelative(relative) || parentHandle(relative) == INVALID_HANDLE_VALUE) {
                SetError(error, L"InvalidCleanupDirectory", relative, ERROR_INVALID_NAME);
                AddError(result, error);
                Summary(result);
                return result;
            }
            auto handle = std::make_unique<ScopedHandle>(
                OpenRelative(parentHandle(relative), relative.substr(relative.find_last_of(L'\\') + 1),
                             DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, FILE_OPEN,
                             FILE_ATTRIBUTE_NORMAL, FILE_DIRECTORY_FILE));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!handle->valid() || !Info(handle->get(), info, relative, error, true)) {
                if (!handle->valid())
                    SetError(error, L"PinCleanupDirectory", relative, GetLastError());
                AddError(result, error);
                Summary(result);
                return result;
            }
            if (!heldDirectories.emplace(relative, std::move(handle)).second) {
                SetError(error, L"DuplicateCleanupDirectory", relative, ERROR_DUP_NAME);
                AddError(result, error);
                Summary(result);
                return result;
            }
        }
        for (const auto& relative : files) {
            if (stopped()) {
                Summary(result);
                return result;
            }
            if (!safeRelative(relative) || parentHandle(relative) == INVALID_HANDLE_VALUE) {
                SetError(error, L"InvalidCleanupFile", relative, ERROR_INVALID_NAME);
                AddError(result, error);
                Summary(result);
                return result;
            }
            auto handle = std::make_unique<ScopedHandle>(
                OpenRelative(parentHandle(relative), relative.substr(relative.find_last_of(L'\\') + 1),
                             GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ, FILE_OPEN, FILE_ATTRIBUTE_NORMAL, FILE_NON_DIRECTORY_FILE));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!handle->valid() || !Info(handle->get(), info, relative, error, false)) {
                if (!handle->valid())
                    SetError(error, L"PinCleanupFile", relative, GetLastError());
                AddError(result, error);
                Summary(result);
                return result;
            }
            heldFiles.emplace_back(relative, std::move(handle));
        }
        // Build child inventories once: avoid O(directory_count * file_count).
        std::map<std::wstring, std::set<std::wstring, Less>, Less> expectedChildren;
        for (const auto& relative : files)
            expectedChildren[parentOf(relative)].insert(relative.substr(relative.find_last_of(L'\\') + 1));
        for (const auto& relative : directories)
            expectedChildren[parentOf(relative)].insert(relative.substr(relative.find_last_of(L'\\') + 1));
        // Validate the complete physical layout against the exact held objects.
        auto checkLayout = [&] {
            auto inspect = [&](HANDLE handle, const std::wstring& relative) {
                BY_HANDLE_FILE_INFORMATION identity{};
                if (!Info(handle, identity, relative, error, true))
                    return false;
                std::vector<DirectoryEntry> actual;
                if (!ReadDirectoryEntries(handle, relative, actual, error, cancel))
                    return false;
                const auto& expected = expectedChildren[relative];
                if (actual.size() != expected.size() ||
                    std::any_of(actual.begin(), actual.end(), [&](const auto& entry) {
                        return (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                               !expected.count(entry.name);
                    }))
                    return SetError(error, L"UnknownCleanupContent", relative, ERROR_DIR_NOT_EMPTY);
                return true;
            };
            if (removeRoot && !inspect(rootHandle, L""))
                return false;
            for (const auto& item : heldDirectories)
                if (!inspect(item.second->get(), item.first))
                    return false;
            return true;
        };
        if (!checkLayout()) {
            result.cancelled = error.code == ERROR_CANCELLED;
            AddError(result, error);
            Summary(result);
            return result;
        }
        if (validator) {
            std::vector<PinnedTreeFile> views;
            for (const auto& item : heldFiles) {
                BY_HANDLE_FILE_INFORMATION identity{};
                if (!Info(item.second->get(), identity, item.first, error, false)) {
                    AddError(result, error);
                    Summary(result);
                    return result;
                }
                views.push_back({item.first, item.second->get(), identity});
            }
            for (const auto& item : heldDirectories) {
                BY_HANDLE_FILE_INFORMATION identity{};
                if (!Info(item.second->get(), identity, item.first, error, true)) {
                    AddError(result, error);
                    Summary(result);
                    return result;
                }
                views.push_back({item.first, item.second->get(), identity, true});
            }
            if (!validator(rootIdentity, views, error) || !checkLayout()) {
                AddError(result, error);
                Summary(result);
                return result;
            }
        }
        for (auto& item : heldFiles) {
            if (beforeDelete)
                beforeDelete(item.first);
            if (stopped()) {
                Summary(result);
                return result;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!Info(item.second->get(), info, item.first, error, false)) {
                AddError(result, error);
                Summary(result);
                return result;
            }
            if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                FILE_BASIC_INFO basic{};
                basic.FileAttributes = info.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY;
                if (!basic.FileAttributes)
                    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (!SetFileInformationByHandle(item.second->get(), FileBasicInfo, &basic, sizeof(basic))) {
                    SetError(error, L"ClearCleanupReadOnly", item.first, GetLastError());
                    AddError(result, error);
                    Summary(result);
                    return result;
                }
            }
            if (!Dispose(item.second->get())) {
                SetError(error, L"DisposeCleanupFile", item.first, GetLastError());
                AddError(result, error);
                Summary(result);
                return result;
            }
            item.second->reset();
            ++result.deleted;
        }
        std::reverse(ordered.begin(), ordered.end());
        for (const auto& relative : ordered) {
            if (beforeDelete)
                beforeDelete(L"[directory] " + relative);
            if (stopped()) {
                Summary(result);
                return result;
            }
            auto& handle = heldDirectories.at(relative);
            std::vector<DirectoryEntry> actual;
            if (!ReadDirectoryEntries(handle->get(), relative, actual, error, cancel) || !actual.empty() ||
                !Dispose(handle->get())) {
                SetError(error, L"DisposeCleanupDirectory", relative,
                         actual.empty() ? GetLastError() : ERROR_DIR_NOT_EMPTY);
                AddError(result, error);
                Summary(result);
                return result;
            }
            handle->reset();
        }
        if (removeRoot) {
            if (beforeDelete)
                beforeDelete(L"[root]");
            if (stopped()) {
                Summary(result);
                return result;
            }
            BY_HANDLE_FILE_INFORMATION identity{};
            std::vector<DirectoryEntry> remaining;
            if (!Info(rootHandle, identity, root, error, true) ||
                !ReadDirectoryEntries(rootHandle, root, remaining, error, cancel) || !remaining.empty() ||
                !Dispose(rootHandle)) {
                if (error.code == ERROR_SUCCESS)
                    SetError(error, L"DisposePinnedRoot", root, GetLastError());
                AddError(result, error);
                Summary(result);
                return result;
            }
        }
        result.complete = true;
    } catch (...) {
        FileError error;
        SetError(error, L"ClearTreeException", root, ERROR_UNHANDLED_EXCEPTION);
        AddError(result, error);
    }
    Summary(result);
    return result;
}
bool DeleteFileChecked(const std::wstring& path, FileError& error) {
    try {
        error = FileError{};
        std::wstring n;
        Pins parents;
        if (!Normalize(path, n, error))
            return false;
        if (!PinDirs(Parent(n), false, parents, error)) {
            if (Missing(error.code)) {
                error = FileError{};
                SetLastError(ERROR_SUCCESS);
                return true;
            }
            return false;
        }
        ScopedHandle h(OpenRelative(parents.last(), n.substr(n.find_last_of(L'\\') + 1),
                                    DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                                    FILE_OPEN, FILE_ATTRIBUTE_NORMAL, FILE_NON_DIRECTORY_FILE));
        if (!h.valid()) {
            DWORD code = GetLastError();
            if (Missing(code)) {
                SetLastError(ERROR_SUCCESS);
                return true;
            }
            return SetError(error, L"OpenDeleteTarget", n, code);
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!Info(h.get(), info, n, error, false))
            return false;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
            FILE_BASIC_INFO basic{};
            basic.FileAttributes = info.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY;
            if (!basic.FileAttributes)
                basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileInformationByHandle(h.get(), FileBasicInfo, &basic, sizeof(basic)))
                return SetError(error, L"ClearReadOnlyForDelete", n, GetLastError());
        }
        if (!Dispose(h.get()))
            return SetError(error, L"DeleteByHandle", n, GetLastError());
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return SetError(error, L"DeleteException", path, ERROR_UNHANDLED_EXCEPTION);
    }
}
bool DeleteFileForce(const std::wstring& path) {
    FileError e;
    return DeleteFileChecked(path, e);
}
bool Exists(const std::wstring& path) {
    try {
        BY_HANDLE_FILE_INFORMATION info{};
        FileError e;
        return GetPathInfo(path, info, e);
    } catch (...) {
        SetLastError(ERROR_UNHANDLED_EXCEPTION);
        return false;
    }
}
bool IsDir(const std::wstring& path) {
    try {
        BY_HANDLE_FILE_INFORMATION info{};
        FileError e;
        if (!GetPathInfo(path, info, e))
            return false;
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            SetLastError(ERROR_DIRECTORY);
            return false;
        }
        return true;
    } catch (...) {
        SetLastError(ERROR_UNHANDLED_EXCEPTION);
        return false;
    }
}
long long FileSize(const std::wstring& path) {
    try {
        BY_HANDLE_FILE_INFORMATION info{};
        FileError e;
        if (!GetPathInfo(path, info, e))
            return -1;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            SetLastError(ERROR_DIRECTORY);
            return -1;
        }
        return (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    } catch (...) {
        SetLastError(ERROR_UNHANDLED_EXCEPTION);
        return -1;
    }
}
bool FileReadable(const std::wstring& path) {
    try {
        FileError e;
        std::wstring n;
        Pins parents;
        if (!Normalize(path, n, e) || !PinDirs(Parent(n), false, parents, e))
            return false;
        ScopedHandle h(OpenRelative(parents.last(), n.substr(n.find_last_of(L'\\') + 1), GENERIC_READ,
                                    FILE_SHARE_READ, FILE_OPEN, FILE_ATTRIBUTE_NORMAL,
                                    FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY));
        if (!h.valid())
            return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!Info(h.get(), info, n, e, false))
            return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h.get(), &sz))
            return false;
        std::vector<char> buf(64 * 1024);
        LONGLONG remaining = sz.QuadPart;
        while (remaining > 0) {
            DWORD nread = static_cast<DWORD>(remaining < static_cast<LONGLONG>(buf.size()) ? remaining
                                                                                           : buf.size()),
                  got = 0;
            if (!ReadFile(h.get(), buf.data(), nread, &got, nullptr))
                return false;
            if (!got) {
                SetLastError(ERROR_HANDLE_EOF);
                return false;
            }
            remaining -= got;
        }
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        SetLastError(ERROR_UNHANDLED_EXCEPTION);
        return false;
    }
}

namespace {
MirrorResult Transfer(const std::wstring& src, const std::wstring& dst, const ProgressFn& progress,
                      bool mirror) {
    MirrorResult r;
    try {
        FileError e;
        std::wstring s, d, reason;
        Pins srcPins, dstPins;
        ScopedHandle marker;
        int done = 0, total = 0;
        std::wstring current;
        CancelFn cancel = [&]() { return progress && !progress(done, total, current); };
        auto cancelled = [&]() {
            if (CheckCancel(cancel)) {
                r.cancelled = true;
                return true;
            }
            return false;
        };
        if (cancelled()) {
            Summary(r);
            return r;
        }
        if (!Normalize(src, s, e) || !Normalize(dst, d, e)) {
            AddError(r, e);
            Summary(r);
            return r;
        }
        if (!PinDirs(s, false, srcPins, e, cancel)) {
            r.cancelled = e.code == ERROR_CANCELLED;
            AddError(r, e);
            Summary(r);
            return r;
        }
        if (!pathpolicy::AreDisjoint(s, d, reason)) {
            SetError(e, L"RejectRootOverlap", d, GetLastError());
            AddError(r, e);
            Summary(r);
            return r;
        }
        auto source = ListFilesChecked(s, cancel);
        if (!source.complete) {
            r.cancelled = source.cancelled;
            for (const auto& error : source.errors)
                AddError(r, error);
            Summary(r);
            return r;
        }
        if (source.files.empty()) {
            r.skipped = true;
            r.complete = true;
            Summary(r);
            return r;
        }
        if (cancelled()) {
            Summary(r);
            return r;
        }
        if (!PinDirs(d, !mirror, dstPins, e, cancel)) {
            r.cancelled = e.code == ERROR_CANCELLED;
            AddError(r, e);
            Summary(r);
            return r;
        }
        if (mirror && !Marker(d, marker, e, dstPins.last())) {
            AddError(r, e);
            Summary(r);
            return r;
        }
        // Preflight the ENTIRE existing target before the first copy, including
        // type conflicts. Never delete a directory tree to make a file fit.
        auto target = ListFilesChecked(d, cancel);
        if (!target.complete) {
            r.cancelled = target.cancelled;
            for (const auto& error : target.errors)
                AddError(r, error);
            Summary(r);
            return r;
        }
        std::set<std::wstring, pathpolicy::CaseInsensitiveLess> sf(source.files.begin(), source.files.end());
        std::set<std::wstring, pathpolicy::CaseInsensitiveLess> sd(source.directories.begin(),
                                                                   source.directories.end());
        std::set<std::wstring, pathpolicy::CaseInsensitiveLess> tf(target.files.begin(), target.files.end());
        std::set<std::wstring, pathpolicy::CaseInsensitiveLess> td(target.directories.begin(),
                                                                   target.directories.end());
        for (const auto& rel : sf)
            if (td.count(rel)) {
                SetError(e, L"TypeConflictTargetDirectory", d + L"\\" + rel, ERROR_ALREADY_EXISTS);
                AddError(r, e);
                Summary(r);
                return r;
            }
        for (const auto& rel : sd)
            if (tf.count(rel)) {
                SetError(e, L"TypeConflictTargetFile", d + L"\\" + rel, ERROR_DIRECTORY);
                AddError(r, e);
                Summary(r);
                return r;
            }
        total = static_cast<int>(source.files.size());
        for (const auto& rel : source.files) {
            current = rel;
            if (cancelled()) {
                Summary(r);
                return r;
            }
            if (!CopyFileChecked(s + L"\\" + rel, d + L"\\" + rel, e, cancel)) {
                if (e.code == ERROR_CANCELLED)
                    r.cancelled = true;
                else
                    AddError(r, e);
                Summary(r);
                return r; // fail closed; never enter deletion
            }
            ++r.copied;
            r.copiedPaths.push_back(rel);
            ++done;
        }
        current = L"";
        if (cancelled()) {
            Summary(r);
            return r;
        }
        if (mirror) {
            // Repeat source enumeration before destructive work. Directory churn,
            // access errors, reparse insertion and case collisions all forbid deletion.
            auto verify = ListFilesChecked(s, cancel);
            if (!verify.complete) {
                r.cancelled = verify.cancelled;
                for (const auto& error : verify.errors)
                    AddError(r, error);
                Summary(r);
                return r;
            }
            if (verify.files != source.files || verify.directories != source.directories) {
                SetError(e, L"SourceChangedBeforeDeletion", s, ERROR_FILE_INVALID);
                AddError(r, e);
                Summary(r);
                return r;
            }
            // Fresh COMPLETE target enumeration is mandatory too.
            auto finalTarget = ListFilesChecked(d, cancel);
            if (!finalTarget.complete) {
                r.cancelled = finalTarget.cancelled;
                for (const auto& error : finalTarget.errors)
                    AddError(r, error);
                Summary(r);
                return r;
            }
            for (const auto& rel : finalTarget.files) {
                if (sf.count(rel))
                    continue;
                current = L"[delete] " + rel;
                if (cancelled()) {
                    Summary(r);
                    return r;
                }
                if (!DeleteFileChecked(d + L"\\" + rel, e)) {
                    AddError(r, e);
                    Summary(r);
                    return r;
                }
                ++r.deleted;
            }
        }
        r.complete = true;
    } catch (...) {
        FileError e;
        SetError(e, L"TransferExceptionOrCallback", dst, ERROR_UNHANDLED_EXCEPTION);
        AddError(r, e);
    }
    Summary(r);
    return r;
}
} // namespace
MirrorResult MirrorDir(const std::wstring& src, const std::wstring& dst, const ProgressFn& progress) {
    return Transfer(src, dst, progress, true);
}
MirrorResult CopyTreeChecked(const std::wstring& src, const std::wstring& dst, const ProgressFn& progress) {
    return Transfer(src, dst, progress, false);
}
int CopyTree(const std::wstring& src, const std::wstring& dst, std::vector<std::wstring>* busyOut,
             const ProgressFn& progress) {
    auto r = CopyTreeChecked(src, dst, progress);
    if (busyOut)
        try {
            for (const auto& e : r.errors)
                if (Busy(e.code))
                    busyOut->push_back(e.path);
        } catch (...) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        }
    return r.copied;
}
} // namespace fileops
