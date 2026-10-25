// JSONL logger. Legacy fire-and-forget APIs delegate to observable Try* APIs.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
namespace core {
enum class LogLevel { Debug, Info, Warn, Error, Fatal };
// Thread-affine scope for an opaque, non-sensitive correlation token (never credentials).
// Nested scopes restore the previous trace; invalid/oversized/allocation-failed input is a no-op.
class ScopedLogContext {
  public:
    explicit ScopedLogContext(const std::wstring& trace) noexcept;
    ~ScopedLogContext() noexcept;
    ScopedLogContext(const ScopedLogContext&) = delete;
    ScopedLogContext& operator=(const ScopedLogContext&) = delete;
    ScopedLogContext(ScopedLogContext&&) = delete;
    ScopedLogContext& operator=(ScopedLogContext&&) = delete;

  private:
    std::string m_trace;
    const std::string* m_previous = nullptr;
    bool m_active = false;
};
struct LogTailStats {
    size_t bytesRead = 0;
    size_t filesOpened = 0;
    bool cacheHit = false;
};
class Logger {
  public:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    // A successful closed -> open transition starts a unique run_id and seq at 1.
    // Repeating Open with the already-open path/options is idempotent, not a new run.
    bool Open(const std::wstring& path, long long maxBytes = 2 * 1024 * 1024, int maxFiles = 5);
    bool TryLog(LogLevel level, const std::wstring& module, const std::wstring& message);
    bool TryClose();
    std::wstring LastError() const;
    void Log(LogLevel level, const std::wstring& module, const std::wstring& message);
    void Close();
    void Info(const std::wstring& module, const std::wstring& msg) {
        Log(LogLevel::Info, module, msg);
    }
    void Warn(const std::wstring& module, const std::wstring& msg) {
        Log(LogLevel::Warn, module, msg);
    }
    void Error(const std::wstring& module, const std::wstring& msg) {
        Log(LogLevel::Error, module, msg);
    }
    // All-or-nothing out: false leaves out unchanged and supplies a friendly error.
    // Missing rotations are normal; missing current file and all rotations is an error.
    static bool TryExportAll(const std::wstring& path, std::wstring& out, std::wstring* error = nullptr,
                             int maxFiles = 5);
    // Raw JSONL lines (one record per line) for callers that parse fields themselves.
    static bool TryReadRaw(const std::wstring& path, std::wstring& out, std::wstring* error = nullptr,
                           int maxFiles = 5);
    // Preview only: complete UTF-8 records without CR/LF, oldest -> newest; never cuts JSON.
    // maxBytes is a soft aggregate budget: one newest record may exceed it (up to 6 MiB + 4 KiB).
    // Requests are capped at 10,000 records / 8 MiB; bounded reverse I/O, not a full-history read.
    // Unterminated final lines are incomplete and omitted. Invalid UTF-8/oversized records fail.
    // Same-process rotation snapshot: pin handles + sizes under allLogsMutex, read outside it.
    // Concurrent appends are excluded by snapshot size; rotations retain the pinned file identity.
    // External in-place overwrite/truncation is unsupported (detected short reads fail atomically).
    // A bounded one-entry identity/size/mtime cache avoids reading unchanged previews again.
    static bool TryReadRawTail(const std::wstring& path, std::vector<std::string>& records,
                               std::wstring* error = nullptr, size_t maxRecords = 200,
                               size_t maxBytes = 256 * 1024, int maxFiles = 5, LogTailStats* stats = nullptr);
    static std::wstring ExportAll(const std::wstring& path);

  private:
    std::wstring m_path;
    std::wstring m_error;
    long long m_maxBytes = 0;
    int m_maxFiles = 0;
    void* m_file = nullptr;
    std::string m_runId;
    std::uint64_t m_sequence = 0;
    std::uint64_t m_openTick = 0;
    mutable std::mutex m_mutex;
    // Lifecycle/write helpers require allLogsMutex followed by m_mutex.
    bool OpenRun();
    bool OpenHandle();
    bool TryLogLocked(LogLevel level, const std::wstring& module, const std::wstring& message);
    bool TryCloseLocked();
    bool RotateIfNeeded();
    bool SetError(const std::wstring& message, unsigned long code = 0);
};
Logger& Log();
} // namespace core
