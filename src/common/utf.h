// Strict UTF-8 / UTF-16 conversion. Legacy string-returning APIs fail closed.
#pragma once
#include <limits>
#include <string>
#include <windows.h>

inline bool TryUtf8ToWide(const std::string& input, std::wstring& output) noexcept {
    try {
        if (input.empty()) {
            output.clear();
            return true;
        }
        if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            return false;
        const int count = static_cast<int>(input.size());
        const int needed =
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), count, nullptr, 0);
        if (needed <= 0)
            return false;
        std::wstring converted(static_cast<size_t>(needed), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), count, &converted[0], needed) !=
            needed)
            return false;
        output.swap(converted);
        return true;
    } catch (...) {
        return false;
    }
}
inline bool TryWideToUtf8(const std::wstring& input, std::string& output) noexcept {
    try {
        if (input.empty()) {
            output.clear();
            return true;
        }
        if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            return false;
        const int count = static_cast<int>(input.size());
        const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), count, nullptr, 0,
                                               nullptr, nullptr);
        if (needed <= 0)
            return false;
        std::string converted(static_cast<size_t>(needed), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), count, &converted[0], needed,
                                nullptr, nullptr) != needed)
            return false;
        output.swap(converted);
        return true;
    } catch (...) {
        return false;
    }
}
inline std::wstring Utf8ToWide(const std::string& input) noexcept {
    std::wstring output;
    TryUtf8ToWide(input, output);
    return output;
}
inline std::string WideToUtf8(const std::wstring& input) noexcept {
    std::string output;
    TryWideToUtf8(input, output);
    return output;
}
