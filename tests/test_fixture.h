// Shared fixtures for isolated owned-cache tests.
#pragma once
#include "file_ops.h"
#include "test_isolation.h"
#include <json.hpp>
#include <stdexcept>
#include <string>
namespace testutil {
inline void OwnCache(const std::wstring& cache) {
    std::wstring error;
    if (!fileops::PrepareDedicatedRoot(cache, error))
        throw std::runtime_error("Cannot prepare isolated owned cache");
}
inline std::string ReadBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(Native(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot read fixture metadata");
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart < 0 || size.QuadPart > 1048576) {
        CloseHandle(h);
        throw std::runtime_error("Invalid fixture metadata size");
    }
    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD n = 0;
    BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &n, nullptr);
    CloseHandle(h);
    if (!ok || n != out.size())
        throw std::runtime_error("Incomplete fixture metadata read");
    return out;
}
} // namespace testutil
