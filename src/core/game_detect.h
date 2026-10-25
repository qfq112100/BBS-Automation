// Installation discovery is a WEAK HEURISTIC, not signature or publisher authentication.
// Version-resource CompanyName is untrusted metadata and can be forged.
#pragma once
#include <string>
namespace core {
class GameDetect {
  public:
    // Checks actual regular executable files + layout + exclusion heuristics.
    // A true result means plausible installation only; never authorizes executing code.
    static bool IsGameRoot(const std::wstring& path);
    // Blocking worker-thread API (never call on a UI thread). Explicit paths may be remote.
    // Automatic scan uses fixed drives only and cooperative 2 s / 400 entries / 2048 candidates.
    // Individual filesystem/registry calls cannot be forcibly timed out; no recursive crawl.
    // Explicit path, common locations, and registry. deep=true is NOT supported:
    // returns empty and LastError explains that no deep scan was performed.
    static std::wstring Find(const std::wstring& explicitPath, bool deep = false);
    // Reads unsigned version-resource metadata only. This is NOT a digital signature.
    static std::wstring GetCompanyName(const std::wstring& exePath);
    static std::wstring LastError();
    static const wchar_t* DetectionMethod() {
        return L"文件布局与未认证的版本资源启发式（非数字签名认证）";
    }
};
} // namespace core
