#include "game_detect.h"
#include "../common/limits_shared.h"
#include "../common/winutil.h"
#include "utf.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <set>
#include <stdexcept>
#include <tlhelp32.h>
#include <vector>
#include <windows.h>
#ifdef LOL_AUX_TEST_HOOKS
#include "../../tests/private/aux_test_hooks.h"
#endif
namespace core {
namespace {
thread_local std::wstring detectionError;
using winutil::ScopedHandle;
#ifdef LOL_AUX_TEST_HOOKS
thread_local aux_test::ScanFixture* scanFixture = nullptr;
#endif
ULONGLONG ScanClock() {
#ifdef LOL_AUX_TEST_HOOKS
    if (scanFixture && scanFixture->clock)
        return scanFixture->clock();
#endif
    return GetTickCount64();
}
struct ScanBudget {
    ULONGLONG start = ScanClock();
    size_t candidates = 0, entries = 0;
    bool exhausted = false;
    bool Continue() {
        exhausted = exhausted || ScanClock() - start >= 2000;
        return !exhausted;
    }
    bool Candidate() {
        size_t limit = 2048;
#ifdef LOL_AUX_TEST_HOOKS
        if (scanFixture)
            limit = scanFixture->candidateLimit;
#endif
        if (!Continue() || candidates >= limit) {
            exhausted = true;
            return false;
        }
        ++candidates;
#ifdef LOL_AUX_TEST_HOOKS
        if (scanFixture)
            ++scanFixture->candidates;
#endif
        return true;
    }
    bool Entry() {
        size_t limit = 400;
#ifdef LOL_AUX_TEST_HOOKS
        if (scanFixture)
            limit = scanFixture->entryLimit;
#endif
        if (!Continue() || entries >= limit) {
            exhausted = true;
            return false;
        }
        ++entries;
#ifdef LOL_AUX_TEST_HOOKS
        if (scanFixture)
            ++scanFixture->entries;
#endif
        return true;
    }
};
thread_local ScanBudget* activeBudget = nullptr;
bool ScanContinue() {
    return !activeBudget || activeBudget->Continue();
}
struct BudgetScope {
    explicit BudgetScope(ScanBudget& budget) {
        activeBudget = &budget;
    }
    ~BudgetScope() {
        activeBudget = nullptr;
    }
};
class ScopedFindHandle {
    HANDLE handle;

