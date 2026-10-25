#include "path_policy.h"
#include "../common/limits_shared.h"
#include "../common/winutil.h"
#include <algorithm>
#include <memory>
#include <tuple>
#include <vector>
#include <winternl.h>

namespace pathpolicy {
namespace {
bool Fail(std::wstring& error, DWORD code, const wchar_t* message) {
    try {
        error = message;
        error += L" (Win32=" + std::to_wstring(code) + L")";
    } catch (...) {
    }
    SetLastError(code);
    return false;
}
bool Eq(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}
bool Reserved(const std::wstring& part) {
    std::wstring stem = part.substr(0, part.find(L'.'));
    if (Eq(stem, L"CON") || Eq(stem, L"PRN") || Eq(stem, L"AUX") || Eq(stem, L"NUL") || Eq(stem, L"CONIN$") ||
        Eq(stem, L"CONOUT$"))
        return true;
    if (stem.size() == 4 && (Eq(stem.substr(0, 3), L"COM") || Eq(stem.substr(0, 3), L"LPT"))) {
        wchar_t c = stem[3];
        return (c >= L'0' && c <= L'9') || c == 0xB9 || c == 0xB2 || c == 0xB3;
    }
    return false;
}
bool Spelling(const std::wstring& path, std::wstring& out, std::wstring& error) {
    // Check ordering: NormalizeRoot(root, root, error) is a supported call.
    std::wstring p = path;
    out.clear();
    error.clear();
    if (p.empty())
        return Fail(error, ERROR_INVALID_NAME, L"Empty path is forbidden");
    if (p.size() >= 32700)
        return Fail(error, ERROR_FILENAME_EXCED_RANGE, L"Path exceeds safe Win32 length");
    for (wchar_t c : p) {
        if (c < 32 || c == 127)
            return Fail(error, ERROR_INVALID_NAME, L"NUL/control characters are forbidden");
    }
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.size() >= 8 && Eq(p.substr(0, 8), L"\\\\?\\UNC\\"))
        p = L"\\\\" + p.substr(8);
    else if (p.compare(0, 4, L"\\\\?\\") == 0)
        p = p.substr(4);
    if (p.compare(0, 4, L"\\\\.\\") == 0 || p.compare(0, 4, L"\\??\\") == 0)
        return Fail(error, ERROR_INVALID_NAME, L"Device namespace is forbidden");
    bool drive = p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
                 p[1] == L':' && p[2] == L'\\';
    bool unc = p.size() >= 5 && p[0] == L'\\' && p[1] == L'\\';
    if (!drive && !unc)
        return Fail(error, ERROR_BAD_PATHNAME, L"An absolute drive or UNC path is required");
    size_t start = drive ? 3 : 2;
    // Remove trailing separators but not the drive root separator.
    while (p.size() > start && p.back() == L'\\')
        p.pop_back();
    size_t count = 0;
    for (size_t i = start; i < p.size();) {
        size_t end = p.find(L'\\', i);
        if (end == std::wstring::npos)
            end = p.size();
        std::wstring part = p.substr(i, end - i);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ')
            return Fail(error, ERROR_INVALID_NAME, L"Ambiguous path component is forbidden");
        if (part.size() > 255)
            return Fail(error, ERROR_FILENAME_EXCED_RANGE, L"Path component exceeds 255 characters");
        if (part.find_first_of(L"<>:\"|?*") != std::wstring::npos || Reserved(part))
            return Fail(error, ERROR_INVALID_NAME, L"Reserved or invalid path component");
        ++count;
        i = end + 1;
    }
    if ((drive && count == 0) || (unc && count <= 2))
        return Fail(error, ERROR_ACCESS_DENIED, L"Drive/share roots are forbidden");
    out = p;
    SetLastError(ERROR_SUCCESS);
    return true;
}
bool DirectoryInfo(HANDLE h, std::wstring& error) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info))
        return Fail(error, GetLastError(), L"Cannot query path identity");
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return Fail(error, ERROR_CANT_ACCESS_FILE, L"Reparse points are forbidden");
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return Fail(error, ERROR_DIRECTORY, L"Existing root component is not a directory");
    struct CaseFlags {
        ULONG Flags;
    } flags{};
    if (GetFileInformationByHandleEx(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(23), &flags, sizeof(flags)) &&
        (flags.Flags & 1))
        return Fail(error, ERROR_NOT_SUPPORTED, L"Case-sensitive directories are unsupported");
    return true;
}
HANDLE OpenDirectoryRelative(HANDLE parent, const std::wstring& name) {
    using CreateFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                      PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using ErrorFn = ULONG(WINAPI*)(NTSTATUS);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "NtCreateFile"));
    auto convert = reinterpret_cast<ErrorFn>(GetProcAddress(module, "RtlNtStatusToDosError"));
    if (!create || !convert) {
        SetLastError(ERROR_PROC_NOT_FOUND);
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
        create(&result, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &object, &io, nullptr,
               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
               FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (status < 0) {
        SetLastError(convert(status));
        return INVALID_HANDLE_VALUE;
    }
    return result;
}
// Existing ancestors deny DELETE sharing while being examined. WRITE sharing
// is required for ordinary atomic file replacement in related service jobs.
using Identity = std::tuple<DWORD, DWORD, DWORD>;
using Anchors = std::vector<std::pair<Identity, std::wstring>>;
// File operations keep their own longer-lived pins until all writes are over.
bool Canonical(const std::wstring& p, std::wstring& canonical, std::wstring& error,
               Anchors* anchors = nullptr) {
    size_t root = RootLength(p);
    std::vector<std::unique_ptr<winutil::ScopedHandle>> handles;
    canonical.clear();
    size_t previous = root;
    for (size_t end = root;;) {
        std::wstring cur = p.substr(0, end);
        for (const auto& existing : handles)
            if (!DirectoryInfo(existing->get(), error))
                return false;
        HANDLE raw =
            handles.empty()
                ? CreateFileW(LongPath(cur).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)
                : OpenDirectoryRelative(handles.back()->get(), cur.substr(cur.find_last_of(L'\\') + 1));
        if (raw == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (!handles.empty() && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
                while (!canonical.empty() && canonical.back() == L'\\')
                    canonical.pop_back();
                canonical += L"\\" + p.substr(previous == root && p[root] != L'\\' ? root : previous + 1);
                SetLastError(ERROR_SUCCESS);
                return true;
            }
            return Fail(error, e, L"Cannot validate root component");
        }
        winutil::ScopedHandle one(raw);
        if (!DirectoryInfo(raw, error))
            return false;
        if (anchors) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(raw, &info))
                return Fail(error, GetLastError(), L"Cannot query ancestor identity");
            std::wstring suffix = p.substr(end);
            if (!suffix.empty() && suffix.front() == L'\\')
                suffix.erase(0, 1);
            anchors->push_back(
                {{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow}, suffix});
        }
        std::vector<wchar_t> buf(common::kMaxPathChars);
        DWORD n = GetFinalPathNameByHandleW(raw, buf.data(), static_cast<DWORD>(buf.size()),
                                            FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
        if (!n)
            n = GetFinalPathNameByHandleW(raw, buf.data(), static_cast<DWORD>(buf.size()),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!n || n >= buf.size())
            return Fail(error, n ? ERROR_FILENAME_EXCED_RANGE : GetLastError(),
                        L"Cannot resolve final root path");
        canonical.assign(buf.data(), n);
        auto owner = std::unique_ptr<winutil::ScopedHandle>(new winutil::ScopedHandle());
        owner->reset(one.release());
        handles.push_back(std::move(owner));
        if (end == p.size())
            break;
        size_t next = p.find(L'\\', end == root && p[root] != L'\\' ? root : end + 1);
        if (next == std::wstring::npos)
            next = p.size();
        previous = end;
        end = next;
    }
    while (!canonical.empty() && canonical.back() == L'\\')
        canonical.pop_back();
    SetLastError(ERROR_SUCCESS);
    return true;
}
bool Overlaps(const std::wstring& a, const std::wstring& b) {
    if (Eq(a, b))
        return true;
    return a.size() < b.size() && b[a.size()] == L'\\' && Eq(a, b.substr(0, a.size()));
}
} // namespace

