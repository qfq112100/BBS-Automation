// Service owns serialized file jobs; monitoring and cancellation never wait behind IO.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../common/limits_shared.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace core {
struct Config;
class Service {
  public:
    static Service& Instance();
    struct StartOptions {
        bool allowAutostart = true;
        bool enableMonitoring = true;
        bool enableScheduling = true;
        bool enableAutodetect = false;
        bool validateGameRoot = true;
        std::function<int(const std::wstring&)> monitorProbe; // -1 unknown, 0 offline, 1 online
        int monitorIntervalMs = 500;
        int retryDelayMs = common::kStartRetryDelayMs;
        // Test/embedding override for the lobby-online mirror period. 0 uses syncMinutes*60000.
        int syncPeriodMs = 0;
        std::function<int(const std::wstring&)> gameProbe;
        // Deterministic embedding/test barrier, invoked outside service locks.
        std::function<void(const std::wstring&)> onPhase;
        std::function<ULONGLONG()> monotonicClock;
    };
    struct Event {
        std::wstring json;
    };
    struct Status {
        bool installed = false, autoOn = true, lobbyOnline = false, monitorKnown = false;
        std::wstring backupState = L"ready", monitorState = L"stopped";
        std::wstring lastSync = L"--", nextSync = L"--", lastResult = L"尚未执行备份", lastError;
        std::wstring lobbyCycle = L"等待大厅检测";
        std::wstring gameRoot, cacheRoot;
        bool gameOk = false, autostart = false;
        int theme = 0, closeAction = 0, syncMinutes = 5;
        int cacheEntries = 0, progressPercent = -1;
        std::wstring progressLabel;
        unsigned long long operationId = 0;
        // Switch preference is not execution readiness; expose the actual automatic gates.
        bool periodicReady = false, matchBackupReady = false;
        std::wstring autoState = L"stopped", autoReason = L"服务尚未运行";
    };
    void Start(HWND hwnd, const std::wstring& installDir, const std::wstring& cacheRoot);
    bool StartWithOptions(HWND hwnd, const std::wstring& installDir, const std::wstring& cacheRoot,
                          const StartOptions& options);
    void RequestStop() noexcept;
    void Stop(); // Call outside the UI thread; UI keeps pumping until this completes.
    bool IsRunning() const noexcept {
        return m_running.load();
    }
    bool IsBusy() const noexcept {
        return m_busy.load();
    }
    void PostCommand(int reqId, const std::wstring& method, const std::wstring& paramsJson);
    std::vector<Event> DrainEvents();
    Status GetStatus();
    // Periodic lobby policy: only an actually-running lobby may trigger scheduled work.
    // Offline/unknown never authorize automatic I/O; no match-end/offline exception exists.
    static bool AutoBackupDue(bool autoOn, bool schedulingEnabled, int lobby, unsigned long long elapsedMs,
                              unsigned long long periodMs) noexcept {
        return autoOn && schedulingEnabled && lobby == 1 && periodMs > 0 && elapsedMs >= periodMs;
    }
    void BackupNow();
    void RestoreNow();
    ~Service();

  private:
    Service() = default;
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    struct Command {
        int id = 0;
        std::wstring method, params;
        unsigned long long cancelEpoch = 0;
        unsigned long long sessionEpoch = 0;
        int configRetries = 0;
        unsigned long long rootEpoch = 0; // Bind queued tasks to the selected directory pair.
        // Diagnostics only: append fields so existing aggregate/token initialization stays unchanged.
        unsigned long long traceId = 0;
        ULONGLONG enqueuedAt = 0;
    };
    void Stamp(Command& command) noexcept;
    static std::wstring Trace(const Command& command) noexcept;
    void AuditProbe(bool game, int state, DWORD error, const wchar_t* reason, unsigned long long rootEpoch,
                    bool writeSafety = false) noexcept;
    void ThreadMain() noexcept;
    void MonitorMain() noexcept;
    void ProcessCommand(const Command& command);
    void RunOperation(const Command& command, bool restore, bool automatic);
    bool PeriodicSessionReady(const Command& command) const noexcept;
    void PushEvent(const std::wstring& json);
    void PushStatusEvent();
    void Respond(int id, bool ok, const std::wstring& data);
    void Failure(int id, const std::string& code, const std::wstring& message);
    void Notify(const std::wstring& message, bool error = false);
    void RefreshCount();
    int ProbeLobby(const std::wstring& gameRoot);
    void Phase(const std::wstring& phase);
    ULONGLONG Tick() const;
    bool ValidateRoots(const std::wstring& game, const std::wstring& cache, std::wstring& error);
    std::atomic<bool> m_running{false}, m_busy{false}, m_jobQueued{false}, m_cancelRequested{false};
    // False only while the launch copy is queued/running; not a persistent success/approval gate.
    std::atomic<bool> m_launchHandled{false};
    std::atomic<unsigned long long> m_sessionEpoch{0}, m_rootEpoch{0}, m_onlineSession{0};
    std::atomic<int> m_lobby{-1};
    std::atomic<bool> m_restoreInProgress{false};
    std::atomic<bool> m_schedulingEnabled{false};
    std::atomic<unsigned long long> m_cancelEpoch{0};
    std::atomic<DWORD> m_workerThreadId{0};
    std::atomic<ULONGLONG> m_lastSyncMs{0}, m_lastProgressMs{0};
    std::thread m_worker, m_monitor;
    std::mutex m_stopMutex, m_queueMutex, m_eventMutex, m_statusMutex;
    std::condition_variable m_wake, m_monitorWake;
    std::queue<Command> m_commands;
    std::vector<Event> m_events;
    Status m_status;
    HWND m_hwnd = nullptr;
    std::wstring m_installDir;
    StartOptions m_options;
    std::atomic<bool> m_autoRestoreQueued{
        false};               // Commands serialized by queue mutex; status may sample atomically.
    Command m_pendingRestore; // Immutable tokens captured at the real launch edge.
    unsigned long long m_nextOperation = 0; // Worker only.
    std::atomic<unsigned long long> m_nextTrace{0}, m_activeOperation{0};
    std::atomic<ULONGLONG> m_stopStartedAt{0}, m_eventDropNoticeAt{0}, m_eventWakeNoticeAt{0};
    std::atomic<unsigned long long> m_eventDrops{0};
    std::atomic<bool> m_stopLogged{true};
    struct ProbeAudit {
        bool initialized = false;
        unsigned long long rootEpoch = 0, suppressed = 0;
        int state = -2;
        DWORD error = ERROR_SUCCESS;
        std::wstring reason;
        ULONGLONG loggedAt = 0;
    };
    std::mutex m_probeAuditMutex;
    ProbeAudit m_probeAudit[3]; // lobby, own-session match, all-sessions Game write guard.
};
} // namespace core