  public:
    explicit ScopedFindHandle(HANDLE value) : handle(value) {
#ifdef LOL_AUX_TEST_HOOKS
        if (valid() && scanFixture)
            ++scanFixture->opened;
#endif
    }
    ~ScopedFindHandle() {
        if (valid()) {
            FindClose(handle); // Search handles are never CloseHandle handles.
#ifdef LOL_AUX_TEST_HOOKS
            if (scanFixture)
                ++scanFixture->closed;
#endif
        }
    }
    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;
    bool valid() const {
        return handle != INVALID_HANDLE_VALUE && handle != nullptr;
    }
    HANDLE get() const {
        return handle;
    }
};

bool ValidPath(const std::wstring& path) {
    if (path.empty() || path.size() > 32700)
        return false;
    for (wchar_t c : path)
        if (c < 32)
            return false;
    std::string encoded;
    return TryWideToUtf8(path, encoded);
}
std::wstring Normalize(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\')
        path.pop_back();
    return path;
}
std::wstring Lower(std::wstring value) {
    for (auto& c : value)
        c = static_cast<wchar_t>(towlower(c));
    return value;
}
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos || slash <= 2 ? std::wstring() : path.substr(0, slash);
}
std::wstring Leaf(const std::wstring& path) {
    const auto slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
bool Directory(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(winutil::LongPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}
bool Exists(const std::wstring& path) {
    return GetFileAttributesW(winutil::LongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}
// Open the file rather than treating an existing directory named *.exe as executable.
// A minimal PE-header check strengthens the layout heuristic, but does NOT verify trust.
bool ExecutableFile(const std::wstring& path) {
    ScopedHandle file(CreateFileW(winutil::LongPath(path).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid())
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < static_cast<LONGLONG>(sizeof(IMAGE_DOS_HEADER)))
        return false;
    IMAGE_DOS_HEADER dos{};
    DWORD count = 0;
    if (!ReadFile(file.get(), &dos, sizeof(dos), &count, nullptr) || count != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        static_cast<LONGLONG>(dos.e_lfanew) + 4 > size.QuadPart)
        return false;
    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN))
        return false;
    DWORD signature = 0;
    return ReadFile(file.get(), &signature, sizeof(signature), &count, nullptr) &&
           count == sizeof(signature) && signature == IMAGE_NT_SIGNATURE;
}
bool Within(const std::vector<BYTE>& buffer, const void* ptr, size_t bytes) {
    const auto begin = reinterpret_cast<uintptr_t>(buffer.data());
    const auto p = reinterpret_cast<uintptr_t>(ptr);
    return p >= begin && p - begin <= buffer.size() && bytes <= buffer.size() - (p - begin);
}
class RegistryKey {
    HKEY key = nullptr;

  public:
    ~RegistryKey() {
        if (key)
            RegCloseKey(key);
    }
    HKEY get() const {
        return key;
    }
    HKEY* put() {
        return &key;
    }
};
bool RegistryString(HKEY key, const wchar_t* name, std::wstring& value) {
    DWORD type = 0, bytes = 0;
    LONG status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) ||
        bytes > 65534 || bytes % sizeof(wchar_t))
        return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        DWORD actual = bytes, actualType = 0;
        status = RegQueryValueExW(key, name, nullptr, &actualType, reinterpret_cast<BYTE*>(buffer.data()),
                                  &actual);
        if (status == ERROR_MORE_DATA) {
            if (actual <= bytes || actual > 65534 || actual % sizeof(wchar_t))
                return false;
            bytes = actual;
            continue;
        }
        if (status != ERROR_SUCCESS || (actualType != REG_SZ && actualType != REG_EXPAND_SZ) ||
            actual < sizeof(wchar_t) || actual > bytes || actual % sizeof(wchar_t))
            return false;
        const size_t chars = actual / sizeof(wchar_t);
        if (buffer[chars - 1] != L'\0' ||
            std::find(buffer.begin(), buffer.begin() + chars - 1, L'\0') != buffer.begin() + chars - 1)
            return false;
        std::wstring result(buffer.data(), chars - 1);
        if (actualType == REG_EXPAND_SZ) {
            DWORD needed = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);
            if (!needed || needed > 32700)
                return false;
            std::vector<wchar_t> expanded(needed, L'\0');
            DWORD copied = ExpandEnvironmentStringsW(result.c_str(), expanded.data(), needed);
            if (!copied || copied > needed)
                return false;
            result.assign(expanded.data(), copied - 1);
        }
        if (!ValidPath(result))
            return false;
        value = Normalize(result);
        return true;
    }
    return false;
}

