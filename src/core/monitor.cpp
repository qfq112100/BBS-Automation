#include "monitor.h"
#include "../common/limits_shared.h"
#include "../common/winutil.h"
#include "utf.h"
#include <algorithm>
#include <tlhelp32.h>
#include <vector>
namespace core {
namespace {
LobbyProbe Unknown(const std::wstring& message, DWORD code) {
    return {LobbyState::Unknown, code, message};
}
bool IsName(const wchar_t* name) {
    return _wcsicmp(name, L"LeagueClient.exe") == 0 || _wcsicmp(name, L"LeagueClientUx.exe") == 0;
}
bool ValidPath(const std::wstring& path) {
    if (path.empty() || path.size() > 32700)
        return false;
    for (wchar_t c : path)
        if (c < 32)
            return false;
    std::string encoded;
    return TryWideToUtf8(path, encoded);
}
bool CanonicalFile(const std::wstring& path, std::wstring& canonical, DWORD& error) {
    if (!ValidPath(path)) {
        error = ERROR_INVALID_NAME;
        return false;
    }
    winutil::ScopedHandle file(CreateFileW(winutil::LongPath(path).c_str(), FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        error = GetLastError();
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) {
        error = GetLastError();
        return false;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        error = ERROR_DIRECTORY;
        return false;
    }
    DWORD needed = GetFinalPathNameByHandleW(file.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!needed) {
        error = GetLastError();
        return false;
    }
    if (needed > 32767) {
        error = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }
    std::vector<wchar_t> buffer(needed + 1);
    DWORD copied = GetFinalPathNameByHandleW(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!copied || copied >= buffer.size()) {
        error = GetLastError();
        return false;
    }
    canonical.assign(buffer.data(), copied);
    return true;
}
} // namespace
static LobbyProbe ProbeInstallation(const std::wstring& gameRoot, bool game, bool writeSafety = false) {
    try {
        if (!ValidPath(gameRoot))
            return Unknown(L"游戏安装路径为空或无效，无法确定大厅状态。", ERROR_INVALID_NAME);
        std::wstring root = gameRoot;
        std::replace(root.begin(), root.end(), L'/', L'\\');
        while (!root.empty() && root.back() == L'\\')
            root.pop_back();
        const size_t slash = root.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash == 0)
            return Unknown(L"游戏目录没有有效的客户端父目录。", ERROR_INVALID_NAME);
        DWORD rootAttributes = GetFileAttributesW(winutil::LongPath(root).c_str());
        if (rootAttributes == INVALID_FILE_ATTRIBUTES)
            return Unknown(L"配置游戏目录不可访问。", GetLastError());
        if (!(rootAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return Unknown(L"配置游戏路径不是目录。", ERROR_DIRECTORY);
        const std::wstring parent = root.substr(0, slash);
        std::wstring expectedClient, expectedUx;
        DWORD clientError = 0, uxError = 0;
        bool clientExists =
            CanonicalFile(game ? root + L"\\League of Legends.exe" : parent + L"\\LeagueClient.exe",
                          expectedClient, clientError);
        bool uxExists = !game && CanonicalFile(parent + L"\\LeagueClientUx.exe", expectedUx, uxError);
        if (!clientExists)
            return Unknown(game ? L"无法验证配置目录中的游戏程序。"
                                : L"无法验证配置安装目录中的 LeagueClient.exe。",
                           clientError);
        // Some installations have no UX executable until patching finishes.
        if (!game && !uxExists && uxError != ERROR_FILE_NOT_FOUND && uxError != ERROR_PATH_NOT_FOUND)
            return Unknown(L"无法验证配置安装目录中的 LeagueClientUx.exe。", uxError);
        DWORD currentSession = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession))
            return Unknown(L"无法查询当前 Windows 会话。", GetLastError());
        winutil::ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot.valid())
            return Unknown(L"创建进程快照失败；保留上次可信状态。", GetLastError());
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.get(), &entry)) {
            DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_FILES)
                return {LobbyState::Offline, ERROR_SUCCESS, L""};
            return Unknown(L"无法开始枚举进程；保留上次可信状态。", code);
        }
        LobbyProbe uncertain = {LobbyState::Offline, ERROR_SUCCESS, L""};
        do {
            if (game ? _wcsicmp(entry.szExeFile, L"League of Legends.exe") != 0 : !IsName(entry.szExeFile))
                continue;
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session)) {
                DWORD code = GetLastError();
                if (writeSafety || code != ERROR_INVALID_PARAMETER)
                    uncertain = Unknown(L"无法查询候选大厅/游戏进程会话。", code);
                continue;
            }
            if (!monitor_detail::IncludesSession(writeSafety, currentSession, session))
                continue;
            winutil::ScopedHandle process(
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (!process.valid()) {
                DWORD code = GetLastError();
                if (writeSafety || code != ERROR_INVALID_PARAMETER)
                    uncertain = Unknown(L"无法读取候选大厅/游戏进程路径。", code);
                continue;
            }
            std::vector<wchar_t> buffer(common::kMaxPathChars);
            DWORD size = static_cast<DWORD>(buffer.size());
            if (!QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size)) {
                DWORD code = GetLastError(), exitCode = STILL_ACTIVE;
                if (!writeSafety && GetExitCodeProcess(process.get(), &exitCode) && exitCode != STILL_ACTIVE)
                    continue;
                uncertain = Unknown(L"无法验证候选大厅进程映像。", code);
                continue;
            }
            std::wstring actual;
            DWORD code = 0;
            if (!CanonicalFile(std::wstring(buffer.data(), size), actual, code)) {
                uncertain = Unknown(L"无法解析候选大厅进程实际路径。", code);
                continue;
            }
            if (_wcsicmp(actual.c_str(), expectedClient.c_str()) == 0 ||
                (uxExists && _wcsicmp(actual.c_str(), expectedUx.c_str()) == 0))
                return {LobbyState::Online, ERROR_SUCCESS, L""};
        } while (Process32NextW(snapshot.get(), &entry));
        DWORD code = GetLastError();
        if (code != ERROR_NO_MORE_FILES)
            return Unknown(L"进程枚举未完成；保留上次可信状态。", code);
        return uncertain;
    } catch (...) {
        return Unknown(L"大厅/游戏进程检测遇到未分类异常；保留上次可信状态。", ERROR_UNHANDLED_EXCEPTION);
    }
}
LobbyProbe Monitor::Probe(const std::wstring& gameRoot) {
    return ProbeInstallation(gameRoot, false);
}
LobbyProbe Monitor::ProbeGame(const std::wstring& gameRoot) {
    return ProbeInstallation(gameRoot, true);
}
LobbyProbe Monitor::ProbeGameForWriteSafety(const std::wstring& gameRoot) {
    return ProbeInstallation(gameRoot, true, true);
}
bool Monitor::IsLobbyOnline() {
    DWORD current = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &current))
        return false;
    winutil::ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry))
        return false;
    do {
        if (IsName(entry.szExeFile)) {
            DWORD session = 0;
            if (ProcessIdToSessionId(entry.th32ProcessID, &session) && session == current)
                return true;
        }
    } while (Process32NextW(snapshot.get(), &entry));
    return false;
}
} // namespace core
