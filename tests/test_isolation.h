// Isolated test data only. No shell, fixed directories, registry or production defaults.
#pragma once
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace testutil {
inline std::set<std::wstring>& OwnedRoots() {
    static std::set<std::wstring> roots;
    return roots;
}
inline std::wstring Native(std::wstring p) {
    // Extended Win32 paths do not accept '/' separators. Test paths use both styles.
    for (auto& c : p)
        if (c == L'/')
            c = L'\\';
    if (p.rfind(L"\\\\?\\", 0) == 0)
        return p;
    if (p.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}
inline void RemoveTreeImpl(const std::wstring& path) {
    const auto native = Native(path);
    const DWORD attr = GetFileAttributesW(native.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return;
        throw std::runtime_error("Cannot inspect isolated test directory");
    }
    // Never traverse a symlink/junction, including one created inside a test.
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((native + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            try {
                do {
                    if (std::wstring(fd.cFileName) != L"." && std::wstring(fd.cFileName) != L"..")
                        RemoveTreeImpl(path + L"\\" + fd.cFileName);
                } while (FindNextFileW(h, &fd));
                if (GetLastError() != ERROR_NO_MORE_FILES)
                    throw std::runtime_error("Cannot enumerate test directory");
            } catch (...) {
                FindClose(h);
                throw;
            }
            FindClose(h);
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("Cannot enumerate test directory");
    }
    if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT) && (attr & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(native.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    const BOOL ok =
        (attr & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(native.c_str()) : DeleteFileW(native.c_str());
    if (!ok)
        throw std::runtime_error("Cannot remove isolated test data");
}
inline void ResetOwnedDirectory(const std::wstring& root) {
    if (!OwnedRoots().count(root))
        throw std::runtime_error("Refusing to clean an unowned test directory");
    RemoveTreeImpl(root);
    if (!CreateDirectoryW(Native(root).c_str(), nullptr))
        throw std::runtime_error("Cannot recreate isolated test directory");
}
class Directory {
    std::wstring root_;

  public:
    explicit Directory(const wchar_t* tag) {
        wchar_t temp[32768]{};
        const DWORD n = GetTempPathW(32768, temp);
        if (!n || n >= 32768)
            throw std::runtime_error("Cannot locate temporary directory");
        for (unsigned i = 0; i < 1000; ++i) {
            root_ = std::wstring(temp) + L"LOLBackupV2-test-" + tag + L"-" +
                    std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                    std::to_wstring(i);
            if (CreateDirectoryW(Native(root_).c_str(), nullptr)) {
                OwnedRoots().insert(root_);
                return;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                throw std::runtime_error("Cannot create isolated test directory");
        }
        throw std::runtime_error("Cannot allocate unique isolated test directory");
    }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    const std::wstring& path() const {
        return root_;
    }
    ~Directory() {
        try {
            RemoveTreeImpl(root_);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Test cleanup failed: %s\n", e.what());
        }
        OwnedRoots().erase(root_);
    }
};
} // namespace testutil