// Foreign-only policy: exact path components, never substrings of unrelated names.
bool PathHasChinaTokens(const std::wstring& path) {
    const std::wstring low = Lower(path);
    const wchar_t* tokens[] = {L"wegame",  L"wegameapps", L"tencent", L"tencent games", L"tgp",
                               L"腾讯",    L"腾讯游戏",   L"tcls",    L"英雄联盟",      L"tenprotect",
                               L"qqgames", L"lol.qq",     L"timi"};
    size_t begin = 0;
    while (begin < low.size()) {
        const size_t end = low.find_first_of(L"\\/", begin);
        const std::wstring component = low.substr(begin, end == std::wstring::npos ? end : end - begin);
        for (const auto token : tokens)
            if (component == token)
                return true;
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
    }
    return false;
}
bool ChinaComponentsIn(const std::wstring& dir) {
    const wchar_t* markers[] = {L"TCLS",     L"TPHelper.exe",  L"TenProtect", L"TenioDL.exe",
                                L"Tenio",    L"WeGameApps",    L"WeGame",     L"英雄联盟",
                                L"腾讯游戏", L"CrossProxy.exe"};
    for (auto marker : markers)
        if (Exists(dir + L"\\" + marker))
            return true;
    return false;
}
bool ChinaInstall(const std::wstring& gameRoot) {
    if (PathHasChinaTokens(gameRoot))
        return true;
    // Only this game/client install owns these components. A sibling WeGame install
    // under a common Games/grandparent directory says nothing about this client.
    return ChinaComponentsIn(gameRoot) || ChinaComponentsIn(Parent(gameRoot));
}
// Positive foreign evidence, used only to keep diagnostics honest (never required to pass).
bool RiotClientNear(const std::wstring& gameRoot) {
    std::wstring dir = gameRoot;
    for (int depth = 0; depth < 4 && !dir.empty(); ++depth) {
        if (Exists(dir + L"\\Riot Client") || Exists(dir + L"\\RiotClientServices.exe"))
            return true;
        dir = Parent(dir);
    }
    const std::wstring client = Parent(gameRoot);
    return Lower(Leaf(client)) == L"league of legends" && Lower(Leaf(Parent(client))) == L"riot games";
}
bool TryCandidate(const std::wstring& candidate, std::wstring& found);
bool RunningClientCandidate(std::wstring& candidate) {
#ifdef LOL_AUX_TEST_HOOKS
    if (scanFixture) {
        for (const auto& game : scanFixture->runningCandidates)
            if (TryCandidate(game, candidate))
                return true;
        return false;
    }
#endif
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return false;
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry))
        return false;
    size_t visited = 0;
    do {
        if (!ScanContinue() || ++visited > 4096)
            break;
        if (_wcsicmp(entry.szExeFile, L"LeagueClientUx.exe") != 0 &&
            _wcsicmp(entry.szExeFile, L"LeagueClient.exe") != 0)
            continue;
        DWORD candidateSession = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &candidateSession) || candidateSession != session)
            continue;
        ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process.valid())
            continue;
        std::vector<wchar_t> buffer(common::kMaxPathChars);
        DWORD size = static_cast<DWORD>(buffer.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size))
            continue;
        const std::wstring image = Normalize(std::wstring(buffer.data(), size));
        const std::wstring install = Parent(image);
        if (install.empty())
            continue;
        const std::wstring game = install + L"\\Game";
        // A China/unreadable first process must not hide a later eligible foreign client.
        if (TryCandidate(game, candidate))
            return true;
    } while (Process32NextW(snapshot.get(), &entry));
    return false;
}
std::vector<std::wstring> DriveRoots() {
    std::vector<std::wstring> roots;
#ifdef LOL_AUX_TEST_HOOKS
    if (scanFixture) {
        for (const auto& drive : scanFixture->drives)
            if (drive.second == DRIVE_FIXED)
                roots.push_back(drive.first);
        return roots;
    }
#endif
    const DWORD mask = GetLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if (!(mask & (1u << (letter - L'A'))))
            continue;
        std::wstring root(1, letter);
        root += L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
            continue;
        roots.push_back(root);
    }
    return roots;
}
bool TryCandidate(const std::wstring& candidate, std::wstring& found) {
    if (!found.empty() || candidate.empty())
        return !found.empty();
    if (activeBudget && !activeBudget->Candidate())
        return false;
    if (GameDetect::IsGameRoot(candidate)) {
        found = Normalize(candidate);
        return true;
    }
    return false;
}
bool TryWithAdditions(const std::wstring& base, std::wstring& found) {
    if (base.empty() || found.size())
        return false;
    const wchar_t* additions[] = {L"", L"Game", L"League of Legends\\Game",
                                  L"Riot Games\\League of Legends\\Game"};
    for (auto addition : additions)
        if (TryCandidate(*addition ? base + L"\\" + addition : base, found))
            break;
    return !found.empty();
}
std::wstring Trim(std::wstring value) {
    const size_t first = value.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return L"";
    return value.substr(first, value.find_last_not_of(L" \t") - first + 1);
}
bool IconIndex(const std::wstring& input) {
    const auto value = Trim(input);
    size_t i = !value.empty() && (value[0] == L'-' || value[0] == L'+') ? 1 : 0;
    if (i == value.size())
        return false;
    for (; i < value.size(); ++i)
        if (value[i] < L'0' || value[i] > L'9')
            return false;
    return true;
}
std::wstring DisplayIconPath(const std::wstring& input) {
    if (input.empty() || input.size() > 32700)
        return L"";
    auto value = Trim(input);
    if (value.empty())
        return L"";
    if (value.front() == L'"') {
        const auto end = value.find(L'"', 1);
        if (end == std::wstring::npos)
            return L"";
        const auto suffix = Trim(value.substr(end + 1));
        if (!suffix.empty() && (suffix.front() != L',' || !IconIndex(suffix.substr(1))))
            return L"";
        value = value.substr(1, end - 1);
    } else {
        const auto comma = value.find_last_of(L',');
        if (comma != std::wstring::npos && IconIndex(value.substr(comma + 1)))
            value = Trim(value.substr(0, comma));
        if (value.find(L'"') != std::wstring::npos)
            return L"";
    }
    return ValidPath(value) ? Normalize(value) : L"";
}
bool UninstallScan(const std::wstring& path, std::wstring& found) {
    RegistryKey root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, root.put()) !=
        ERROR_SUCCESS)
        return false;
    for (DWORD index = 0; index < 1024 && ScanContinue(); ++index) {
        wchar_t name[512]{};
        DWORD length = 512;
        if (RegEnumKeyExW(root.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        RegistryKey entry;
        if (RegOpenKeyExW(root.get(), name, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, entry.put()) !=
            ERROR_SUCCESS)
            continue;
        std::wstring display, publisher, location;
        if (!RegistryString(entry.get(), L"DisplayName", display))
            continue;
        const std::wstring lowName = Lower(display);
        if (lowName.find(L"league of legends") == std::wstring::npos)
            continue;
        if (lowName.find(L"英雄联盟") != std::wstring::npos)
            continue;
        const bool publisherForeign = !RegistryString(entry.get(), L"Publisher", publisher) ||
                                      Lower(publisher).find(L"riot") != std::wstring::npos;
        if (!publisherForeign)
            continue;
        if (RegistryString(entry.get(), L"InstallLocation", location) && TryWithAdditions(location, found))
            return true;
        if (RegistryString(entry.get(), L"DisplayIcon", location)) {
            location = Parent(DisplayIconPath(location));
            if (TryWithAdditions(location, found))
                return true;
        }
    }
    return false;
}
} // namespace
#ifdef LOL_AUX_TEST_HOOKS
void aux_test::SetScanFixture(ScanFixture* fixture) {
    scanFixture = fixture;
}
std::wstring aux_test::ParseDisplayIcon(const std::wstring& value) {
    return DisplayIconPath(value);
}
#endif
std::wstring GameDetect::LastError() {
    return detectionError;
}
std::wstring GameDetect::GetCompanyName(const std::wstring& exePath) {
    detectionError = L"无法读取有效的 CompanyName 版本资源；这不是数字签名验证结果。";
    try {
        if (!ValidPath(exePath))
            return L"";
        const std::wstring path = winutil::LongPath(exePath);
        DWORD ignored = 0;
        DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        if (!size || size > 16 * 1024 * 1024)
            return L"";
        std::vector<BYTE> buffer(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data()))
            return L"";
        struct Translation {
            WORD language;
            WORD codePage;
        };
        Translation* translations = nullptr;
        UINT length = 0;
        if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                            reinterpret_cast<void**>(&translations), &length) ||
            length < sizeof(Translation) || length % sizeof(Translation) ||
            !Within(buffer, translations, length))
            return L"";
        for (size_t i = 0; i < length / sizeof(Translation); ++i) {
            wchar_t query[64]{};
            swprintf(query, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", translations[i].language,
                     translations[i].codePage);
            wchar_t* company = nullptr;
            UINT chars = 0;
            if (!VerQueryValueW(buffer.data(), query, reinterpret_cast<void**>(&company), &chars) ||
                chars < 2 || chars > 65536 ||
                !Within(buffer, company, static_cast<size_t>(chars) * sizeof(wchar_t)) ||
                company[chars - 1] != L'\0')
                continue;
            std::wstring result(company, chars - 1);
            if (!ValidPath(result))
                continue;
            detectionError.clear();
            return result; // UNSIGNED metadata, not identity or signature evidence.
        }
    } catch (...) {
        return L"";
    }
    return L"";
}
bool GameDetect::IsGameRoot(const std::wstring& path) {
    detectionError.clear();
    try {
        if (!ValidPath(path)) {
            detectionError = L"游戏目录包含无效路径字符。";
            return false;
        }
        std::wstring root = Normalize(path);
        if (!Directory(root)) {
            detectionError = L"游戏目录不存在或无法访问。";
            return false;
        }
        if (PathHasChinaTokens(root)) {
            detectionError = L"检测到国服/腾讯路径特征，本工具只支持外服客户端。";
            return false;
        }
        const std::wstring parent = Parent(root);
        if (parent.empty()) {
            detectionError = L"游戏目录没有客户端父目录。";
            return false;
        }
        if (!ExecutableFile(root + L"\\League of Legends.exe") ||
            !ExecutableFile(parent + L"\\LeagueClient.exe")) {
            detectionError = L"游戏或客户端不是可读取的普通 PE 文件。";
            return false;
        }
        if (ChinaInstall(root)) {
            detectionError = L"安装目录包含国服组件（TCLS/TenProtect/WeGame 等），只支持外服客户端。";
            return false;
        }
        const std::wstring company = Lower(GetCompanyName(parent + L"\\LeagueClient.exe"));
        if (company.find(L"tencent") != std::wstring::npos || company.find(L"腾讯") != std::wstring::npos ||
            company.find(L"timi") != std::wstring::npos) {
            detectionError = L"客户端版本资源属于腾讯/国服（未验证数字签名）。";
            return false;
        }
        // Unknown CompanyName stays compatible: this is a layout heuristic, not authentication.
        detectionError.clear();
        return true;
    } catch (...) {
        detectionError = L"游戏目录检测异常或内存不足。";
        return false;
    }
}
std::wstring GameDetect::Find(const std::wstring& explicitPath, bool deep) {
    detectionError.clear();
    try {
        if (deep) {
            detectionError = L"不支持深度扫描；未执行自动磁盘扫描，请使用普通查找或手动指定 Game 目录。";
            return L"";
        }
        if (!explicitPath.empty()) {
            if (IsGameRoot(explicitPath))
                return Normalize(explicitPath);
            return L"";
        }
        ScanBudget budget;
        BudgetScope budgetScope(budget);
        std::wstring found;
        // 1) Check every running client hint until a foreign candidate actually validates.
        std::wstring running;
        if (RunningClientCandidate(running))
            return Normalize(running);
#ifdef LOL_AUX_TEST_HOOKS
        if (!scanFixture) {
#endif
            // 2) Riot's own registry keys.
            HKEY hives[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
            REGSAM views[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY, 0};
            for (HKEY hive : hives)
                for (REGSAM view : views) {
                    if (!budget.Continue())
                        break;
                    RegistryKey key;
                    if (RegOpenKeyExW(hive, L"SOFTWARE\\Riot Games\\League of Legends", 0,
                                      KEY_QUERY_VALUE | view, key.put()) != ERROR_SUCCESS)
                        continue;
                    std::wstring base;
                    if (!RegistryString(key.get(), L"InstallLocation", base) &&
                        !RegistryString(key.get(), L"Path", base))
                        continue;
                    if (TryWithAdditions(base, found))
                        return found;
                }
            // 3) Uninstall entries published by Riot (Tencent entries are filtered by name/publisher).
            const wchar_t* uninstallKeys[] = {
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"};
            for (auto keyPath : uninstallKeys)
                if (UninstallScan(keyPath, found))
                    return found;
#ifdef LOL_AUX_TEST_HOOKS
        }
#endif
        // 4) Standard Riot installation layouts on fixed local drives only.
        const auto drives = DriveRoots();
        const wchar_t* relative[] = {L"Riot Games\\League of Legends\\Game",
                                     L"Program Files\\Riot Games\\League of Legends\\Game",
                                     L"Program Files (x86)\\Riot Games\\League of Legends\\Game",
                                     L"Games\\Riot Games\\League of Legends\\Game",
                                     L"Games\\League of Legends\\Game",
                                     L"League of Legends\\Game",
                                     L"Program Files\\League of Legends\\Game",
                                     L"Riot Games\\League of Legends"};
        for (const auto& drive : drives)
            for (auto suffix : relative)
                if (TryCandidate(drive + suffix, found))
                    return found;
        // 5) Bounded one-level scan of drive roots for custom folders (no system trees).
        std::set<std::wstring> skip = {
            L"windows",     L"programdata", L"$recycle.bin", L"system volume information",
            L"recovery",    L"users",       L"perflogs",     L"$windows.~bt",
            L"$windows.~ws"};
        for (const auto& drive : drives) {
            if (!budget.Continue())
                break;
            WIN32_FIND_DATAW data{};
            ScopedFindHandle search(FindFirstFileW(winutil::LongPath(drive + L"*").c_str(), &data));
            if (!search.valid())
                continue;
#ifdef LOL_AUX_TEST_HOOKS
            if (scanFixture && scanFixture->throwAfterOpen)
                throw std::runtime_error("injected enumeration failure");
#endif
            do {
                // Count every enumerated entry, including files/skipped/reparse names.
                if (!budget.Entry())
                    break;
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                    continue;
                if (skip.count(Lower(data.cFileName)))
                    continue;
                const std::wstring base = drive + data.cFileName;
                const wchar_t* nested[] = {
                    L"\\League of Legends\\Game", L"\\Riot Games\\League of Legends\\Game",
                    L"\\Games\\League of Legends\\Game", L"\\Games\\Riot Games\\League of Legends\\Game"};
                for (auto suffix : nested)
                    if (TryCandidate(base + suffix, found))
                        return found;
            } while (FindNextFileW(search.get(), &data));
        }
        if (budget.exhausted) {
            detectionError = L"自动查找达到 2 秒/400 条目录项/2048 个候选的协作预算；请手动指定 Game 目录。";
            return L"";
        }
        detectionError = L"未找到符合外服布局的安装目录（国服客户端不会被采用）；仍可手动指定 Game 目录。";
    } catch (...) {
        detectionError = L"游戏路径查找异常或内存不足。";
    }
    return L"";
}
} // namespace core