size_t RootLength(const std::wstring& p) {
    if (p.size() >= 3 && p[1] == L':')
        return 3;
    size_t server = p.find(L'\\', 2);
    if (server == std::wstring::npos)
        return p.size();
    size_t share = p.find(L'\\', server + 1);
    return share == std::wstring::npos ? p.size() : share;
}
std::wstring LongPath(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0)
        return p;
    return p.compare(0, 2, L"\\\\") == 0 ? L"\\\\?\\UNC\\" + p.substr(2) : L"\\\\?\\" + p;
}
bool NormalizePath(const std::wstring& p, std::wstring& out, std::wstring& error) {
    try {
        return Spelling(p, out, error);
    } catch (...) {
        out.clear();
        return Fail(error, ERROR_NOT_ENOUGH_MEMORY, L"Path validation failed");
    }
}
bool NormalizeRoot(const std::wstring& p, std::wstring& out, std::wstring& error) {
    try {
        if (!Spelling(p, out, error))
            return false;
        std::wstring canonical;
        if (!Canonical(out, canonical, error)) {
            out.clear();
            return false;
        }
        return true;
    } catch (...) {
        out.clear();
        return Fail(error, ERROR_NOT_ENOUGH_MEMORY, L"Root validation failed");
    }
}
bool ValidateRoot(const std::wstring& p, std::wstring& error) {
    std::wstring out;
    return NormalizeRoot(p, out, error);
}
bool AreDisjoint(const std::wstring& a, const std::wstring& b, std::wstring& error) {
    try {
        std::wstring na, nb, ca, cb;
        Anchors aa, ab;
        if (!Spelling(a, na, error) || !Spelling(b, nb, error) || !Canonical(na, ca, error, &aa) ||
            !Canonical(nb, cb, error, &ab))
            return false;
        if (Overlaps(ca, cb) || Overlaps(cb, ca))
            return Fail(error, ERROR_INVALID_PARAMETER, L"Roots are identical or have an ancestor overlap");
        // File IDs close UNC-server/8.3/drive-alias gaps in final-path spelling.
        // Suffixes are compared relative to every shared existing ancestor, so
        // nonexisting tails and ancestor overlaps are handled as well.
        for (const auto& x : aa)
            for (const auto& y : ab) {
                if (x.first != y.first)
                    continue;
                if (x.second.empty() || y.second.empty() || Overlaps(x.second, y.second) ||
                    Overlaps(y.second, x.second))
                    return Fail(error, ERROR_INVALID_PARAMETER, L"Root identities overlap");
            }
        error.clear();
        SetLastError(ERROR_SUCCESS);
        return true;
    } catch (...) {
        return Fail(error, ERROR_NOT_ENOUGH_MEMORY, L"Root isolation check failed");
    }
}
bool EqualPath(const std::wstring& a, const std::wstring& b) {
    return Eq(a, b);
}
bool CaseInsensitiveLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                TRUE) == CSTR_LESS_THAN;
}
} // namespace pathpolicy
