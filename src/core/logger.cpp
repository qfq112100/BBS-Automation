#include "logger.h"
#include "../common/winutil.h"
#include "utf.h"
#include <algorithm>
#include <limits>
#include <objbase.h>
#include <stdexcept>
#include <vector>
#ifdef LOL_AUX_TEST_HOOKS
#include "../../tests/private/aux_test_hooks.h"
#endif
#define JSON_DIAGNOSTICS 0
#include "../../third_party/json/json.hpp"
using json = nlohmann::json;
namespace core {
namespace {
// Serialize exports with in-process writers/rotations for a coherent file-set snapshot.
std::mutex allLogsMutex;
// Shared by every Logger, accessed only under allLogsMutex. The GUID also separates processes.
std::uint64_t runSequence = 0;
constexpr size_t MaxInputBytes = 1024 * 1024;
// Points only into live, nonmovable ScopedLogContext objects on this thread; no global trace state.
thread_local const std::string* currentTrace = nullptr;
void DebugFailure(const wchar_t* operation, const std::wstring& reason) noexcept {
    // No logging recursion, formatting allocations, or changes to Logger::LastError()/Win32 last-error.
    const DWORD savedError = GetLastError();
    OutputDebugStringW(operation);
    OutputDebugStringW(reason.c_str());
    OutputDebugStringW(L"\n");
    SetLastError(savedError);
}
constexpr size_t TailChunkBytes = 64 * 1024;
// A 1 MiB logger input can expand six-fold when JSON escapes control characters.
constexpr size_t TailMaxRecordBytes = 6 * 1024 * 1024 + 4096;
struct TailCache {
    std::mutex mutex;
    std::wstring path;
    std::vector<unsigned long long> key;
    std::vector<std::string> records;
};
TailCache tailCache;
#ifdef LOL_AUX_TEST_HOOKS
thread_local std::function<void()> tailSnapshotHook;
#endif
bool FullPath(const std::wstring& input, std::wstring& path) {
    if (input.empty() || input.size() > 32700)
        return false;
    for (wchar_t c : input)
        if (c < 32)
            return false;
    std::string encoded;
    if (!TryWideToUtf8(input, encoded))
        return false;
    std::wstring normalized = input;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (normalized.compare(0, 4, L"\\\\.\\") == 0)
        return false;
    DWORD n = GetFullPathNameW(normalized.c_str(), 0, nullptr, nullptr);
    if (!n || n > 32700)
        return false;
    std::vector<wchar_t> buffer(n);
    DWORD count = GetFullPathNameW(normalized.c_str(), n, buffer.data(), nullptr);
    if (!count || count >= n)
        return false;
    path = winutil::LongPath(std::wstring(buffer.data(), count));
    return true;
}
bool EnsureParent(const std::wstring& path, DWORD& error) {
    const size_t last = path.find_last_of(L'\\');
    if (last == std::wstring::npos) {
        error = ERROR_INVALID_NAME;
        return false;
    }
    const std::wstring dir = path.substr(0, last);
    size_t root = 7; // Extended drive-root prefix length.
    if (dir.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        size_t server = dir.find(L'\\', 8);
        if (server == std::wstring::npos) {
            error = ERROR_BAD_NETPATH;
            return false;
        }
        size_t share = dir.find(L'\\', server + 1);
        root = share == std::wstring::npos ? dir.size() : share + 1;
    }
    if (dir.size() <= root)
        return true;
    for (size_t end = root; end <= dir.size(); ++end) {
        if (end < dir.size() && dir[end] != L'\\')
            continue;
        if (end <= root)
            continue;
        std::wstring current = dir.substr(0, end);
        if (!CreateDirectoryW(current.c_str(), nullptr)) {
            DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS) {
                error = code;
                return false;
            }
            DWORD attrs = GetFileAttributesW(current.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                error = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
                return false;
            }
        }
    }
    return true;
}
// Called under allLogsMutex. Ordinary MoveFileEx replacement can reject an open target
// even with FILE_SHARE_DELETE. Rename the target aside first; pinned readers keep its
// identity, and DeleteFile retires it when the final reader closes (no name reuse).
unsigned long long retiredSequence = 0;
bool RenameAside(const std::wstring& path, std::wstring& retired, DWORD& code) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        retired = path + L".retired." + std::to_wstring(GetCurrentProcessId()) + L"." +
                  std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++retiredSequence);
        if (MoveFileExW(path.c_str(), retired.c_str(), 0))
            return true;
        code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS)
            return false;
    }
    return false;
}
bool MoveRotation(const std::wstring& from, const std::wstring& to, DWORD& code, std::wstring& detail) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    code = GetLastError();
    if (code != ERROR_ACCESS_DENIED)
        return false;
    // Only apply this compatibility fallback to an ordinary writable, unaliased target.
    // A source sharing violation/read-only target must not destroy the older evidence.
    winutil::ScopedHandle target(CreateFileW(to.c_str(), FILE_READ_ATTRIBUTES | DELETE,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                             OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!target.valid() || !GetFileInformationByHandle(target.get(), &info) ||
        GetFileType(target.get()) != FILE_TYPE_DISK || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY)))
        return false;
    std::wstring retired;
    if (!RenameAside(to, retired, code))
        return false;
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
        code = GetLastError();
        if (!MoveFileExW(retired.c_str(), to.c_str(), 0))
            detail = L" 较旧日志回滚失败，原始记录保留于：" + retired;
        return false;
    }
    target.reset();
    if (!DeleteFileW(retired.c_str())) {
        code = GetLastError();
        detail = L" 已退休的旧日志清理失败，原始记录保留于：" + retired;
        return false;
    }
    return true;
}
std::wstring ErrorText(const std::wstring& message, DWORD code) {
    return code ? message + L" (Windows error " + std::to_wstring(code) + L")" : message;
}
} // namespace
ScopedLogContext::ScopedLogContext(const std::wstring& trace) noexcept {
    try {
        if (trace.size() > MaxInputBytes || !TryWideToUtf8(trace, m_trace) || m_trace.size() > MaxInputBytes)
            return;
        m_previous = currentTrace;
        currentTrace = &m_trace;
        m_active = true;
    } catch (...) {
        // Context construction must never interrupt the operation being diagnosed.
    }
}
ScopedLogContext::~ScopedLogContext() noexcept {
    if (m_active)
        currentTrace = m_previous;
}
bool Logger::SetError(const std::wstring& message, unsigned long code) {
    m_error = ErrorText(message, code);
    return false;
}
std::wstring Logger::LastError() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_error;
}
Logger::~Logger() {
    try {
        TryClose();
    } catch (...) { /* destructor must not throw */
    }
}
bool Logger::OpenRun() {
    if (runSequence == (std::numeric_limits<std::uint64_t>::max)())
        return SetError(L"日志运行标识计数已耗尽。");
    GUID guid{};
    const HRESULT result = CoCreateGuid(&guid); // No COM initialization required; no wall-clock dependency.
    if (FAILED(result))
        return SetError(L"无法生成日志运行标识。", static_cast<DWORD>(result));
    wchar_t text[40]{};
    const int count = StringFromGUID2(guid, text, 40);
    std::string runId;
    if (count <= 1 || !TryWideToUtf8(std::wstring(text, static_cast<size_t>(count - 1)), runId))
        return SetError(L"无法编码日志运行标识。");
    runId += "-" + std::to_string(++runSequence);
    if (!OpenHandle())
        return false;
    // Commit metadata only after the handle is valid, under both lifecycle locks.
    m_runId.swap(runId);
    m_sequence = 0;
    m_openTick = GetTickCount64();
    return true;
}
bool Logger::OpenHandle() {
    if (m_path.empty())
        return SetError(L"日志尚未指定路径。");
    winutil::ScopedHandle file(CreateFileW(m_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                           nullptr, OPEN_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid())
        return SetError(L"无法打开日志文件。", GetLastError());
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info))
        return SetError(L"无法检查日志文件。", GetLastError());
    if (GetFileType(file.get()) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1)
        return SetError(L"日志路径必须是非重解析、单硬链接的普通磁盘文件。");
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file.get(), zero, nullptr, FILE_END))
        return SetError(L"无法定位日志末尾。", GetLastError());
    m_file = file.release();
    return true;
}
bool Logger::Open(const std::wstring& path, long long maxBytes, int maxFiles) {
    std::lock_guard<std::mutex> exportGuard(allLogsMutex);
    std::lock_guard<std::mutex> guard(m_mutex);
    m_error.clear();
    try {
        std::wstring canonical;
        if (!FullPath(path, canonical) || maxBytes < 0 || maxFiles < 0 || maxFiles > 100)
            return SetError(L"日志路径、轮转大小或数量无效（最多 100 份轮转）。");
        if (m_file) {
            if (_wcsicmp(canonical.c_str(), m_path.c_str()) == 0 && maxBytes == m_maxBytes &&
                maxFiles == m_maxFiles)
                return true;
            return SetError(L"日志已打开在另一条路径或使用不同轮转参数，请先关闭。");
        }
        DWORD code = 0;
        if (!EnsureParent(canonical, code))
            return SetError(L"无法创建日志目录。", code);
        m_path = canonical;
        m_maxBytes = maxBytes;
        m_maxFiles = maxFiles;
        return OpenRun();
    } catch (...) {
        return SetError(L"打开日志时发生异常或内存不足。");
    }
}
bool Logger::RotateIfNeeded() {
    if (!m_file || m_maxBytes == 0)
        return true;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(static_cast<HANDLE>(m_file), &size))
        return SetError(L"无法读取日志大小。", GetLastError());
    if (size.QuadPart < m_maxBytes)
        return true;
    if (!FlushFileBuffers(static_cast<HANDLE>(m_file)))
        return SetError(L"日志轮转前刷盘失败。", GetLastError());
    HANDLE old = static_cast<HANDLE>(m_file);
    m_file = nullptr;
    if (!CloseHandle(old))
        return SetError(L"日志轮转前关闭失败。", GetLastError());
    auto recover = [&](const std::wstring& message, DWORD code) {
        std::wstring original = ErrorText(message, code);
        if (!OpenHandle())
            original += L" " + m_error;
        m_error = std::move(original);
        return false;
    };
    if (m_maxFiles == 0) {
        std::wstring retired;
        DWORD code = 0;
        if (!RenameAside(m_path, retired, code))
            return recover(L"无法退休已满日志。", code);
        if (!DeleteFileW(retired.c_str())) {
            code = GetLastError();
            const bool restored = MoveFileExW(retired.c_str(), m_path.c_str(), 0) != FALSE;
            return recover(
                L"无法删除已满日志。" + (restored ? std::wstring() : L" 原始记录保留于：" + retired), code);
        }
    } else {
        // Stop on a real move error: continuing would overwrite an unmoved older record.
        for (int i = m_maxFiles - 1; i >= 1; --i) {
            std::wstring from = m_path + L"." + std::to_wstring(i);
            std::wstring to = m_path + L"." + std::to_wstring(i + 1);
            DWORD code = 0;
            std::wstring detail;
            if (!MoveRotation(from, to, code, detail) && code != ERROR_FILE_NOT_FOUND)
                return recover(L"日志轮转失败。" + detail, code);
        }
        DWORD code = 0;
        std::wstring detail;
        if (!MoveRotation(m_path, m_path + L".1", code, detail))
            return recover(L"当前日志轮转失败。" + detail, code);
    }
    return OpenHandle();
}
bool Logger::TryLog(LogLevel level, const std::wstring& module, const std::wstring& message) {
    std::lock_guard<std::mutex> exportGuard(allLogsMutex);
    std::lock_guard<std::mutex> guard(m_mutex);
    return TryLogLocked(level, module, message);
}
bool Logger::TryLogLocked(LogLevel level, const std::wstring& module, const std::wstring& message) {
    m_error.clear();
    try {
        std::string encodedModule, encodedMessage;
        if (!TryWideToUtf8(module, encodedModule) || !TryWideToUtf8(message, encodedMessage))
            return SetError(L"日志包含无效 UTF-16。");
        const std::string emptyTrace;
        const std::string& trace = currentTrace ? *currentTrace : emptyTrace;
        if (encodedModule.size() > MaxInputBytes || encodedMessage.size() > MaxInputBytes ||
            trace.size() > MaxInputBytes ||
            encodedModule.size() + encodedMessage.size() + trace.size() > MaxInputBytes)
            return SetError(L"单条日志超过 1 MiB（含 trace）。");
        // Preserve legacy lazy re-open, but Close ends its run. Rotation recovery retains the run.
        if (!m_file && !(m_runId.empty() ? OpenRun() : OpenHandle()))
            return false;
        if (m_sequence == (std::numeric_limits<std::uint64_t>::max)())
            return SetError(L"日志序号已耗尽，请关闭后重新打开日志。");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(static_cast<HANDLE>(m_file), &info))
            return SetError(L"无法重新检查日志文件。", GetLastError());
        if (GetFileType(static_cast<HANDLE>(m_file)) != FILE_TYPE_DISK ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.nNumberOfLinks != 1)
            return SetError(L"日志文件不再是单硬链接普通磁盘文件，已拒绝写入。");
        // Reject a changed current leaf before any rotation; OpenHandle revalidates the new leaf.
        if (!RotateIfNeeded())
            return false;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t timestamp[40]{};
        swprintf(timestamp, 40, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond, st.wMilliseconds);
        const char* label = "info";
        switch (level) {
        case LogLevel::Debug:
            label = "debug";
            break;
        case LogLevel::Info:
            break;
        case LogLevel::Warn:
            label = "warn";
            break;
        case LogLevel::Error:
            label = "error";
            break;
        case LogLevel::Fatal:
            label = "fatal";
            break;
        default:
            return SetError(L"未知日志级别。");
        }
        json entry = {{"ts", WideToUtf8(timestamp)},
                      {"level", label},
                      {"module", encodedModule},
                      {"msg", encodedMessage},
                      {"pid", GetCurrentProcessId()},
                      {"tid", GetCurrentThreadId()},
                      {"seq", m_sequence + 1},
                      {"run_id", m_runId},
                      {"uptime_ms", GetTickCount64() - m_openTick},
                      {"trace", trace}};
        std::string line = entry.dump() + "\n";
        // Reserve before WriteFile: a partial write/flush failure must never reuse an emitted seq.
        ++m_sequence;
        DWORD count = 0;
        if (!WriteFile(static_cast<HANDLE>(m_file), line.data(), static_cast<DWORD>(line.size()), &count,
                       nullptr))
            return SetError(L"日志写入失败。", GetLastError());
        if (count != line.size())
            return SetError(L"日志写入不完整。", ERROR_WRITE_FAULT);
        if ((level == LogLevel::Error || level == LogLevel::Fatal) &&
            !FlushFileBuffers(static_cast<HANDLE>(m_file)))
            return SetError(L"错误日志刷盘失败。", GetLastError());
        return true;
    } catch (...) {
        return SetError(L"日志序列化失败或内存不足。");
    }
}
void Logger::Log(LogLevel level, const std::wstring& module, const std::wstring& message) {
    std::lock_guard<std::mutex> exportGuard(allLogsMutex);
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!TryLogLocked(level, module, message))
        DebugFailure(L"[logger] log failed: ", m_error);
}
bool Logger::TryClose() {
    std::lock_guard<std::mutex> exportGuard(allLogsMutex);
    std::lock_guard<std::mutex> guard(m_mutex);
    return TryCloseLocked();
}
bool Logger::TryCloseLocked() {
    m_error.clear();
    m_runId.clear();
    m_sequence = 0;
    m_openTick = 0;
    if (!m_file)
        return true;
    HANDLE file = static_cast<HANDLE>(m_file);
    m_file = nullptr;
    DWORD code = ERROR_SUCCESS;
    if (!FlushFileBuffers(file))
        code = GetLastError();
    if (!CloseHandle(file) && code == ERROR_SUCCESS)
        code = GetLastError();
    if (code != ERROR_SUCCESS)
        return SetError(L"日志关闭或刷盘失败。", code);
    return true;
}
void Logger::Close() {
    std::lock_guard<std::mutex> exportGuard(allLogsMutex);
    std::lock_guard<std::mutex> guard(m_mutex);
    if (!TryCloseLocked())
        DebugFailure(L"[logger] close failed: ", m_error);
}
bool Logger::TryExportAll(const std::wstring& path, std::wstring& out, std::wstring* error, int maxFiles) {
    std::lock_guard<std::mutex> exportGuard(allLogsMutex);
    try {
        auto fail = [&](const std::wstring& message, DWORD code = 0) {
            if (error)
                *error = ErrorText(message, code);
            return false;
        };
        std::wstring canonical;
        if (!FullPath(path, canonical) || maxFiles < 0 || maxFiles > 100)
            return fail(L"日志路径或轮转数量无效。");
        std::wstring result;
        size_t totalBytes = 0;
        bool found = false;
        for (int i = maxFiles; i >= 0; --i) {
            std::wstring candidate = canonical + (i ? L"." + std::to_wstring(i) : L"");
            winutil::ScopedHandle file(CreateFileW(
                candidate.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!file.valid()) {
                DWORD code = GetLastError();
                if (code == ERROR_FILE_NOT_FOUND)
                    continue;
                return fail(L"无法打开日志导出文件。", code);
            }
            found = true;
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(file.get(), &info))
                return fail(L"无法检查日志导出文件。", GetLastError());
            if (GetFileType(file.get()) != FILE_TYPE_DISK ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                info.nNumberOfLinks != 1)
                return fail(L"日志导出路径不是普通文件。");
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.get(), &size))
                return fail(L"无法读取日志文件大小。", GetLastError());
            if (size.QuadPart < 0 || size.QuadPart > 50 * 1024 * 1024 ||
                totalBytes + static_cast<size_t>(size.QuadPart) > 50 * 1024 * 1024)
                return fail(L"日志导出总量超过 50 MiB。");
            totalBytes += static_cast<size_t>(size.QuadPart);
            std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
            if (!bytes.empty()) {
                DWORD count = 0;
                if (!ReadFile(file.get(), &bytes[0], static_cast<DWORD>(bytes.size()), &count, nullptr))
                    return fail(L"日志读取失败。", GetLastError());
                if (count != bytes.size())
                    return fail(L"日志读取不完整。", ERROR_HANDLE_EOF);
            }
            size_t pos = 0;
            while (pos < bytes.size()) {
                size_t end = bytes.find('\n', pos);
                if (end == std::string::npos)
                    end = bytes.size();
                std::string line = bytes.substr(pos, end - pos);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty()) {
                    std::wstring readable;
                    try {
                        json entry = json::parse(line, [](int depth, json::parse_event_t, json&) {
                            if (depth > 64)
                                throw std::runtime_error("log nesting exceeds 64");
                            return true;
                        });
                        std::string text = entry.at("ts").get<std::string>() + " [" +
                                           entry.at("level").get<std::string>() + "] " +
                                           entry.at("module").get<std::string>() + ": " +
                                           entry.at("msg").get<std::string>();
                        if (!TryUtf8ToWide(text, readable))
                            return fail(L"日志内容包含无效 UTF-8。");
                    } catch (...) {
                        // Legacy/plain-text lines are intentionally retained, but never replace bad UTF-8
                        // silently.
                        if (!TryUtf8ToWide(line, readable))
                            return fail(L"日志内容包含无效 UTF-8。");
                    }
                    result += readable + L"\r\n";
                }
                pos = end == bytes.size() ? end : end + 1;
            }
        }
        if (!found)
            return fail(L"日志文件尚不存在。");
        out.swap(result);
        if (error)
            error->clear();
        return true;
    } catch (...) {
        if (error)
            *error = L"日志导出发生异常或内存不足。";
        return false;
    }
}
#ifdef LOL_AUX_TEST_HOOKS
void aux_test::SetTailSnapshotHook(std::function<void()> hook) {
    tailSnapshotHook = std::move(hook);
}
#endif
bool Logger::TryReadRawTail(const std::wstring& path, std::vector<std::string>& records, std::wstring* error,
                            size_t maxRecords, size_t maxBytes, int maxFiles, LogTailStats* stats) {
    LogTailStats work;
    auto fail = [&](const std::wstring& message, DWORD code = 0) {
        if (error)
            *error = ErrorText(message, code);
        if (stats)
            *stats = work;
        return false;
    };
    try {
        std::wstring canonical;
        if (!FullPath(path, canonical) || maxFiles < 0 || maxFiles > 100 || maxRecords == 0 ||
            maxRecords > 10000 || maxBytes == 0 || maxBytes > 8 * 1024 * 1024)
            return fail(L"日志尾部读取参数无效。");
        struct Snapshot {
            winutil::ScopedHandle file;
            LONGLONG size;
            Snapshot(winutil::ScopedHandle& value, LONGLONG bytes) noexcept
                : file(value.release()), size(bytes) {}
            Snapshot(Snapshot&& other) noexcept : file(other.file.release()), size(other.size) {}
            Snapshot(const Snapshot&) = delete;
            Snapshot& operator=(const Snapshot&) = delete;
        };
        std::vector<Snapshot> snapshots;
        std::vector<unsigned long long> key = {maxRecords, maxBytes, static_cast<size_t>(maxFiles)};
        {
            // Only filesystem metadata/handle acquisition is serialized with writers.
            // DELETE sharing lets rotation rename/remove names without invalidating these handles.
            std::lock_guard<std::mutex> guard(allLogsMutex);
            for (int i = 0; i <= maxFiles; ++i) {
                const std::wstring candidate = canonical + (i ? L"." + std::to_wstring(i) : L"");
                winutil::ScopedHandle file(CreateFileW(
                    candidate.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (!file.valid()) {
                    const DWORD code = GetLastError();
                    if (code == ERROR_FILE_NOT_FOUND) {
                        key.insert(key.end(), {static_cast<unsigned>(i), 0});
                        continue;
                    }
                    return fail(L"无法打开日志尾部文件。", code);
                }
                ++work.filesOpened;
                BY_HANDLE_FILE_INFORMATION info{};
                if (!GetFileInformationByHandle(file.get(), &info))
                    return fail(L"无法检查日志尾部文件。", GetLastError());
                if (GetFileType(file.get()) != FILE_TYPE_DISK ||
                    (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                    info.nNumberOfLinks != 1)
                    return fail(L"日志路径必须是非重解析、单硬链接的普通磁盘文件。");
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0)
                    return fail(L"无法读取日志尾部大小。", GetLastError());
                key.insert(key.end(),
                           {static_cast<unsigned>(i), 1, info.dwVolumeSerialNumber, info.nFileIndexHigh,
                            info.nFileIndexLow, static_cast<unsigned long long>(size.QuadPart),
                            info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime});
                snapshots.emplace_back(file, size.QuadPart);
            }
        }
        if (snapshots.empty())
            return fail(L"日志文件尚不存在。");
#ifdef LOL_AUX_TEST_HOOKS
        if (tailSnapshotHook)
            tailSnapshotHook();
#endif
        {
            std::lock_guard<std::mutex> guard(tailCache.mutex);
            if (tailCache.path == canonical && tailCache.key == key) {
                auto result = tailCache.records;
                records.swap(result);
                work.cacheHit = true;
                if (stats)
                    *stats = work;
                if (error)
                    error->clear();
                return true;
            }
        }
        std::vector<std::string> newestFirst;
        size_t selectedBytes = 0;
        bool done = false;
        // Includes one incomplete suffix and one complete oversize/budget-boundary record.
        const size_t readLimit = maxBytes + 2 * TailMaxRecordBytes + TailChunkBytes;
        auto finishLine = [&](std::string& reversed) {
            if (reversed.empty())
                return;
            std::reverse(reversed.begin(), reversed.end());
            if (reversed.back() == '\r')
                reversed.pop_back();
            if (reversed.empty())
                return;
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reversed.data(),
                                     static_cast<int>(reversed.size()), nullptr, 0))
                throw std::runtime_error("invalid log UTF-8");
            if (!newestFirst.empty() && reversed.size() + 1 > maxBytes - selectedBytes) {
                done = true;
                return;
            }
            selectedBytes += reversed.size() + 1;
            newestFirst.push_back(std::move(reversed));
            reversed.clear();
            done = newestFirst.size() >= maxRecords || selectedBytes >= maxBytes;
        };
        std::vector<char> block(TailChunkBytes);
        for (auto& snapshot : snapshots) {
            if (done)
                break;
            LONGLONG position = snapshot.size;
            bool haveEnd = false;
            std::string reversed;
            while (position > 0 && !done) {
                if (work.bytesRead >= readLimit)
                    return fail(L"日志尾部扫描已达到安全字节上限。");
                const DWORD count = static_cast<DWORD>(std::min<unsigned long long>(
                    std::min(TailChunkBytes, readLimit - work.bytesRead), position));
                position -= count;
                LARGE_INTEGER offset{};
                offset.QuadPart = position;
                DWORD actual = 0;
                if (!SetFilePointerEx(snapshot.file.get(), offset, nullptr, FILE_BEGIN) ||
                    !ReadFile(snapshot.file.get(), block.data(), count, &actual, nullptr))
                    return fail(L"日志尾部读取失败。", GetLastError());
                work.bytesRead += actual;
                if (actual != count)
                    return fail(L"日志在尾部读取期间被截断，请重试。", ERROR_HANDLE_EOF);
                for (size_t n = actual; n > 0 && !done; --n) {
                    const char c = block[n - 1];
                    if (c == '\n') {
                        if (haveEnd)
                            finishLine(reversed);
                        reversed.clear();
                        haveEnd = true;
                    } else {
                        // Count even an unterminated suffix, but never return that partial record.
                        if (reversed.size() >= TailMaxRecordBytes)
                            return fail(L"单条日志超过尾部读取安全上限。");
                        reversed.push_back(c);
                    }
                }
            }
            if (!done && haveEnd)
                finishLine(reversed);
        }
        std::reverse(newestFirst.begin(), newestFirst.end());
        // Cache allocation failure does not turn a successfully read preview into failure.
        try {
            auto cached = newestFirst;
            std::lock_guard<std::mutex> guard(tailCache.mutex);
            tailCache.path = canonical;
            tailCache.key = std::move(key);
            tailCache.records.swap(cached);
        } catch (...) {
            std::lock_guard<std::mutex> guard(tailCache.mutex);
            tailCache.path.clear();
            tailCache.key.clear();
            tailCache.records.clear();
        }
        records.swap(newestFirst);
        if (stats)
            *stats = work;
        if (error)
            error->clear();
        return true;
    } catch (...) {
        return fail(L"日志尾部内容无效、读取异常或内存不足。");
    }
}
bool Logger::TryReadRaw(const std::wstring& path, std::wstring& out, std::wstring* error, int maxFiles) {
    std::lock_guard<std::mutex> exportGuard(allLogsMutex);
    try {
        auto fail = [&](const std::wstring& message, DWORD code = 0) {
            if (error)
                *error = ErrorText(message, code);
            return false;
        };
        std::wstring canonical;
        if (!FullPath(path, canonical) || maxFiles < 0 || maxFiles > 100)
            return fail(L"日志路径或轮转数量无效。");
        std::wstring result;
        size_t totalBytes = 0;
        bool found = false;
        for (int i = maxFiles; i >= 0; --i) {
            std::wstring candidate = canonical + (i ? L"." + std::to_wstring(i) : L"");
            winutil::ScopedHandle file(CreateFileW(
                candidate.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!file.valid()) {
                DWORD code = GetLastError();
                if (code == ERROR_FILE_NOT_FOUND)
                    continue;
                return fail(L"无法打开日志文件。", code);
            }
            found = true;
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(file.get(), &info))
                return fail(L"无法检查日志文件。", GetLastError());
            if (GetFileType(file.get()) != FILE_TYPE_DISK ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                info.nNumberOfLinks != 1)
                return fail(L"日志路径不是普通文件。");
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.get(), &size))
                return fail(L"无法读取日志文件大小。", GetLastError());
            if (size.QuadPart < 0 || size.QuadPart > 50 * 1024 * 1024 ||
                totalBytes + static_cast<size_t>(size.QuadPart) > 50 * 1024 * 1024)
                return fail(L"日志总量超过 50 MiB。");
            totalBytes += static_cast<size_t>(size.QuadPart);
            std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
            if (!bytes.empty()) {
                DWORD count = 0;
                if (!ReadFile(file.get(), &bytes[0], static_cast<DWORD>(bytes.size()), &count, nullptr))
                    return fail(L"日志读取失败。", GetLastError());
                if (count != bytes.size())
                    return fail(L"日志读取不完整。", ERROR_HANDLE_EOF);
            }
            std::wstring wide;
            if (!TryUtf8ToWide(bytes, wide))
                return fail(L"日志内容包含无效 UTF-8。");
            result += wide;
        }
        if (!found)
            return fail(L"日志文件尚不存在。");
        out.swap(result);
        if (error)
            error->clear();
        return true;
    } catch (...) {
        if (error)
            *error = L"日志读取发生异常或内存不足。";
        return false;
    }
}
std::wstring Logger::ExportAll(const std::wstring& path) {
    std::wstring out;
    if (!TryExportAll(path, out))
        OutputDebugStringW(L"[logger] export failed; use TryExportAll for diagnostics");
    return out;
}
Logger& Log() {
    static Logger logger;
    return logger;
}
} // namespace core
