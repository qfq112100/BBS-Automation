#pragma once
#include <windows.h>
namespace platform {
// Desktop Windows 10 RTM (1507 / 10240) and newer. Architecture is a build
// choice: the primary x86 PE runs natively on x86 and through WOW64 on x64.
// Newer DPI/DWM features are optional at the UI boundary, never loader imports.
inline constexpr DWORD MinimumWindows10Build = 10240;
inline constexpr bool IsSupportedWindowsVersion(DWORD major, DWORD build) noexcept {
    return major > 10 || (major == 10 && build >= MinimumWindows10Build);
}
struct WindowsVersion {
    DWORD major = 0, minor = 0, build = 0;
};
inline bool QueryWindowsVersion(WindowsVersion& out) noexcept {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto query =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!query || query(&info) != 0)
        return false;
    out = {info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber};
    return true;
}
} // namespace platform
