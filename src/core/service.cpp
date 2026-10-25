// All filesystem jobs are serialized; monitor, cancel and shutdown are independent.
#include "service.h"
#include "../../third_party/json/json.hpp"
#include "../common/limits_shared.h"
#include "../common/utf.h"
#include "../common/winutil.h"
#include "config.h"
#include "file_ops.h"
#include "game_detect.h"
#include "logger.h"
#include "monitor.h"
#include "path_policy.h"
#include "sync_engine.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace core {
using json = nlohmann::json;
namespace {
// Diagnostics never participate in cancellation, scheduling, or commit decisions.
std::atomic<bool> serviceLogReady{false};
std::wstring Limited(const std::wstring& text, size_t maximum = 1024) {
    if (text.size() <= maximum)
        return text;
    constexpr size_t suffixLength = 12; // L" [truncated]"
    size_t count = maximum > suffixLength ? maximum - suffixLength : 0;
    if (count && text[count - 1] >= 0xd800 && text[count - 1] <= 0xdbff)
        --count;
    return text.substr(0, count) + std::wstring(L" [truncated]", (std::min)(maximum, suffixLength));
}
template <class Make> void Audit(LogLevel level, const wchar_t* module, Make make) noexcept {
    const DWORD saved = GetLastError();
    try {
        const auto message = make();
        if (message.empty()) {
            SetLastError(saved);
            return;
        }
        if (serviceLogReady.load())
            Log().Log(level, module, message);
        else {
            const auto fallback = std::wstring(L"[service-audit] ") + module + L" " + message + L"\n";
            OutputDebugStringW(fallback.c_str());
        }
    } catch (...) {
        OutputDebugStringW(L"[service-audit] diagnostic unavailable\n");
    }
    SetLastError(saved);
}
bool QuietMethod(const std::wstring& method) noexcept {
    return method == L"get_status" || method == L"get_logs" || method == L"echo";
}
thread_local const std::wstring* auditMethod = nullptr;
thread_local bool* auditFailed = nullptr;
thread_local bool auditProbeSuppressed = false;
struct ProbeSilence {
    bool previous = auditProbeSuppressed;
    ProbeSilence() noexcept {
        auditProbeSuppressed = true;
    }
    ~ProbeSilence() noexcept {
        auditProbeSuppressed = previous;
    }
};
struct MethodScope {
    const std::wstring* previous;
    explicit MethodScope(const std::wstring& method) noexcept : previous(auditMethod) {
        auditMethod = &method;
    }
    ~MethodScope() noexcept {
        auditMethod = previous;
    }
};
struct CommandAudit {
    const std::wstring& method;
    unsigned long long traceId;
    ULONGLONG started = GetTickCount64();
    bool failed = false;
    int exceptionDepth = std::uncaught_exceptions();
    bool* previousFailure = auditFailed;
    MethodScope scope;
    CommandAudit(const std::wstring& name, unsigned long long trace, ULONGLONG queued) noexcept
        : method(name), traceId(trace), scope(name) {
        auditFailed = &failed;
        if (!QuietMethod(method))
            Audit(LogLevel::Info, L"command", [&] {
                return L"command.begin method=" + Limited(method, 64) + L" commandId=" +
                       std::to_wstring(traceId) + L" waitMs=" +
                       std::to_wstring(queued && started >= queued ? started - queued : 0);
            });
    }
    ~CommandAudit() noexcept {
        failed = failed || std::uncaught_exceptions() > exceptionDepth;
        if (!QuietMethod(method) || failed)
            Audit(failed ? LogLevel::Warn : LogLevel::Info, L"command", [&] {
                return L"command.complete method=" + Limited(method, 64) + L" commandId=" +
                       std::to_wstring(traceId) + L" result=" + (failed ? L"failed" : L"returned") +
                       L" durationMs=" + std::to_wstring(GetTickCount64() - started);
            });
        auditFailed = previousFailure;
    }
};
template <class Errors> void AuditFileErrors(const wchar_t* stage, const Errors& errors) noexcept {
    Audit(LogLevel::Warn, L"io", [&] {
        std::wstring message = std::wstring(stage) + L" errors=" + std::to_wstring(errors.size());
        const size_t count = (std::min)(errors.size(), size_t(10));
        for (size_t i = 0; i < count; ++i) {
            const auto& error = errors[i];
            message += L"\n[" + std::to_wstring(i + 1) + L"] " +
                       Limited(error.operation + L": " + error.path + L" Win32=" +
                               std::to_wstring(error.code) + L" " + error.message);
        }
        if (errors.size() > count)
            message += L"\nomitted=" + std::to_wstring(errors.size() - count);
        return message;
    });
}
constexpr const wchar_t* kBotFiles[] = {L"bot_5.dll", L"config.ini",  L"hc.dat",
                                        L"HID.dll",   L"shards.json", L"user.ini"};
constexpr int kBotFileCount = 6;
struct Finally {
    std::function<void()> f;
    ~Finally() noexcept {
        try {
            f();
        } catch (...) {
            Audit(LogLevel::Error, L"service",
                  [] { return std::wstring(L"finalizer.exception: completion callback/event unavailable"); });
        }
    }
};
std::wstring Pack(const json& value) {
    return Utf8ToWide(value.dump());
}
std::wstring Now() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t s[32]{};
    swprintf(s, 32, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    return s;
}
json StatusJson(const Service::Status& s) {
    return {{"installed", s.installed},
            {"auto_on", s.autoOn},
            {"lobby_online", s.lobbyOnline},
            {"monitor_known", s.monitorKnown},
            {"backup_state", WideToUtf8(s.backupState)},
            {"monitor_state", WideToUtf8(s.monitorState)},
            {"last_sync", WideToUtf8(s.lastSync)},
            {"next_sync", WideToUtf8(s.nextSync)},
            {"lobby_cycle", WideToUtf8(s.lobbyCycle)},
            {"periodic_ready", s.periodicReady},
            {"match_backup_ready", s.matchBackupReady},
            {"auto_state", WideToUtf8(s.autoState)},
            {"auto_reason", WideToUtf8(s.autoReason)},
            {"last_result", WideToUtf8(s.lastResult)},
            {"last_error", WideToUtf8(s.lastError)},
            {"game_root", WideToUtf8(s.gameRoot)},
            {"game_ok", s.gameOk},
            {"cache_entries", s.cacheEntries},
            {"theme", s.theme},
            {"autostart", s.autostart},
            {"close_action", s.closeAction},
            {"sync_minutes", s.syncMinutes},
            {"cache_root", WideToUtf8(s.cacheRoot)},
            {"progress_percent", s.progressPercent},
            {"progress_label", WideToUtf8(s.progressLabel)},
            {"operation_id", s.operationId}};
}
bool IntField(const json& j, const char* key, int lo, int hi, int& value, std::wstring& error) {
    if (!j.contains(key))
        return true;
    const auto& v = j.at(key);
    if (!v.is_number_integer() ||
        (v.is_number_unsigned() && v.get<unsigned long long>() > static_cast<unsigned long long>(hi))) {
        error = L"设置项必须是范围内的整数: " + Utf8ToWide(key);
        return false;
    }
    const auto n = v.get<long long>();
    if (n < lo || n > hi) {
        error = L"设置项超出允许范围: " + Utf8ToWide(key);
        return false;
    }
    value = static_cast<int>(n);
    return true;
}
bool BoolField(const json& j, const char* key, bool& value, std::wstring& error) {
    if (!j.contains(key))
        return true;
    if (!j.at(key).is_boolean()) {
        error = L"设置项必须为开关值: " + Utf8ToWide(key);
        return false;
    }
    value = j.at(key).get<bool>();
    return true;
}
bool PathField(const json& j, const char* key, std::wstring& value, std::wstring& error) {
    if (!j.contains(key))
        return true;
    if (!j.at(key).is_string() || !TryUtf8ToWide(j.at(key).get<std::string>(), value)) {
        error = L"目录必须为有效的文本";
        return false;
    }
    std::wstring normalized;
    if (!pathpolicy::NormalizeRoot(value, normalized, error))
        return false;
    value = std::move(normalized);
    return true;
}
template <class F> bool UpdateConfig(const std::wstring& path, F mutate) {
    return Config::UpdateAtomic(
        path,
        [](Config& config, void* context) -> bool {
            if constexpr (std::is_same_v<std::invoke_result_t<F, Config&>, bool>)
                return (*static_cast<F*>(context))(config);
            else {
                (*static_cast<F*>(context))(config);
                return true;
            }
        },
        &mutate);
}
struct RunValue {
    bool exists = false;
    DWORD type = REG_SZ;
    std::vector<BYTE> bytes;
};
bool ReadRunValue(RunValue& value, std::wstring& error) {
    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                            KEY_QUERY_VALUE, &key);
    if (rc == ERROR_FILE_NOT_FOUND)
        return true;
    if (rc != ERROR_SUCCESS) {
        error = L"无法读取原开机启动设置: " + std::to_wstring(rc);
        return false;
    }
    Finally close{[&] { RegCloseKey(key); }};
    DWORD size = 0;
    rc = RegQueryValueExW(key, L"LOLBackupV3", nullptr, &value.type, nullptr, &size);
    if (rc == ERROR_FILE_NOT_FOUND)
        return true;
    if (rc != ERROR_SUCCESS || size > 65536) {
        error = L"原开机启动设置无法安全读取";
        return false;
    }
    value.bytes.resize(size);
    rc = RegQueryValueExW(key, L"LOLBackupV3", nullptr, &value.type, value.bytes.data(), &size);
    if (rc != ERROR_SUCCESS) {
        error = L"开机启动设置在读取期间变化，请重试";
        return false;
    }
    value.bytes.resize(size);
    value.exists = true;
    return true;
}
bool RestoreRunValue(const RunValue& value, std::wstring& error) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                              nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        error = L"无法恢复原开机启动设置";
        return false;
    }
    Finally close{[&] { RegCloseKey(key); }};
    rc = value.exists ? RegSetValueExW(key, L"LOLBackupV3", 0, value.type, value.bytes.data(),
                                       static_cast<DWORD>(value.bytes.size()))
                      : RegDeleteValueW(key, L"LOLBackupV3");
    if (!value.exists && rc == ERROR_FILE_NOT_FOUND)
        rc = ERROR_SUCCESS;
    if (rc != ERROR_SUCCESS) {
        error = L"恢复原开机启动设置失败: " + std::to_wstring(rc);
        return false;
    }
    return true;
}
bool SameDirectory(const std::wstring& a, const std::wstring& b) {
    if (pathpolicy::EqualPath(a, b))
        return true;
    winutil::ScopedHandle ha(CreateFileW(winutil::LongPath(a).c_str(), FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    winutil::ScopedHandle hb(CreateFileW(winutil::LongPath(b).c_str(), FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    return ha.valid() && hb.valid() && GetFileInformationByHandle(ha.get(), &x) &&
           GetFileInformationByHandle(hb.get(), &y) && x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
           x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}
bool ChangeAutostart(bool on, std::wstring& error) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                              nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        error = L"无法写入开机启动设置，错误码 " + std::to_wstring(rc);
        return false;
    }
    Finally close{[&] { RegCloseKey(key); }};
    std::vector<wchar_t> path(common::kMaxPathChars);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) {
        error = L"无法取得当前程序路径";
        return false;
    }
    std::wstring command = L"\"" + std::wstring(path.data(), n) + L"\"";
    rc = on ? RegSetValueExW(key, L"LOLBackupV3", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                             static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
            : RegDeleteValueW(key, L"LOLBackupV3");
    if (!on && rc == ERROR_FILE_NOT_FOUND)
        rc = ERROR_SUCCESS;
    if (rc != ERROR_SUCCESS) {
        error = L"开机启动设置失败，错误码 " + std::to_wstring(rc);
        return false;
    }
    return true;
}
} // namespace
void Service::Stamp(Command& command) noexcept {
    if (!command.traceId)
        command.traceId = m_nextTrace.fetch_add(1) + 1;
    if (!command.enqueuedAt)
        command.enqueuedAt = GetTickCount64();
}
std::wstring Service::Trace(const Command& command) noexcept {
    const DWORD saved = GetLastError();
    try {
        auto trace = command.id > 0 ? L"request:" + std::to_wstring(command.id)
                                    : L"command:" + std::to_wstring(command.traceId);
        SetLastError(saved);
        return trace;
    } catch (...) {
        SetLastError(saved);
        return {};
    }
}
void Service::AuditProbe(bool game, int state, DWORD error, const wchar_t* reason, unsigned long long epoch,
                         bool writeSafety) noexcept {
    if (auditProbeSuppressed)
        return;
    const DWORD saved = GetLastError();
    try {
        const auto now = GetTickCount64();
        bool changed = false, emit = false;
        unsigned long long suppressed = 0;
        ULONGLONG since = 0;
        {
            std::lock_guard<std::mutex> lock(m_probeAuditMutex);
            auto& sample = m_probeAudit[game ? (writeSafety ? 2 : 1) : 0];
            changed = !sample.initialized || sample.rootEpoch != epoch || sample.state != state ||
                      sample.error != error || sample.reason != reason;
            emit = changed || (state < 0 && now - sample.loggedAt >= common::kMillisecondsPerMinute);
            if (!emit) {
                if (state < 0)
                    ++sample.suppressed;
            } else {
                suppressed = sample.suppressed;
                since = sample.initialized ? now - sample.loggedAt : 0;
                sample.initialized = true;
                sample.rootEpoch = epoch;
                sample.state = state;
                sample.error = error;
                sample.reason = reason;
                sample.loggedAt = now;
                sample.suppressed = 0;
            }
        }
        if (emit)
            Audit(state < 0 ? LogLevel::Warn : LogLevel::Info, L"monitor", [&] {
                return std::wstring(L"probe kind=") + (game ? L"game" : L"lobby") + L" state=" +
                       std::to_wstring(state) + L" rootEpoch=" + std::to_wstring(epoch) + L" policy=" +
                       (writeSafety ? L"all_sessions_write_guard" : L"own_session") + L" Win32=" +
                       std::to_wstring(error) + L" reason=" + Limited(reason) + L" changed=" +
                       std::to_wstring(changed) + L" suppressed=" + std::to_wstring(suppressed) +
                       L" intervalMs=" + std::to_wstring(since);
            });
    } catch (...) {
        OutputDebugStringW(L"[service-audit] probe diagnostic unavailable\n");
    }
    SetLastError(saved);
}
Service& Service::Instance() {
    static Service service;
    return service;
}
Service::~Service() {
    RequestStop();
    if (m_monitor.joinable())
        m_monitor.join();
    if (m_worker.joinable())
        m_worker.join();
}
// Status describes scheduling and actual I/O, not a persistent approval/freeze state.
Service::Status Service::GetStatus() {
    Status status;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        status = m_status;
    }
    const int lobby = m_lobby.load();
    const bool enabled = m_running.load() && status.autoOn && m_schedulingEnabled.load() &&
                         !status.gameRoot.empty() && !status.cacheRoot.empty();
    status.periodicReady = enabled && lobby == 1 && m_launchHandled.load() && !m_autoRestoreQueued.load() &&
                           !m_restoreInProgress.load();
    status.matchBackupReady = false;
    auto state = [&](const wchar_t* code, const wchar_t* reason) {
        status.autoState = code;
        status.autoReason = reason;
    };
    if (!m_running.load())
        state(L"stopped", L"服务已停止");
    else if (!status.autoOn)
        state(L"disabled", L"自动同步已关闭，可直接手动复制");
    else if (!m_schedulingEnabled.load())
        state(L"scheduling_disabled", L"当前实例未启用自动调度");
    else if (status.gameRoot.empty())
        state(L"missing_game", L"请先选择 Game 目录");
    else if (status.cacheRoot.empty())
        state(L"missing_cache", L"请先选择缓存文件夹");
    else if (lobby < 0)
        state(L"unknown_lobby", L"大厅状态未知，自动任务暂停");
    else if (lobby == 0)
        state(L"offline", L"大厅已离线，同步已停止，缓存保持当前状态");
    else if (m_restoreInProgress.load())
        state(L"restore_in_progress", L"正在直接覆盖复制缓存到 Game，无哈希复核、无保护冻结");
    else if (m_autoRestoreQueued.load())
        state(L"restore_pending", L"大厅已启动，正在排队直接复制缓存到 Game");
    else if (m_busy.load())
        state(L"busy", L"正在处理直接文件复制任务");
    else if (!m_launchHandled.load())
        state(L"restore_pending", L"等待大厅启动复制任务");
    else
        state(L"active", L"大厅在线：按自定义间隔直接全量镜像 Game 到缓存");
    status.lobbyCycle = status.autoReason;
    if (!status.periodicReady || m_busy.load())
        status.nextSync = status.autoReason;
    else {
        constexpr ULONGLONG MillisecondsPerSecond = 1000;
        constexpr ULONGLONG SecondsPerMinute = 60;
        const auto period = m_options.syncPeriodMs > 0
                                ? static_cast<ULONGLONG>(m_options.syncPeriodMs)
                                : static_cast<ULONGLONG>(status.syncMinutes) * common::kMillisecondsPerMinute;
        const auto now = Tick(), previous = m_lastSyncMs.load();
        const auto elapsed = now >= previous ? now - previous : 0;
        if (elapsed >= period)
            status.nextSync = L"已到期，等待任务空闲";
        else {
            const auto seconds = (period - elapsed + MillisecondsPerSecond - 1) / MillisecondsPerSecond;
            status.nextSync = std::to_wstring(seconds / SecondsPerMinute) + L"分" +
                              std::to_wstring(seconds % SecondsPerMinute) + L"秒";
        }
    }
    return status;
}
void Service::Start(HWND hwnd, const std::wstring& install, const std::wstring& cache) {
    StartOptions options;
    StartWithOptions(hwnd, install, cache, options);
}
// Restrict operations to the two explicitly configured, nonoverlapping roots.
bool Service::ValidateRoots(const std::wstring& game, const std::wstring& cache, std::wstring& error) {
    if ((!cache.empty() && !pathpolicy::ValidateRoot(cache, error)) ||
        (!game.empty() && !pathpolicy::ValidateRoot(game, error)) ||
        (!game.empty() && !cache.empty() && !pathpolicy::AreDisjoint(game, cache, error))) {
        error = L"不是有效路径：" + error;
        return false;
    }
    return true;
}
bool Service::StartWithOptions(HWND hwnd, const std::wstring& install, const std::wstring& cache,
                               const StartOptions& options) {
    Stop();
    std::lock_guard<std::mutex> stopLock(m_stopMutex);
    m_hwnd = hwnd;
    m_options = options;
    m_schedulingEnabled = options.enableScheduling;
    m_installDir = install;
    m_nextOperation = 0;
    m_activeOperation = 0;
    m_stopLogged = false;
    m_stopStartedAt = 0;
    m_eventDrops = 0;
    m_eventDropNoticeAt = 0;
    m_eventWakeNoticeAt = 0;
    m_running = false;
    m_cancelRequested = false;
    m_cancelEpoch = 0;
    m_busy = false;
    m_jobQueued = false;
    m_lobby = -1;
    m_restoreInProgress = false;
    m_autoRestoreQueued = false;
    m_launchHandled = false;
    m_sessionEpoch = 0;
    m_rootEpoch = 0;
    m_onlineSession = 0;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status = Status{};
    }
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        m_events.clear();
    }
    const ULONGLONG startingAt = GetTickCount64();
    const wchar_t* startupStage = L"install-validation";
    auto startup = [&](const wchar_t* stage) noexcept {
        startupStage = stage;
        Audit(LogLevel::Info, L"service", [&] { return std::wstring(L"startup.stage=") + stage; });
    };
    try {
        std::wstring error;
        if (!pathpolicy::ValidateRoot(install, error) || !fileops::EnsureDir(install) ||
            !fileops::ProbeWritable(install, error))
            throw std::runtime_error(WideToUtf8(error.empty() ? L"无法创建可写的用户设置目录" : error));
        startupStage = L"log-open";
        const auto logs = install + L"\\logs";
        if (!pathpolicy::ValidateRoot(logs, error) || !fileops::EnsureDir(logs))
            throw std::runtime_error(WideToUtf8(L"Cannot prepare application logs: " + error + L" Win32=" +
                                                std::to_wstring(GetLastError())));
        if (!Log().Open(logs + L"\\app.jsonl"))
            throw std::runtime_error(WideToUtf8(L"Cannot open application log: " + Log().LastError()));
        serviceLogReady = true;
        try {
            std::lock_guard<std::mutex> lock(m_probeAuditMutex);
            for (auto& sample : m_probeAudit)
                sample = ProbeAudit{};
        } catch (...) {
            OutputDebugStringW(L"[service-audit] probe reset unavailable\n");
        }
        Audit(LogLevel::Info, L"service", [&] {
            return L"startup.begin install=" + Limited(install) + L" monitoring=" +
                   std::to_wstring(options.enableMonitoring) + L" scheduling=" +
                   std::to_wstring(options.enableScheduling) + L" autodetect=" +
                   std::to_wstring(options.enableAutodetect) + L" validateGameRoot=" +
                   std::to_wstring(options.validateGameRoot);
        });
        startup(L"config-load");
        Config cfg;
        if (!Config::LoadSafe(install + L"\\config.json", cfg))
            throw std::runtime_error(
                WideToUtf8(L"配置文件无效，已保留原文件，请修复或重命名后重试。 " + Config::LastError()));
        if (cfg.cacheRoot.empty())
            cfg.cacheRoot = cache;
        if (cfg.gameRoot.empty() && options.enableAutodetect) {
            startup(L"game-autodetect");
            cfg.gameRoot = GameDetect::Find(L"", false);
            Audit(LogLevel::Info, L"service", [&] {
                return L"game-autodetect result=" +
                       Limited(cfg.gameRoot.empty() ? L"not-found" : cfg.gameRoot);
            });
        }
        startup(L"root-validation");
        if (!ValidateRoots(cfg.gameRoot, cfg.cacheRoot, error))
            throw std::runtime_error(WideToUtf8(error));
        if (!cfg.cacheRoot.empty() && !fileops::EnsureDir(cfg.cacheRoot))
            throw std::runtime_error(WideToUtf8(L"无法创建缓存目录；位置=" + cfg.cacheRoot + L"；Win32=" +
                                                std::to_wstring(GetLastError())));
        startup(L"publish-and-threads");
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.autoOn = cfg.autoOn;
            m_status.theme = cfg.theme;
            m_status.closeAction = cfg.closeAction;
            m_status.autostart = cfg.autostart;
            m_status.syncMinutes = cfg.syncMinutes;
            m_status.lastSync = cfg.lastSync;
            m_status.gameRoot = cfg.gameRoot;
            m_status.cacheRoot = cfg.cacheRoot;
            m_status.installed = !cfg.gameRoot.empty();
            m_status.gameOk =
                !cfg.gameRoot.empty() && (!options.validateGameRoot || GameDetect::IsGameRoot(cfg.gameRoot));
            m_status.monitorState = options.enableMonitoring ? L"unknown" : L"stopped";
        }
        RefreshCount();
        m_lastSyncMs = Tick();
        m_lastProgressMs = 0;
        m_running = true;
        m_worker = std::thread(&Service::ThreadMain, this);
        if (options.enableMonitoring || options.monitorProbe)
            m_monitor = std::thread(&Service::MonitorMain, this);
        Audit(LogLevel::Info, L"service", [&] {
            return L"startup.ready 原生服务已启动；取消和大厅检测独立于文件任务 durationMs=" +
                   std::to_wstring(GetTickCount64() - startingAt);
        });
        PushStatusEvent();
        return true;
    } catch (const std::exception& e) {
        Audit(LogLevel::Error, L"service", [&] {
            return std::wstring(L"startup.failed stage=") + startupStage + L" durationMs=" +
                   std::to_wstring(GetTickCount64() - startingAt) + L" reason=" +
                   Limited(Utf8ToWide(e.what()));
        });
        RequestStop();
        if (m_monitor.joinable())
            m_monitor.join();
        if (m_worker.joinable())
            m_worker.join();
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.lastError = Utf8ToWide(e.what());
            m_status.lastResult = L"启动失败，原数据未删除";
        }
        Notify(GetStatus().lastError, true);
        PushStatusEvent();
        return false;
    } catch (...) {
        Audit(LogLevel::Error, L"service",
              [&] { return std::wstring(L"startup.failed unknown-exception stage=") + startupStage; });
        RequestStop();
        if (m_monitor.joinable())
            m_monitor.join();
        if (m_worker.joinable())
            m_worker.join();
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.lastError = L"启动遇到未知错误，原数据未删除";
        }
        PushStatusEvent();
        return false;
    }
}
void Service::RequestStop() noexcept {
    m_cancelRequested = true;
    m_running = false;
    m_wake.notify_all();
    m_monitorWake.notify_all();
    if (!m_stopLogged.exchange(true)) {
        m_stopStartedAt = GetTickCount64();
        Audit(LogLevel::Info, L"service", [&] {
            return L"stop.request busy=" + std::to_wstring(m_busy.load()) + L" operationId=" +
                   std::to_wstring(m_activeOperation.load()) + L" cancelEpoch=" +
                   std::to_wstring(m_cancelEpoch.load());
        });
    }
}
void Service::Stop() {
    std::lock_guard<std::mutex> stopLock(m_stopMutex);
    const bool hadWork =
        m_running.load() || m_worker.joinable() || m_monitor.joinable() || serviceLogReady.load();
    RequestStop();
    const DWORD worker = m_workerThreadId.load();
    if (worker) {
        HANDLE thread = OpenThread(THREAD_TERMINATE, FALSE, worker);
        if (thread) {
            const BOOL cancelled = CancelSynchronousIo(thread);
            const DWORD code = cancelled ? ERROR_SUCCESS : GetLastError();
            CloseHandle(thread);
            Audit(cancelled || code == ERROR_NOT_FOUND ? LogLevel::Info : LogLevel::Warn, L"service", [&] {
                return L"stop.cancel-synchronous-io workerTid=" + std::to_wstring(worker) + L" Win32=" +
                       std::to_wstring(code);
            });
        } else {
            const DWORD code = GetLastError();
            Audit(LogLevel::Warn, L"service",
                  [&] { return L"stop.open-worker-thread failed Win32=" + std::to_wstring(code); });
        }
    }
    if (m_monitor.joinable())
        m_monitor.join();
    if (m_worker.joinable())
        m_worker.join();
    size_t discarded = 0;
    bool discardedRestore = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        discarded = m_commands.size();
        discardedRestore = m_autoRestoreQueued.load();
        std::queue<Command> empty;
        std::swap(m_commands, empty);
        m_autoRestoreQueued = false;
    }
    if (hadWork)
        Audit(LogLevel::Info, L"queue", [&] {
            return L"stop.discard queued=" + std::to_wstring(discarded) + L" launchRestore=" +
                   std::to_wstring(discardedRestore);
        });
    m_busy = false;
    m_jobQueued = false;
    m_restoreInProgress = false;
    m_workerThreadId = 0;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.backupState = L"ready";
        m_status.progressPercent = -1;
        m_status.monitorState = L"stopped";
    }
    if (hadWork)
        Audit(LogLevel::Info, L"service", [&] {
            const auto began = m_stopStartedAt.load();
            return L"stop.complete leases-released durationMs=" +
                   std::to_wstring(began ? GetTickCount64() - began : 0) + L" eventDrops=" +
                   std::to_wstring(m_eventDrops.load());
        });
    serviceLogReady = false;
    try {
        if (!Log().TryClose())
            Audit(LogLevel::Error, L"service",
                  [&] { return L"stop.log-close-failed " + Limited(Log().LastError()); });
    } catch (...) {
        OutputDebugStringW(L"[service-audit] log close threw\n");
    }
}
void Service::PushEvent(const std::wstring& value) {
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        if (m_events.size() >= 2048) {
            m_events.erase(m_events.begin());
            dropped = true;
        }
        m_events.push_back({value});
    }
    if (dropped) {
        const auto total = m_eventDrops.fetch_add(1) + 1;
        const auto now = GetTickCount64(), previous = m_eventDropNoticeAt.load();
        if (!previous || now - previous >= common::kMillisecondsPerMinute) {
            m_eventDropNoticeAt = now;
            Audit(LogLevel::Warn, L"service", [&] {
                return L"event.queue-overflow droppedTotal=" + std::to_wstring(total) + L" capacity=2048";
            });
        }
    }
    if (m_hwnd && !PostMessageW(m_hwnd, WM_APP + 3, 0, 0)) {
        const DWORD code = GetLastError();
        const auto now = GetTickCount64(), previous = m_eventWakeNoticeAt.load();
        if (!previous || now - previous >= common::kMillisecondsPerMinute) {
            m_eventWakeNoticeAt = now;
            Audit(LogLevel::Warn, L"service",
                  [&] { return L"event.wake-failed Win32=" + std::to_wstring(code); });
        }
    }
}
std::vector<Service::Event> Service::DrainEvents() {
    std::vector<Event> events;
    std::lock_guard<std::mutex> lock(m_eventMutex);
    events.swap(m_events);
    return events;
}
void Service::PushStatusEvent() {
    PushEvent(Pack({{"event", "status"}, {"data", StatusJson(GetStatus())}}));
}
void Service::Respond(int id, bool ok, const std::wstring& data) {
    json value = json::parse(WideToUtf8(data));
    json response = {{"id", id}, {"ok", ok}};
    response[ok ? "data" : "error"] = value;
    PushEvent(Pack(response));
    if (ok && (!auditMethod || !QuietMethod(*auditMethod)))
        Audit(LogLevel::Info, L"command", [&] {
            return L"response.ok requestId=" + std::to_wstring(id) + L" method=" +
                   (auditMethod ? Limited(*auditMethod, 64) : L"unspecified");
        });
}
void Service::Failure(int id, const std::string& code, const std::wstring& message) {
    // Only the root-store physical-identity diagnostic enables the explicit UI confirmation flow.
    // Other invalid owners, authorization failures and unsafe paths retain their original error code.
    const char* responseCode = code == "E_DIR" && message.rfind(L"[E_ROOT_IDENTITY_CHANGED]", 0) == 0
                                   ? "E_ROOT_IDENTITY_CHANGED"
                                   : code.c_str();
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.lastError = message;
    }
    if (auditFailed)
        *auditFailed = true;
    Audit(LogLevel::Warn, L"command", [&] {
        return L"command.reject-or-fail requestId=" + std::to_wstring(id) + L" method=" +
               (auditMethod ? Limited(*auditMethod, 64) : L"unspecified") + L" code=" +
               Utf8ToWide(responseCode) +
               (auditMethod && QuietMethod(*auditMethod) ? L"" : L" reason=" + Limited(message));
    });
    Respond(id, false, Pack({{"code", responseCode}, {"message", WideToUtf8(message)}}));
    if (id == 0)
        Notify(message, true);
}
void Service::Notify(const std::wstring& message, bool error) {
    PushEvent(Pack({{"event", "toast"},
                    {"data",
                     {{"kind", error ? "error" : "info"},
                      {"title", error ? "操作未完成" : "LOLBackup"},
                      {"body", WideToUtf8(message)}}}}));
}
void Service::RefreshCount() {
    const auto status = GetStatus();
    if (status.cacheRoot.empty()) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.cacheEntries = 0;
        return;
    }
    auto list = fileops::ListBusinessFilesChecked(status.cacheRoot, [&] { return m_cancelRequested.load(); });
    // Keep the last complete count on cancellation or a managed-tree error; partial results must
    // never be presented as a complete cache. Unrelated mixed-root content is not enumerated.
    if (!list.complete) {
        Audit(LogLevel::Warn, L"inventory", [&] {
            return L"cache-count.incomplete previous-count-retained cancelled=" +
                   std::to_wstring(list.cancelled);
        });
        if (!list.errors.empty())
            AuditFileErrors(L"cache-count", list.errors);
        return;
    }
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_status.cacheEntries = static_cast<int>(std::min<size_t>(
        std::count_if(list.files.begin(), list.files.end(),
                      [](const std::wstring& relative) {
                          std::wstring name = relative;
                          std::transform(name.begin(), name.end(), name.begin(),
                                         [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                          if (name.rfind(L"saves\\", 0) == 0 || name.rfind(L"shards\\", 0) == 0)
                              return true;
                          for (const auto* file : kBotFiles)
                              if (_wcsicmp(name.c_str(), file) == 0)
                                  return true;
                          return false;
                      }),
        INT_MAX));
}
void Service::PostCommand(int id, const std::wstring& method, const std::wstring& params) {
    Command incoming;
    incoming.id = id;
    Stamp(incoming);
    ScopedLogContext context(Trace(incoming));
    MethodScope methodScope(method);
    bool reserved = false;
    try {
        if (method == L"cancel_op") {
            unsigned long long previousEpoch = 0;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                previousEpoch = m_cancelEpoch.fetch_add(1);
                m_autoRestoreQueued = false;
                m_launchHandled = m_lobby.load() != 0 && m_onlineSession.load() != 0;
                m_lastSyncMs = Tick();
            }
            m_cancelRequested = true;
            Audit(LogLevel::Info, L"command", [&] {
                return L"cancel.request epochBefore=" + std::to_wstring(previousEpoch) + L" epochAfter=" +
                       std::to_wstring(previousEpoch + 1) + L" busy=" + std::to_wstring(m_busy.load()) +
                       L" operationId=" + std::to_wstring(m_activeOperation.load());
            });
            Respond(id, true, Pack({{"requested", true}, {"busy", m_busy.load()}}));
            Audit(LogLevel::Info, L"command", [&] {
                return L"cancel.complete durationMs=" +
                       std::to_wstring(GetTickCount64() - incoming.enqueuedAt);
            });
            return;
        }
        if (!m_running) {
            Failure(id, "E_STOPPED", L"服务尚未启动或正在退出");
            return;
        }
        if (!method.empty() && method.front() == L'@') {
            Failure(id, "E_UNKNOWN", L"内部任务不可直接提交");
            return;
        }
        if (method.size() > 64 || params.size() > 65536) {
            Failure(id, "E_INPUT", L"请求超出长度限制");
            return;
        }
        const bool job = method == L"backup_now" || method == L"restore_now" || method == L"import_cache" ||
                         method == L"clear_game_config";
        if (job && (m_busy.load() || m_jobQueued.exchange(true))) {
            Failure(id, "E_BUSY", L"已有数据任务正在执行或等待，请等待完成或取消");
            return;
        }
        reserved = job;
        bool accepted = false;
        size_t queueDepth = 0;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_commands.size() < 128) {
                incoming.method = method;
                incoming.params = params;
                incoming.cancelEpoch = m_cancelEpoch.load();
                incoming.sessionEpoch = m_sessionEpoch.load();
                incoming.rootEpoch = m_rootEpoch.load();
                m_commands.push(std::move(incoming));
                queueDepth = m_commands.size();
                accepted = true;
            }
        }
        if (!accepted) {
            if (job)
                m_jobQueued = false;
            reserved = false;
            Failure(id, "E_BUSY", L"待处理请求过多，请稍后重试");
            return;
        }
        reserved = false;
        if (!QuietMethod(method))
            Audit(LogLevel::Info, L"queue", [&] {
                return L"command.accept queued method=" + Limited(method, 64) + L" commandId=" +
                       std::to_wstring(incoming.traceId) + L" queueDepth=" + std::to_wstring(queueDepth) +
                       L" cancelEpoch=" + std::to_wstring(incoming.cancelEpoch) + L" sessionEpoch=" +
                       std::to_wstring(incoming.sessionEpoch);
            });
        m_wake.notify_one();
    } catch (...) {
        if (reserved)
            m_jobQueued = false;
        Failure(id, "E_INTERNAL", L"无法提交请求，请稍后重试");
    }
}
void Service::BackupNow() {
    PostCommand(0, L"backup_now", L"{}");
}
void Service::RestoreNow() {
    PostCommand(0, L"restore_now", L"{}");
}
int Service::ProbeLobby(const std::wstring& game) {
    const auto probeEpoch = m_rootEpoch.load();
    if (m_options.monitorProbe) {
        int n = m_options.monitorProbe(game);
        const int state = n == 0 || n == 1 ? n : -1;
        AuditProbe(false, state, ERROR_SUCCESS, state < 0 ? L"custom-probe-unknown" : L"", probeEpoch);
        return state;
    }
    if (game.empty())
        return 0;
    const auto result = Monitor::Probe(game);
    const int state = result.state == LobbyState::Online ? 1 : result.state == LobbyState::Offline ? 0 : -1;
    AuditProbe(false, state, result.error, result.message.c_str(), probeEpoch);
    return state;
}
ULONGLONG Service::Tick() const {
    return m_options.monotonicClock ? m_options.monotonicClock() : GetTickCount64();
}
void Service::Phase(const std::wstring& phase) {
    // Explicit low-frequency allowlist: never log scheduler-check, per-file or progress labels.
    for (const auto* allowed : {L"before-lobby-edge",      L"online-edge",
                                L"offline-edge",           L"match-started",
                                L"match-backup-queued",    L"match-backup-skipped",
                                L"cleanup-queued",         L"automatic-restore-dequeued",
                                L"restore-returned",       L"backup-returned",
                                L"match-backup-returned",  L"match-backup-begin",
                                L"restore-begin",          L"backup-begin",
                                L"restore-finished",       L"backup-finished",
                                L"cleanup-finished",       L"cleanup-begin",
                                L"settings-before-commit", L"configuration-committed"}) {
        if (phase == allowed) {
            Audit(LogLevel::Info, L"phase", [&] {
                return L"phase=" + phase + L" operationId=" + std::to_wstring(m_activeOperation.load());
            });
            break;
        }
    }
    if (m_options.onPhase)
        m_options.onPhase(phase);
}
// Only lobby transitions schedule automatic work. Match samples defer Game writes;
// they never authorize extra backups or offline cleanup.
// Lobby polling is independent of file I/O. Direct mode has no match/approval delay.
void Service::MonitorMain() noexcept {
    constexpr int MinimumPollMs = 50;
    constexpr int MaximumPollMs = 5000;
    int previous = -1;
    auto observedRoot = m_rootEpoch.load();
    std::wstring lastState;
    while (m_running.load()) {
        try {
            const auto epoch = m_rootEpoch.load();
            if (epoch != observedRoot) {
                observedRoot = epoch;
                previous = m_lobby.load() == 1 && m_onlineSession.load() != 0 ? 1 : -1;
            }
            const auto before = GetStatus();
            const int online = ProbeLobby(before.gameRoot);
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (epoch != m_rootEpoch.load())
                    continue;
                m_lobby = online;
                std::lock_guard<std::mutex> statusLock(m_statusMutex);
                m_status.monitorKnown = online >= 0;
                if (online >= 0)
                    m_status.lobbyOnline = online == 1;
                m_status.monitorState = online < 0 ? L"unknown" : L"running";
            }
            if (online == 1 && m_autoRestoreQueued.load())
                m_wake.notify_one();
            if (online >= 0 && online != previous) {
                Phase(L"before-lobby-edge");
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    if (epoch != m_rootEpoch.load())
                        continue;
                    previous = online;
                    m_launchHandled = false;
                    m_autoRestoreQueued = false;
                    if (online == 1) {
                        ++m_sessionEpoch;
                        m_onlineSession = m_sessionEpoch.load();
                        bool enabled = false;
                        {
                            std::lock_guard<std::mutex> statusLock(m_statusMutex);
                            enabled = m_status.autoOn && !m_status.gameRoot.empty() && !m_status.cacheRoot.empty();
                        }
                        if (enabled && m_schedulingEnabled.load()) {
                            m_pendingRestore = {0, L"@restore", L"{}", m_cancelEpoch.load(),
                                                m_sessionEpoch.load()};
                            m_pendingRestore.rootEpoch = epoch;
                            Stamp(m_pendingRestore);
                            m_autoRestoreQueued = true;
                            queued = true;
                        }
                    } else
                        m_onlineSession = 0;
                }
                Audit(LogLevel::Info, L"monitor", [&] {
                    return L"lobby.confirmed-edge state=" + std::to_wstring(online) + L" sessionEpoch=" +
                           std::to_wstring(m_sessionEpoch.load()) + L" restoreQueued=" +
                           std::to_wstring(queued) + L" direct=1";
                });
                Phase(online == 1 ? L"online-edge" : L"offline-edge");
                m_wake.notify_one();
                PushStatusEvent();
            }
            const auto actual = GetStatus();
            if (actual.autoState != lastState) {
                lastState = actual.autoState;
                PushStatusEvent();
            }
        } catch (...) {
            m_lobby = -1;
            std::lock_guard<std::mutex> statusLock(m_statusMutex);
            m_status.monitorKnown = false;
            m_status.monitorState = L"unknown";
        }
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_monitorWake.wait_for(
            lock,
            std::chrono::milliseconds(std::clamp(m_options.monitorIntervalMs, MinimumPollMs, MaximumPollMs)),
            [&] { return !m_running.load() || observedRoot != m_rootEpoch.load(); });
    }
}
void Service::ThreadMain() noexcept {
    m_workerThreadId = GetCurrentThreadId();
    Audit(LogLevel::Info, L"worker",
          [] { return std::wstring(L"worker.thread-start priority=fifo>launch-restore>periodic"); });
    while (m_running) {
        Command command;
        bool have = false;
        size_t queueDepth = 0;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            auto restoreReady = [&] { return m_autoRestoreQueued.load() && m_lobby.load() == 1; };
            if (m_commands.empty() && !restoreReady())
                m_wake.wait_for(lock, std::chrono::milliseconds(250),
                                [&] { return !m_running.load() || !m_commands.empty() || restoreReady(); });
            if (!m_running)
                break;
            if (!m_commands.empty()) {
                command = std::move(m_commands.front());
                m_commands.pop();
                have = true;
            } else if (restoreReady()) {
                if (!m_jobQueued.exchange(true)) {
                    m_autoRestoreQueued = false;
                    command = m_pendingRestore; // Never launder cancelled/stale work with fresh tokens.
                    have = true;
                }
            }
            queueDepth = m_commands.size();
        }
        if (have)
            Stamp(command);
        ScopedLogContext context(have ? Trace(command) : std::wstring{});
        MethodScope methodScope(command.method);
        try {
            if (have) {
                CommandAudit diagnostic(command.method, command.traceId, command.enqueuedAt);
                if (!QuietMethod(command.method))
                    Audit(LogLevel::Info, L"queue", [&] {
                        return L"command.dequeued queueDepth=" + std::to_wstring(queueDepth) +
                               L" capturedCancel=" + std::to_wstring(command.cancelEpoch) +
                               L" currentCancel=" + std::to_wstring(m_cancelEpoch.load()) +
                               L" capturedSession=" + std::to_wstring(command.sessionEpoch) +
                               L" currentSession=" + std::to_wstring(m_sessionEpoch.load()) +
                               L" capturedRoot=" + std::to_wstring(command.rootEpoch) + L" currentRoot=" +
                               std::to_wstring(m_rootEpoch.load());
                    });
                if (command.method == L"@restore")
                    Phase(L"automatic-restore-dequeued");
                ProcessCommand(command);
            } else if (m_options.enableScheduling) {
                Phase(L"scheduler-check");
                const auto status = GetStatus();
                const auto period =
                    m_options.syncPeriodMs > 0
                        ? static_cast<ULONGLONG>(m_options.syncPeriodMs)
                        : static_cast<ULONGLONG>(status.syncMinutes) * common::kMillisecondsPerMinute;
                // Only an online, restored session authorizes a complete periodic mirror.
                if (status.periodicReady && AutoBackupDue(status.autoOn, true, m_lobby.load(),
                                                          Tick() - m_lastSyncMs.load(), period)) {
                    Phase(L"periodic-due-ready");
                    Phase(L"periodic-before-dispatch");
                    bool reserved = false;
                    {
                        std::lock_guard<std::mutex> lock(m_queueMutex);
                        command = {0, L"@backup", L"{}", m_cancelEpoch.load(), m_sessionEpoch.load()};
                        command.rootEpoch = m_rootEpoch.load();
                        // Couple authority and tokens: an old due sample cannot borrow
                        // a new lobby session whose launch restore is still pending.
                        reserved = PeriodicSessionReady(command) && !m_jobQueued.exchange(true);
                        if (reserved)
                            m_lastSyncMs = Tick();
                    }
                    if (reserved) {
                        Stamp(command);
                        ScopedLogContext periodicContext(Trace(command));
                        CommandAudit diagnostic(command.method, command.traceId, command.enqueuedAt);
                        Audit(LogLevel::Info, L"schedule", [&] {
                            return L"periodic.due periodMs=" + std::to_wstring(period) + L" sessionEpoch=" +
                                   std::to_wstring(command.sessionEpoch) + L" cancelEpoch=" +
                                   std::to_wstring(command.cancelEpoch);
                        });
                        RunOperation(command, false, true);
                    }
                }
            }
        } catch (const std::exception& e) {
            ScopedLogContext failureContext(Trace(command));
            if (command.method == L"backup_now" || command.method == L"restore_now" ||
                command.method == L"import_cache" || command.method == L"@backup" ||
                command.method == L"@restore") {
                m_jobQueued = false;
                m_busy = false;
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_status.backupState = L"ready";
                m_status.progressPercent = -1;
            }
            Failure(command.id, "E_INTERNAL", L"任务发生异常，缓存保持最后状态。 " + Utf8ToWide(e.what()));
            PushStatusEvent();
        } catch (...) {
            ScopedLogContext failureContext(Trace(command));
            if (command.method == L"backup_now" || command.method == L"restore_now" ||
                command.method == L"import_cache" || command.method == L"@backup" ||
                command.method == L"@restore") {
                m_jobQueued = false;
                m_busy = false;
            }
            Failure(command.id, "E_INTERNAL", L"任务遇到未知异常，操作已停止");
            PushStatusEvent();
        }
    }
    m_workerThreadId = 0;
    Audit(LogLevel::Info, L"worker", [] { return std::wstring(L"worker.thread-exit"); });
}
// Stale readiness must never authorize unrestored Game as a mirror source.
// Task ordering and event tokens implement the requested lobby lifecycle, not a disk approval guard.
bool Service::PeriodicSessionReady(const Command& command) const noexcept {
    return m_running.load() && m_schedulingEnabled.load() && m_lobby.load() == 1 && m_launchHandled.load() &&
           !m_autoRestoreQueued.load() && !m_restoreInProgress.load() &&
           command.cancelEpoch == m_cancelEpoch.load() && command.rootEpoch == m_rootEpoch.load() &&
           command.sessionEpoch == m_sessionEpoch.load() && m_onlineSession.load() == command.sessionEpoch;
}
// Direct-copy mode deliberately has no persistent freeze, process veto, or content hash pass.
void Service::RunOperation(const Command& command, bool restore, bool automatic) {
    const auto started = GetTickCount64();
    Finally finish{[&] {
        m_restoreInProgress = false;
        m_activeOperation = 0;
        m_busy = false;
        m_jobQueued = false;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.backupState = L"ready";
            m_status.progressPercent = -1;
            m_status.progressLabel.clear();
        }
        Phase(restore ? L"restore-returned" : L"backup-returned");
        PushStatusEvent();
    }};
    m_cancelRequested = false;
    m_busy = true;
    if (!m_running.load() || command.cancelEpoch != m_cancelEpoch.load() ||
        command.sessionEpoch != m_sessionEpoch.load() || command.rootEpoch != m_rootEpoch.load()) {
        Failure(command.id, "E_CANCELLED", L"任务已取消或所属大厅/目录已改变，未开始复制");
        return;
    }
    const auto status = GetStatus();
    if (status.gameRoot.empty()) {
        Failure(command.id, "E_GAME", L"预期已选择 Game 目录，实际为空；位置=直接复制入口");
        return;
    }
    if (status.cacheRoot.empty()) {
        Failure(command.id, "E_PATH", L"请先在设置中选择缓存文件夹，未开始复制");
        return;
    }
    if (automatic &&
        (!status.autoOn || m_lobby.load() != 1 || m_onlineSession.load() != command.sessionEpoch ||
         (!restore && !PeriodicSessionReady(command)))) {
        if (restore && status.autoOn) {
            // Preserve at the actual skip decision, not an earlier racy sample.
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_running.load() && command.cancelEpoch == m_cancelEpoch.load() &&
                command.sessionEpoch == m_sessionEpoch.load() && command.rootEpoch == m_rootEpoch.load() &&
                m_onlineSession.load() == command.sessionEpoch && m_lobby.load() != 0) {
                m_pendingRestore = command;
                m_autoRestoreQueued = true;
            }
        }
        Failure(command.id, "E_SKIPPED", L"大厅已离线、状态未知或本轮启动复制尚未执行");
        return;
    }
    m_restoreInProgress = restore;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.backupState = L"running";
        m_status.progressPercent = 0;
        m_status.progressLabel = restore ? L"直接复制缓存到 Game" : L"直接镜像 Game 到缓存";
        m_status.lastError.clear();
        m_status.operationId = ++m_nextOperation;
        m_activeOperation = m_status.operationId;
    }
    PushStatusEvent();
    Audit(LogLevel::Info, L"sync", [&] {
        return L"operation.begin method=" + command.method + L" direct=1 operationId=" +
               std::to_wstring(m_activeOperation.load());
    });
    SyncEngine engine;
    engine.gameRoot = status.gameRoot;
    engine.cacheRoot = status.cacheRoot;
    engine.installDir = m_installDir;
    bool lobbyStoppedAtDeleteBoundary = false;
    bool deleteBoundaryChecked = false;
    engine.shouldCancel = [&] {
        if (!m_running.load() || m_cancelRequested.load() || command.cancelEpoch != m_cancelEpoch.load() ||
            command.sessionEpoch != m_sessionEpoch.load() || command.rootEpoch != m_rootEpoch.load())
            return true;
        return automatic && (lobbyStoppedAtDeleteBoundary || m_lobby.load() != 1 ||
                             m_onlineSession.load() != command.sessionEpoch);
    };
    engine.onProgress = [&](int done, int total, const std::wstring& current) {
        Phase(current);
        // One fresh lobby check before pruning, including empty-source deletion.
        // Restore and per-file copy callbacks perform no process enumeration.
        if (automatic && !restore && !deleteBoundaryChecked &&
            (current == L"mirror/verified" || current.rfind(L"delete/", 0) == 0)) {
            deleteBoundaryChecked = true;
            lobbyStoppedAtDeleteBoundary = ProbeLobby(status.gameRoot) != 1;
        }
        constexpr ULONGLONG ProgressIntervalMs = 100;
        const auto now = GetTickCount64();
        if (done != total && now - m_lastProgressMs.load() < ProgressIntervalMs)
            return;
        m_lastProgressMs = now;
        const int percent =
            total > 0
                ? static_cast<int>(std::clamp<long long>(static_cast<long long>(done) * 100 / total, 0, 100))
                : 0;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.progressPercent = percent;
            m_status.progressLabel = current;
        }
        PushEvent(Pack(
            {{"event", "progress"},
             {"data",
              {{"done", done}, {"total", total}, {"percent", percent}, {"current", WideToUtf8(current)}}}}));
    };
    Phase(restore ? L"restore-begin" : L"backup-begin");
    const auto result = restore ? engine.RestoreDetailed() : engine.RunOnce(true, false, !automatic, true);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const bool current = m_running.load() && command.cancelEpoch == m_cancelEpoch.load() &&
                             command.sessionEpoch == m_sessionEpoch.load() &&
                             command.rootEpoch == m_rootEpoch.load();
        if (current) {
            m_lastSyncMs = Tick();
            if (restore) {
                // An attempt ends the launch phase even on I/O failure: direct mode never freezes.
                m_autoRestoreQueued = false;
                m_launchHandled = m_lobby.load() != 0 && m_onlineSession.load() == command.sessionEpoch;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.lastResult = result.summary;
    }
    if (result.success && !restore) {
        const auto time = Now();
        if (UpdateConfig(m_installDir + L"\\config.json", [&](Config& c) { c.lastSync = time; })) {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.lastSync = time;
        }
    }
    // Restore does not modify the cache; avoid enumerating it again just to update its count.
    if (!restore)
        RefreshCount();
    Audit(result.success ? LogLevel::Info : LogLevel::Warn, L"sync", [&] {
        return L"operation.result method=" + command.method + L" direct=1 success=" +
               std::to_wstring(result.success) + L" copied=" + std::to_wstring(result.copied) + L" deleted=" +
               std::to_wstring(result.deleted) + L" errors=" + std::to_wstring(result.errors) +
               L" durationMs=" + std::to_wstring(GetTickCount64() - started);
    });
    Phase(restore ? L"restore-finished" : L"backup-finished");
    if (result.success) {
        Respond(command.id, true,
                Pack({{"success", true},
                      {"copied", result.copied},
                      {"deleted", result.deleted},
                      {"errors", result.errors},
                      {"summary", WideToUtf8(result.summary)}}));
        Notify(result.summary);
    } else
        Failure(command.id,
                result.cancelled ? "E_CANCELLED"
                : result.partial ? "E_PARTIAL"
                                 : "E_IO",
                result.summary);
}
void Service::ProcessCommand(const Command& command) {
    const auto& method = command.method;
    const int id = command.id;
    if (method == L"@restore") {
        RunOperation(command, true, true);
        return;
    }
    if (method == L"@backup") {
        RunOperation(command, false, true);
        return;
    }
    json params;
    try {
        params = json::parse(WideToUtf8(command.params.empty() ? L"{}" : command.params));
        if (!params.is_object())
            throw std::runtime_error("params");
    } catch (...) {
        if (method == L"import_cache" || method == L"backup_now" || method == L"restore_now" ||
            method == L"clear_game_config")
            m_jobQueued = false;
        Failure(id, "E_INPUT", L"请求参数必须为有效JSON对象");
        return;
    }
    if (method == L"backup_now" || method == L"restore_now") {
        RunOperation(command, method == L"restore_now", false);
        return;
    }
    if (method == L"echo") {
        Respond(id, true, Pack(params));
        return;
    }
    if (method == L"get_status") {
        Respond(id, true, Pack(StatusJson(GetStatus())));
        PushStatusEvent();
        return;
    }
    if (method == L"get_logs" || method == L"export_logs") {
        std::wstring error;
        int limit = 200;
        if (!IntField(params, "limit", 1, 1000, limit, error)) {
            Failure(id, "E_INPUT", error);
            return;
        }
        std::vector<std::string> records;
        if (!Logger::TryReadRawTail(m_installDir + L"\\logs\\app.jsonl", records, &error,
                                    static_cast<size_t>(limit), 256 * 1024)) {
            Failure(id, "E_LOG", error);
            return;
        }
        // Parse each COMPLETE UTF-8 JSON record before shortening string fields.
        // Reserve envelope/text-format overhead below the public 256 KiB cap.
        constexpr size_t MaxLineBytes = 8192, ResponseBudget = 240 * 1024;
        std::deque<std::pair<json, size_t>> tail;
        size_t retainedBytes = 0;
        for (const auto& line : records) {
            json parsed;
            try {
                parsed = json::parse(
                    line,
                    [](int depth, json::parse_event_t, json&) {
                        if (depth > 32)
                            throw std::runtime_error("log nesting");
                        return true;
                    },
                    false);
            } catch (...) {
                parsed = nullptr;
            }
            auto field = [&](const char* key, const std::string& fallback, size_t cap) -> std::string {
                std::string value = parsed.is_object() && parsed.contains(key) && parsed.at(key).is_string()
                                        ? parsed.at(key).get<std::string>()
                                        : fallback;
                if (value.size() > cap) {
                    size_t cut = cap;
                    while (cut && (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80)
                        --cut;
                    value.resize(cut);
                    value += " … [长日志已截断]";
                }
                return value;
            };
            json entry = {{"ts", field("ts", "", 64)},
                          {"level", field("level", "", 16)},
                          {"module", field("module", "", 256)},
                          {"msg", field("msg", line, MaxLineBytes)}};
            const size_t bytes =
                entry.dump().size() + entry.at("msg").get_ref<const std::string&>().size() + 64;
            while (!tail.empty() &&
                   (tail.size() >= static_cast<size_t>(limit) || retainedBytes + bytes > ResponseBudget)) {
                retainedBytes -= tail.front().second;
                tail.pop_front();
            }
            tail.emplace_back(std::move(entry), bytes);
            retainedBytes += bytes;
        }
        json lines = json::array();
        std::string recent;
        for (auto& item : tail) {
            const std::string level = item.first.value("level", ""), module = item.first.value("module", "");
            const bool error = level == "error" || level == "fatal", warn = level == "warn";
            if (!error && !warn && (module == "ui-assets" || module == "ui-glyphs" || module == "ui-about")) {
                lines.push_back(std::move(item.first));
                continue;
            }
            std::string stamp = item.first.value("ts", "");
            if (stamp.size() >= 19)
                stamp = stamp.substr(11, 8);
            recent += stamp + "  ";
            if (error)
                recent += "错误 · ";
            else if (warn)
                recent += "提醒 · ";
            recent += item.first.at("msg").get_ref<const std::string&>();
            recent += "\r\n";
            lines.push_back(std::move(item.first));
        }
        Respond(id, true, Pack({{"logs", lines}, {"text", recent}}));
        return;
    }
    if (method == L"detect_game") {
        const auto found = GameDetect::Find(L"", false);
        if (found.empty())
            Failure(id, "E_NOT_FOUND", L"未找到外服游戏，请手动选择Game目录");
        else
            Respond(id, true, Pack({{"path", WideToUtf8(found)}}));
        return;
    }
    if (method == L"clear_game_config") {
        const auto cleanupStarted = GetTickCount64();
        const wchar_t* cleanupCancelReason = L"none";
        Finally finish{[&] {
            m_activeOperation = 0;
            m_busy = false;
            m_jobQueued = false;
            {
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_status.backupState = L"ready";
                m_status.progressPercent = -1;
                m_status.progressLabel.clear();
            }
            Phase(L"cleanup-finished");
            PushStatusEvent();
        }};
        m_cancelRequested = false;
        if (command.cancelEpoch != m_cancelEpoch.load() || !m_running.load() ||
            command.sessionEpoch != m_sessionEpoch.load()) {
            Failure(id, "E_CANCELLED", L"清理请求已取消或会话已改变，Game 保留");
            return;
        }
        const auto status = GetStatus();
        if (status.gameRoot.empty()) {
            Failure(id, "E_GAME", L"请选择 Game 目录");
            return;
        }
        auto cancelled = [&] {
            auto stop = [&](const wchar_t* reason) noexcept {
                if (cleanupCancelReason == std::wstring_view(L"none"))
                    cleanupCancelReason = reason;
                return true;
            };
            if (m_cancelRequested.load())
                return stop(m_running.load() ? L"user-request" : L"stop");
            if (!m_running.load())
                return stop(L"stop");
            if (command.cancelEpoch != m_cancelEpoch.load())
                return stop(L"cancel-epoch");
            if (command.sessionEpoch != m_sessionEpoch.load())
                return stop(L"session-changed");
            return false;
        };
        m_busy = true;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.backupState = L"running";
            m_status.progressPercent = 0;
            m_status.progressLabel = L"正在安全清除 Game 配置…";
            m_status.operationId = ++m_nextOperation;
            m_activeOperation = m_status.operationId;
        }
        const auto cleanupOperation = m_activeOperation.load();
        Audit(LogLevel::Info, L"cleanup", [&] {
            return L"cleanup.begin operationId=" + std::to_wstring(cleanupOperation) + L" game=" +
                   Limited(status.gameRoot) + L" automatic=0";
        });
        Phase(L"cleanup-begin");
        PushStatusEvent();
        auto listing = fileops::ListFilesChecked(status.gameRoot, cancelled);
        if (!listing.complete) {
            Audit(LogLevel::Warn, L"cleanup", [&] {
                return L"cleanup.listing-incomplete operationId=" + std::to_wstring(cleanupOperation) +
                       L" cancelled=" + std::to_wstring(listing.cancelled) + L" cancelReason=" +
                       cleanupCancelReason;
            });
            if (!listing.errors.empty())
                AuditFileErrors(L"cleanup-listing", listing.errors);
            Failure(id, listing.cancelled ? "E_CANCELLED" : "E_IO", L"未完整检查 Game，未执行清理");
            return;
        }
        const auto isBusiness = [](const std::wstring& relative) {
            std::wstring name = relative;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (name.rfind(L"saves\\", 0) == 0 || name.rfind(L"shards\\", 0) == 0)
                return true;
            for (const auto* file : kBotFiles)
                if (_wcsicmp(name.c_str(), file) == 0)
                    return true;
            return false;
        };
        std::vector<std::wstring> files, directories;
        for (const auto& relative : listing.files)
            if (isBusiness(relative))
                files.push_back(relative);
        for (const auto& relative : listing.directories) {
            std::wstring name = relative;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (name == L"saves" || name == L"shards" || name.rfind(L"saves\\", 0) == 0 ||
                name.rfind(L"shards\\", 0) == 0)
                directories.push_back(relative);
        }
        const auto result = fileops::ClearTreeEntriesChecked(
            status.gameRoot, files, directories, cancelled,
            [&](const std::wstring& relative) { Phase(L"clear/" + relative); });
        const bool success = result.complete && !result.cancelled && result.errors.empty();
        Audit(success ? LogLevel::Info : LogLevel::Warn, L"cleanup", [&] {
            return L"cleanup.result operationId=" + std::to_wstring(cleanupOperation) + L" success=" +
                   std::to_wstring(success) + L" deleted=" + std::to_wstring(result.deleted) +
                   L" cancelled=" + std::to_wstring(result.cancelled) + L" cancelReason=" +
                   cleanupCancelReason + L" errors=" + std::to_wstring(result.errors.size()) +
                   L" durationMs=" + std::to_wstring(GetTickCount64() - cleanupStarted);
        });
        if (!result.errors.empty())
            AuditFileErrors(L"cleanup-result", result.errors);
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.lastResult = success            ? L"已清除 Game 配置，缓存保留"
                                  : result.cancelled ? L"清理已中止，未继续删除；缓存保留"
                                                     : L"清理未完整完成，缓存保留";
        }
        if (success) {
            Respond(id, true, Pack({{"success", true}, {"deleted", result.deleted}, {"failed", 0}}));
            Notify(L"已清除 Game 中的配置；缓存仍保留。");
        } else {
            Failure(id, result.cancelled ? "E_CANCELLED" : "E_IO", GetStatus().lastResult);
        }
        return;
    }
    if (method == L"import_cache") {
        const auto importStarted = GetTickCount64();
        const wchar_t* importCancelReason = L"none";
        Finally finish{[&] {
            m_activeOperation = 0;
            m_busy = false;
            m_jobQueued = false;
            {
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_status.backupState = L"ready";
                m_status.progressPercent = -1;
            }
            PushStatusEvent();
        }};
        std::wstring source, error;
        if (!params.contains("path") || !PathField(params, "path", source, error)) {
            Failure(id, "E_PATH", error.empty() ? L"请选择原缓存目录" : error);
            return;
        }
        const auto status = GetStatus();
        if (status.cacheRoot.empty()) {
            Failure(id, "E_PATH", L"请先在设置中选择缓存文件夹，再导入已有缓存");
            return;
        }
        if (!pathpolicy::AreDisjoint(source, status.cacheRoot, error) || !fileops::IsDir(source)) {
            Failure(id, "E_PATH",
                    L"导入源无效：预期是存在且与缓存目录互不包含的目录，实际不满足；位置=" + source + L"；" +
                        error);
            return;
        }
        m_busy = true;
        m_cancelRequested = false;
        auto cancelled = [&] {
            auto stop = [&](const wchar_t* reason) noexcept {
                if (importCancelReason == std::wstring_view(L"none"))
                    importCancelReason = reason;
                return true;
            };
            if (m_cancelRequested.load())
                return stop(m_running.load() ? L"user-request" : L"stop");
            if (command.cancelEpoch != m_cancelEpoch.load())
                return stop(L"cancel-epoch");
            if (!m_running.load())
                return stop(L"stop");
            return false;
        };
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.backupState = L"running";
            m_status.progressPercent = 0;
            m_status.operationId = ++m_nextOperation;
            m_activeOperation = m_status.operationId;
        }
        const auto importOperation = m_activeOperation.load();
        Audit(LogLevel::Info, L"import", [&] {
            return L"import.begin operationId=" + std::to_wstring(importOperation) + L" source=" +
                   Limited(source) + L" cache=" + Limited(status.cacheRoot) + L" copyOnly=1";
        });
        PushStatusEvent();
        int copied = 0, failed = 0;
        std::vector<std::wstring> reintroduced;
        auto progress = [&](int done, int total, const std::wstring& current) {
            if (cancelled())
                return false;
            {
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_status.progressPercent =
                    total ? static_cast<int>(static_cast<long long>(done) * 100 / total) : 0;
                m_status.progressLabel = current;
            }
            return true;
        };
        for (const auto* dir : {L"saves", L"shards"}) {
            if (cancelled())
                break;
            if (!fileops::IsDir(source + L"\\" + dir))
                continue;
            // Import is copy-only. Pin each destination through the whole copy without creating
            // an ownership marker in the business tree. The external root lease remains held.
            const auto target = status.cacheRoot + L"\\" + dir;
            fileops::DirectoryLease targetPin;
            if (!fileops::EnsureDir(target) || !targetPin.Acquire(target, error, false)) {
                const DWORD code = GetLastError();
                Audit(LogLevel::Warn, L"import", [&] {
                    return L"import.destination-lease-failed path=" + Limited(target) + L" Win32=" +
                           std::to_wstring(code) + L" reason=" + Limited(error);
                });
                ++failed;
                continue;
            }
            auto r = fileops::CopyTreeChecked(source + L"\\" + dir, status.cacheRoot + L"\\" + dir, progress);
            copied += r.copied;
            for (const auto& relative : r.copiedPaths)
                reintroduced.push_back(std::wstring(dir) + L"\\" + relative);
            failed += static_cast<int>(r.errors.size());
            if (!r.errors.empty())
                AuditFileErrors(L"import-copy-tree", r.errors);
            if (r.cancelled)
                m_cancelRequested = true;
        }
        for (int i = 0; i < kBotFileCount && !cancelled(); ++i) {
            const auto src = source + L"\\" + kBotFiles[i];
            if (!fileops::Exists(src))
                continue;
            fileops::FileError e;
            if (fileops::CopyFileChecked(src, status.cacheRoot + L"\\" + kBotFiles[i], e, cancelled)) {
                ++copied;
                reintroduced.push_back(kBotFiles[i]);
            } else {
                Audit(LogLevel::Warn, L"import", [&] {
                    return L"import.copy-failed " + Limited(e.operation + L": " + e.path + L" Win32=" +
                                                            std::to_wstring(e.code) + L" " + e.message);
                });
                ++failed;
            }
        }
        if (!failed && !cancelled() && copied) {
            SyncEngine imported;
            imported.gameRoot = status.gameRoot;
            imported.cacheRoot = status.cacheRoot;
            imported.installDir = m_installDir;
            imported.managementRoot = m_installDir + L"\\management-v1";
            imported.shouldCancel = cancelled;
            if (!imported.ReconcileCacheAbsences(reintroduced, error)) {
                Audit(LogLevel::Warn, L"import", [&] {
                    return L"import.reconcile-absences-failed operationId=" +
                           std::to_wstring(importOperation) + L" reason=" + Limited(error);
                });
                ++failed;
            }
        }
        RefreshCount();
        Audit(failed ? LogLevel::Warn : LogLevel::Info, L"import", [&] {
            return L"import.result operationId=" + std::to_wstring(importOperation) + L" copied=" +
                   std::to_wstring(copied) + L" failed=" + std::to_wstring(failed) + L" cancelReason=" +
                   importCancelReason + L" durationMs=" + std::to_wstring(GetTickCount64() - importStarted) +
                   L" sourceUnchanged=1";
        });
        if (failed || cancelled() || !copied) {
            Failure(id,
                    cancelled() ? "E_CANCELLED"
                    : failed    ? "E_PARTIAL"
                                : "E_EMPTY",
                    L"导入未完整完成；已复制 " + std::to_wstring(copied) + L" 个文件，源目录未改动");
            return;
        }
        Respond(
            id, true,
            Pack({{"copied", copied},
                  {"summary", WideToUtf8(L"已导入 " + std::to_wstring(copied) + L" 个文件；原目录未改动")}}));
        Notify(L"旧缓存已导入，原目录未改动");
        return;
    }
    if (method == L"set_auto" || method == L"save_settings" || method == L"set_game_root" ||
        method == L"set_cache_root") {
        std::wstring error;
        Config old;
        if (!Config::LoadSafe(m_installDir + L"\\config.json", old)) {
            Failure(id, "E_CONFIG", Config::LastError());
            return;
        }
        const Config expected = old;
        const auto before = GetStatus();
        // A disk root changed by an external writer is not an approved/leased
        // active root. Never validate against the old root but persist a new one.
        if ((!old.gameRoot.empty() && !SameDirectory(old.gameRoot, before.gameRoot)) ||
            (!old.cacheRoot.empty() && !SameDirectory(old.cacheRoot, before.cacheRoot))) {
            Failure(id, "E_CONFIG", L"磁盘目录配置已被外部修改；本次设置未写入，请重新启动验证目录");
            return;
        }
        old.cacheRoot = before.cacheRoot;
        old.gameRoot = before.gameRoot;
        Config next = old;
        if (method == L"set_auto") {
            if (!params.contains("enabled") || !BoolField(params, "enabled", next.autoOn, error)) {
                Failure(id, "E_INPUT", error.empty() ? L"缺少enabled开关" : error);
                return;
            }
        } else if (method == L"save_settings") {
            if (!BoolField(params, "auto_on", next.autoOn, error) ||
                !BoolField(params, "autostart", next.autostart, error) ||
                !IntField(params, "theme", 0, 1, next.theme, error) ||
                !IntField(params, "close_action", 0, 2, next.closeAction, error) ||
                !IntField(params, "sync_minutes", 1, 1440, next.syncMinutes, error)) {
                Failure(id, "E_INPUT", error);
                return;
            }
            // Directory changes have dedicated validated entry points, never a partial mixed commit.
            if (params.contains("game_root") || params.contains("cache_root")) {
                const std::wstring rejected = params.contains("game_root") ? L"game_root" : L"cache_root";
                Failure(id, "E_INPUT",
                        L"设置事务已拒绝：预期 save_settings 不含路径字段，实际包含 " + rejected +
                            L"；位置=save_settings；其他设置未保存");
                return;
            }
        } else {
            std::wstring selected;
            if (!params.contains("path") || !PathField(params, "path", selected, error)) {
                Failure(id, "E_PATH", error.empty() ? L"未提供有效目录" : error);
                return;
            }
            if (method == L"set_game_root") {
                if (m_options.validateGameRoot && !GameDetect::IsGameRoot(selected)) {
                    Failure(id, "E_GAME", L"不是有效路径，请选择外服安装目录中的 League of Legends\\Game 文件夹");
                    return;
                }
                next.gameRoot = selected;
            } else {
                if (!before.gameRoot.empty() && !pathpolicy::AreDisjoint(selected, before.gameRoot, error)) {
                    Failure(id, "E_PATH", L"数据目录不能位于游戏目录内或覆盖其父目录。 " + error);
                    return;
                }
                next.cacheRoot = selected;
            }
        }
        if (!ValidateRoots(next.gameRoot, next.cacheRoot, error)) {
            Failure(id, "E_PATH", error);
            return;
        }
        const bool cacheChanged = !pathpolicy::EqualPath(next.cacheRoot, before.cacheRoot);
        const bool sourceRootsChanged =
            cacheChanged || !pathpolicy::EqualPath(next.gameRoot, before.gameRoot);
        if (!next.cacheRoot.empty() && !fileops::EnsureDir(next.cacheRoot)) {
            Failure(id, "E_DIR",
                    L"无法创建缓存目录；位置=" + next.cacheRoot + L"；Win32=" +
                        std::to_wstring(GetLastError()));
            return;
        }
        const bool registry =
            method == L"save_settings" && params.contains("autostart") && m_options.allowAutostart;
        RunValue priorRun;
        if (registry && !ReadRunValue(priorRun, error)) {
            Failure(id, "E_AUTOSTART", error);
            return;
        }
        if (registry && !ChangeAutostart(next.autostart, error)) {
            Failure(id, "E_AUTOSTART", error);
            return;
        }
        if (registry)
            Audit(LogLevel::Info, L"settings", [&] {
                return L"autostart.applied enabled=" + std::to_wstring(next.autostart) +
                       L" pendingConfigCommit=1";
            });
        Phase(L"settings-before-commit");
        Config committed;
        bool configConflict = false;
        if (!UpdateConfig(m_installDir + L"\\config.json", [&](Config& c) -> bool {
                if (c.gameRoot != expected.gameRoot || c.cacheRoot != expected.cacheRoot ||
                    c.autoOn != expected.autoOn || c.autostart != expected.autostart ||
                    c.theme != expected.theme || c.closeAction != expected.closeAction ||
                    c.syncMinutes != expected.syncMinutes) {
                    configConflict = true;
                    return false;
                }
                if (method == L"set_game_root")
                    c.gameRoot = next.gameRoot;
                if (method == L"set_cache_root")
                    c.cacheRoot = next.cacheRoot;
                if (method == L"set_auto" || (method == L"save_settings" && params.contains("auto_on")))
                    c.autoOn = next.autoOn;
                if (method == L"save_settings" && params.contains("autostart"))
                    c.autostart = next.autostart;
                if (method == L"save_settings" && params.contains("theme"))
                    c.theme = next.theme;
                if (method == L"save_settings" && params.contains("close_action"))
                    c.closeAction = next.closeAction;
                if (method == L"save_settings" && params.contains("sync_minutes"))
                    c.syncMinutes = next.syncMinutes;
                committed = c;
                return true;
            })) {
            std::wstring rollback;
            if (registry) {
                const bool restored = RestoreRunValue(priorRun, rollback);
                Audit(restored ? LogLevel::Info : LogLevel::Error, L"settings", [&] {
                    return L"autostart.rollback success=" + std::to_wstring(restored) + L" reason=" +
                           Limited(rollback);
                });
                if (!restored)
                    Notify(L"配置保存失败且开机启动回滚失败，请手动检查。 " + rollback, true);
            }
            if (configConflict && command.configRetries < 3) {
                Command retry = command;
                ++retry.configRetries;
                Audit(LogLevel::Warn, L"settings", [&] {
                    return L"config.conflict-retry attempt=" + std::to_wstring(retry.configRetries) +
                           L" maximum=3 activeConfigUnchanged=1";
                });
                ProcessCommand(retry);
                return;
            }
            Failure(id, "E_CONFIG",
                    configConflict ? L"配置持续被其他写入者修改，请重试"
                                   : L"设置未保存，内存状态未改动。 " + Config::LastError());
            return;
        }
        Audit(LogLevel::Info, L"settings", [&] {
            std::wstring changes;
            auto integer = [&](const wchar_t* key, auto oldValue, auto newValue) {
                if (oldValue != newValue)
                    changes += std::wstring(key) + L"=" + std::to_wstring(oldValue) + L"->" +
                               std::to_wstring(newValue) + L"; ";
            };
            integer(L"auto_on", old.autoOn, committed.autoOn);
            integer(L"autostart", old.autostart, committed.autostart);
            integer(L"theme", old.theme, committed.theme);
            integer(L"close_action", old.closeAction, committed.closeAction);
            integer(L"sync_minutes", old.syncMinutes, committed.syncMinutes);
            if (before.gameRoot != next.gameRoot)
                changes += L"game_root=" + Limited(before.gameRoot) + L"->" + Limited(next.gameRoot) + L"; ";
            if (before.cacheRoot != next.cacheRoot)
                changes +=
                    L"cache_root=" + Limited(before.cacheRoot) + L"->" + Limited(next.cacheRoot) + L"; ";
            return L"settings.commit 设置已保存 changedKeys/values=" + (changes.empty() ? L"none" : changes) +
                   L" registryApplied=" + std::to_wstring(registry) + L" retries=" +
                   std::to_wstring(command.configRetries) + L" cacheChanged=" + std::to_wstring(cacheChanged);
        });
        next.autoOn = committed.autoOn;
        next.autostart = committed.autostart;
        next.theme = committed.theme;
        next.closeAction = committed.closeAction;
        next.syncMinutes = committed.syncMinutes;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.gameRoot = next.gameRoot;
            m_status.cacheRoot = next.cacheRoot;
            m_status.installed = !next.gameRoot.empty();
            m_status.gameOk = !next.gameRoot.empty() &&
                              (!m_options.validateGameRoot || GameDetect::IsGameRoot(next.gameRoot));
            m_status.autoOn = next.autoOn;
            m_status.autostart = next.autostart;
            m_status.theme = next.theme;
            m_status.closeAction = next.closeAction;
            m_status.syncMinutes = next.syncMinutes;
            m_status.lastError.clear();
        }
        if (sourceRootsChanged) {
            const int selectedLobby =
                (m_options.enableMonitoring || m_options.monitorProbe) ? ProbeLobby(next.gameRoot) : 0;
            std::lock_guard<std::mutex> lock(m_queueMutex);
            ++m_rootEpoch;
            ++m_sessionEpoch;
            m_onlineSession = selectedLobby == 1 ? m_sessionEpoch.load() : 0;
            m_launchHandled = false;
            m_autoRestoreQueued = false;
            m_lobby = selectedLobby;
            if (selectedLobby == 1 && next.autoOn && m_schedulingEnabled.load() && !next.gameRoot.empty() && !next.cacheRoot.empty()) {
                m_pendingRestore = {0, L"@restore", L"{}", m_cancelEpoch.load(), m_sessionEpoch.load()};
                m_pendingRestore.rootEpoch = m_rootEpoch.load();
                Stamp(m_pendingRestore);
                m_autoRestoreQueued = true;
                m_wake.notify_one();
            }
        } else if (next.autoOn != before.autoOn) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!next.autoOn)
                m_autoRestoreQueued = false;
            else if (m_lobby.load() == 1 && !next.gameRoot.empty() && !next.cacheRoot.empty() && m_schedulingEnabled.load()) {
                m_lastSyncMs = Tick();
                if (!m_launchHandled.load()) {
                    m_onlineSession = m_sessionEpoch.load();
                    m_pendingRestore = {0, L"@restore", L"{}", m_cancelEpoch.load(), m_sessionEpoch.load()};
                    m_pendingRestore.rootEpoch = m_rootEpoch.load();
                    Stamp(m_pendingRestore);
                    m_autoRestoreQueued = true;
                    m_wake.notify_one();
                }
            }
        }
        Audit(LogLevel::Info, L"settings", [&] {
            return L"settings.published sourceRootsChanged=" + std::to_wstring(sourceRootsChanged) +
                   L" rootEpoch=" + std::to_wstring(m_rootEpoch.load()) + L" direct=1";
        });
        Phase(L"configuration-committed");
        m_monitorWake.notify_one();
        RefreshCount();
        PushStatusEvent();
        json data = StatusJson(GetStatus());
        if (method == L"set_cache_root")
            data["path"] = WideToUtf8(next.cacheRoot);
        if (method == L"set_game_root")
            data["path"] = WideToUtf8(next.gameRoot);
        Respond(id, true, Pack(data));
        return;
    }
    Failure(id, "E_UNKNOWN", L"不支持的操作: " + method);
}
} // namespace core
