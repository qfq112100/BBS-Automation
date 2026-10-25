// Direct Win32 synchronization of the current managed physical files; no retained versions.
#include "sync_engine.h"
#include "file_ops.h"
#include "logger.h"
#include <algorithm>
#include <climits>
#include <map>
#include <set>
#include <windows.h>

namespace core {
namespace {
constexpr DWORD kCopyOverwrite = 0;
constexpr int kCompletionSteps = 1;
constexpr size_t kDiagnosticBatchSize = 8;
constexpr DWORD kSystemMessageChars = 1024;
using Less = pathpolicy::CaseInsensitiveLess;
using ParentResults = std::map<std::wstring, DWORD, Less>;

// Logging must not change a native operation's error code or outcome.
template <typename Build> void Diagnostic(LogLevel level, Build&& build) noexcept {
    const DWORD saved = GetLastError();
    try {
        core::Log().Log(level, L"SyncEngine", build());
    } catch (...) {
    }
    SetLastError(saved);
}

struct TaskDiagnostics;
thread_local TaskDiagnostics* taskDiagnostics = nullptr;

// Preserve structured task diagnostics without owner/state or per-file success logging.
struct TaskDiagnostics {
    const wchar_t* mode;
    const wchar_t* phase = L"preflight";
    const ULONGLONG started = GetTickCount64();
    TaskDiagnostics* previous = taskDiagnostics;
    std::map<std::wstring, size_t> details;

    explicit TaskDiagnostics(const wchar_t* taskMode) : mode(taskMode) {
        taskDiagnostics = this;
    }
    ~TaskDiagnostics() {
        taskDiagnostics = previous;
    }
    void Note(const std::wstring& text) noexcept {
        const DWORD saved = GetLastError();
        try {
            ++details[L"phase=" + std::wstring(phase) + L" detail=" + text];
        } catch (...) {
        }
        SetLastError(saved);
    }
    void Flush() const noexcept {
        const DWORD saved = GetLastError();
        try {
            // Each distinct error is emitted once; batching stays O(n log n), not O(n^2).
            std::wstring batch;
            size_t count = 0;
            auto emit = [&] {
                if (count) {
                    Diagnostic(LogLevel::Error, [&] {
                        return L"event=diagnostic_errors code=operation_failed direct=1 mode=" +
                               std::wstring(mode) + L" distinct=" + std::to_wstring(count) + batch;
                    });
                    batch.clear();
                    count = 0;
                }
            };
            for (const auto& item : details) {
                batch += L" [" + item.first + L" occurrences=" + std::to_wstring(item.second) + L"]";
                if (++count == kDiagnosticBatchSize)
                    emit();
            }
            emit();
        } catch (...) {
        }
        SetLastError(saved);
    }
};

// Select the phase attached to subsequent diagnostic errors.
void Phase(const wchar_t* phase) {
    if (taskDiagnostics)
        taskDiagnostics->phase = phase;
}

// Format the captured native error; never replace it with a logger/callback's LastError.
std::wstring SystemError(DWORD code) {
    wchar_t text[kSystemMessageChars]{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, code, 0, text, kSystemMessageChars, nullptr);
    std::wstring result(text, length);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n'))
        result.pop_back();
    return result;
}

// Keep expected operation, actual native result and location in both summary and diagnostics.
void Error(SyncOutcome& out, const std::wstring& operation, const std::wstring& location,
           DWORD code, const std::wstring& detail = L"") {
    ++out.errors;
    out.errorDetails.push_back(L"expected=" + operation + L" success; actual=Win32 " +
                              std::to_wstring(code) + L" " + SystemError(code) +
                              L"; location=" + location + (detail.empty() ? L"" : L"; " + detail));
    if (taskDiagnostics)
        taskDiagnostics->Note(out.errorDetails.back());
}

// Cancellation is sticky in the outcome even if a caller later clears its flag.
bool CheckCancel(SyncEngine& engine, SyncOutcome& out) {
    if (engine.Cancelled())
        out.cancelled = true;
    return out.cancelled;
}

// Relative paths come exclusively from the managed whitelist enumerator.
std::wstring Join(const std::wstring& root, const std::wstring& relative) {
    return root + L"\\" + relative;
}

// Capture exactly once per source/destination. An incomplete list never authorizes a plan.
bool ListManaged(SyncEngine& engine, const std::wstring& root, fileops::ListResult& result,
                 SyncOutcome& out) {
    result = fileops::ListBusinessFilesChecked(root, [&engine] { return engine.Cancelled(); });
    const DWORD code = GetLastError();
    if (result.cancelled)
        out.cancelled = true;
    for (const auto& error : result.errors) {
        if (result.cancelled && error.code == ERROR_CANCELLED)
            continue;
        Error(out, error.operation, error.path, error.code, error.message);
    }
    if (CheckCancel(engine, out))
        return false;
    if (!result.complete) {
        if (result.errors.empty())
            Error(out, L"complete managed enumeration", root,
                  code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : code);
        return false;
    }
    return result.errors.empty();
}

// Size limits are checked before any integer progress conversion or business mutation.
bool PlanProgress(SyncEngine& engine, size_t copies, size_t deletes, SyncOutcome& out) {
    const size_t maximum = static_cast<size_t>(INT_MAX - kCompletionSteps);
    if (copies > maximum || deletes > maximum - copies) {
        Error(out, L"representable progress total", L"copy/delete plan", ERROR_ARITHMETIC_OVERFLOW);
        return false;
    }
    engine.ReportProgress(0, static_cast<int>(copies + deletes) + kCompletionSteps, L"");
    return !CheckCancel(engine, out);
}

// Cache both successful and failed parent creation results: one EnsureDir call per unique parent.
bool EnsureParent(const std::wstring& destination, ParentResults& parents, SyncOutcome& out) {
    const auto parent = destination.substr(0, destination.find_last_of(L'\\'));
    auto found = parents.find(parent);
    if (found == parents.end()) {
        const bool ensured = fileops::EnsureDir(parent);
        const DWORD code = ensured ? ERROR_SUCCESS : GetLastError();
        found = parents.emplace(parent, code).first;
    }
    if (found->second != ERROR_SUCCESS) {
        Error(out, L"EnsureDir destination parent", parent, found->second,
              L"destination=" + destination);
        return false;
    }
    return true;
}

// One callback context lives for the entire job and is reused for every native copy.
struct CancelContext {
    SyncEngine& engine;
    bool cancelled = false;
    bool transferred = false;
};

// Native cancellation keeps Windows CopyFileExW semantics; no rollback/staging is introduced.
DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER,
                            DWORD, DWORD, HANDLE, HANDLE, LPVOID data) {
    auto& context = *static_cast<CancelContext*>(data);
    context.transferred = context.transferred || transferred.QuadPart > 0;
    if (context.engine.Cancelled()) {
        context.cancelled = true;
        // Stop at the current partial destination; do not delete it as cancel cleanup.
        return PROGRESS_STOP;
    }
    return PROGRESS_CONTINUE;
}

// Always overwrite directly, even when source and destination bytes happen to be identical.
void CopyDirect(SyncEngine& engine, const std::wstring& source, const std::wstring& destination,
                const std::wstring& label, ParentResults& parents, CancelContext& cancel,
                SyncOutcome& out) {
    engine.ReportProgress(engine.progressDone, engine.progressTotal, label);
    if (CheckCancel(engine, out) || !EnsureParent(destination, parents, out))
        return;
    if (CheckCancel(engine, out))
        return;
    cancel.cancelled = false;
    cancel.transferred = false;
    const auto nativeSource = pathpolicy::LongPath(source);
    const auto nativeDestination = pathpolicy::LongPath(destination);
    const BOOL copied = CopyFileExW(nativeSource.c_str(), nativeDestination.c_str(), CopyProgress,
                                    &cancel, nullptr, kCopyOverwrite);
    const DWORD code = copied ? ERROR_SUCCESS : GetLastError();
    if (!copied) {
        out.partial = out.partial || cancel.transferred;
        if (cancel.cancelled)
            out.cancelled = true;
        if (!cancel.cancelled || code != ERROR_REQUEST_ABORTED)
            Error(out, L"CopyFileExW overwrite", source + L" -> " + destination, code);
        CheckCancel(engine, out);
        return;
    }
    ++out.copied;
    engine.ReportProgress(engine.progressDone + 1, engine.progressTotal, label);
    CheckCancel(engine, out);
}

// The legacy 'verified' label means native copy phase complete, NOT hash/content verification.
void CopyCompletion(SyncEngine& engine, const wchar_t* label, SyncOutcome& out) {
    if (!out.errors && !CheckCancel(engine, out))
        engine.ReportProgress(engine.progressDone + kCompletionSteps, engine.progressTotal, label);
    CheckCancel(engine, out);
}

// Delete only a relative path from the one complete managed destination listing.
void DeleteDirect(SyncEngine& engine, const std::wstring& relative, SyncOutcome& out) {
    const auto label = L"delete/" + relative;
    engine.ReportProgress(engine.progressDone, engine.progressTotal, label);
    if (CheckCancel(engine, out))
        return;
    const auto destination = Join(engine.cacheRoot, relative);
    if (!DeleteFileW(pathpolicy::LongPath(destination).c_str())) {
        const DWORD code = GetLastError();
        Error(out, L"DeleteFileW", destination, code);
    } else {
        ++out.deleted;
        engine.ReportProgress(engine.progressDone + 1, engine.progressTotal, label);
    }
    CheckCancel(engine, out);
}
} // namespace

// Publish actual successful work; do not manufacture 100% after a failed operation.
void SyncEngine::ReportProgress(int done, int total, const std::wstring& current) {
    progressTotal = (std::max)(0, total);
    progressDone = (std::max)(0, (std::min)(done, progressTotal));
    progressCurrent = current;
    if (onProgress)
        onProgress(progressDone, progressTotal, current);
}

// Validate configured scope once per job; there is no identity claim or ownership state.
bool SyncEngine::ValidateRoots(SyncOutcome& out, bool requireGame) {
    Phase(L"root_preflight");
    std::wstring normalized, error;
    if (!pathpolicy::NormalizeRoot(cacheRoot, normalized, error)) {
        const DWORD code = GetLastError();
        Error(out, L"normalized cache root", cacheRoot, code, error);
        return false;
    }
    cacheRoot = normalized;
    if (requireGame || !gameRoot.empty()) {
        if (!pathpolicy::NormalizeRoot(gameRoot, normalized, error)) {
            const DWORD code = GetLastError();
            Error(out, L"normalized Game root", gameRoot, code, error);
            return false;
        }
        gameRoot = normalized;
        if (!pathpolicy::AreDisjoint(gameRoot, cacheRoot, error)) {
            const DWORD code = GetLastError();
            Error(out, L"disjoint Game/cache roots", gameRoot + L" / " + cacheRoot, code, error);
            return false;
        }
    }
    Diagnostic(LogLevel::Info, [] {
        return L"event=safety_preflight code=roots_disjoint_and_normalized direct=1 ownership=unused";
    });
    return !CheckCancel(*this, out);
}

// Compatibility preparation now only validates roots and creates the cache directory itself.
bool SyncEngine::PrepareManagement(std::wstring& error) {
    SyncOutcome out;
    TaskDiagnostics diagnostics(L"management_prepare");
    error.clear();
    if (ValidateRoots(out, false) && !fileops::EnsureDir(cacheRoot)) {
        const DWORD code = GetLastError();
        Error(out, L"EnsureDir cache root", cacheRoot, code);
    }
    if (out.errors || out.cancelled) {
        error = out.errorDetails.empty() ? L"缓存目录准备已取消" : out.errorDetails.front();
        diagnostics.Flush();
        return false;
    }
    return true;
}

// Report native execution, including a successful complete empty no-op and a distinct offline skip.
SyncOutcome SyncEngine::Finish(SyncOutcome out, const std::wstring& operation) {
    if (out.ran)
        CheckCancel(*this, out);
    const bool changed = out.copied > 0 || out.deleted > 0 || out.partial;
    out.success = out.ran && !out.cancelled && out.errors == 0;
    out.partial = !out.success && changed;
    out.verifiedNoop = out.success && out.copied == 0 && out.deleted == 0;
    out.summary = operation + (out.cancelled ? L"已取消"
                              : out.errors ? (out.partial ? L"部分完成，存在错误" : L"失败")
                              : out.ran    ? L"完成（原生文件操作）"
                                           : L"已跳过（离线，未执行）");
    out.summary += L"；复制=" + std::to_wstring(out.copied) + L"，删除=" +
                   std::to_wstring(out.deleted) + L"，错误=" + std::to_wstring(out.errors);
    if (!out.errorDetails.empty())
        out.summary += L"；" + out.errorDetails.front();
    if (out.success)
        ReportProgress(progressDone, progressTotal, L"");
    lastOutcome_ = out;
    if (taskDiagnostics)
        taskDiagnostics->Flush();
    Diagnostic(out.errors ? LogLevel::Error : out.cancelled ? LogLevel::Warn : LogLevel::Info, [&] {
        const auto* task = taskDiagnostics;
        return std::wstring(L"event=task_result code=") +
               (out.cancelled ? L"cancelled"
                : out.errors  ? L"failed"
                : out.success ? L"complete"
                              : L"no_effect") +
               L" operation=" + operation + L" mode=" + (task ? task->mode : L"unspecified") +
               L" direct=1 verification=none reason=" +
               (out.ran ? (out.success ? L"native_operations_complete" : (task ? task->phase : L"io"))
                        : L"offline_no_mirror") +
               L" ran=" + (out.ran ? L"1" : L"0") + L" sourceNonempty=" +
               (out.sourceNonempty ? L"1" : L"0") + L" copied=" + std::to_wstring(out.copied) +
               L" deleted=" + std::to_wstring(out.deleted) + L" skipped=" + std::to_wstring(out.skipped) +
               L" errors=" + std::to_wstring(out.errors) + L" success=" + (out.success ? L"1" : L"0") +
               L" partial=" + (out.partial ? L"1" : L"0") + L" cancelled=" + (out.cancelled ? L"1" : L"0") +
               L" elapsed_ms=" + std::to_wstring(task ? GetTickCount64() - task->started : 0);
    });
    return out;
}

// Restore current physical cache files with one whitelist listing and no Game inventory or rescan.
SyncOutcome SyncEngine::RestoreDetailed() {
    SyncOutcome out;
    out.ran = true;
    TaskDiagnostics diagnostics(L"restore");
    Diagnostic(LogLevel::Info, [] { return L"event=task_begin code=requested mode=restore direct=1"; });
    ReportProgress(0, 0, L"");
    if (!ValidateRoots(out, true))
        return Finish(out, L"恢复");
    Phase(L"restore_inventory");
    fileops::ListResult source;
    if (!ListManaged(*this, cacheRoot, source, out))
        return Finish(out, L"恢复");
    out.sourceNonempty = !source.files.empty();
    Diagnostic(LogLevel::Info, [&] {
        return L"event=restore_inventory code=complete direct=1 source_files=" +
               std::to_wstring(source.files.size()) + L" game_inventory=disabled";
    });
    if (!PlanProgress(*this, source.files.size(), 0, out))
        return Finish(out, L"恢复");
    Phase(L"restore_copy");
    ParentResults parents;
    CancelContext cancel{*this};
    size_t attempted = 0;
    for (const auto& relative : source.files) {
        if (CheckCancel(*this, out))
            break;
        CopyDirect(*this, Join(cacheRoot, relative), Join(gameRoot, relative), L"restore/" + relative,
                   parents, cancel, out);
        ++attempted;
        if (out.cancelled)
            break;
    }
    if (out.cancelled)
        out.skipped = static_cast<int>(source.files.size() - attempted);
    CopyCompletion(*this, L"restore/verified", out);
    Diagnostic(LogLevel::Info, [&] {
        return L"event=restore_commit code=native_copy_phase_finished direct=1 verification=none copied=" +
               std::to_wstring(out.copied) + L" errors=" + std::to_wstring(out.errors);
    });
    return Finish(out, L"恢复");
}

// Deprecated metadata reconciliation is deliberately a no-op; physical files alone define the cache.
bool SyncEngine::ReconcileCacheAbsences(const std::vector<std::wstring>& reintroduced,
                                        std::wstring& error) {
    (void)reintroduced;
    error.clear();
    return true;
}

// Retain the historical count-only API without adding another restore pass.
int SyncEngine::RestoreFromCache() {
    return RestoreDetailed().copied;
}

// A complete initial listing defines a direct mirror; copy errors do not suppress remaining deletes.
SyncOutcome SyncEngine::RunOnce(bool online, bool force, bool copyOnly, bool fullMirror) {
    (void)fullMirror;
    SyncOutcome out;
    TaskDiagnostics diagnostics(!online && !force ? L"offline_no_mirror"
                                : copyOnly        ? L"copy_only"
                                                  : L"full_mirror");
    Diagnostic(LogLevel::Info, [&] {
        return std::wstring(L"event=task_begin code=requested mode=") + diagnostics.mode +
               L" direct=1 online=" + (online ? L"1" : L"0") + L" force=" + (force ? L"1" : L"0") +
               L" copy_only=" + (copyOnly ? L"1" : L"0");
    });
    ReportProgress(0, 0, L"");
    if (!online && !force) {
        Diagnostic(LogLevel::Info, [] {
            return L"event=mirror_did_not_run code=offline_no_mirror direct=1";
        });
        return Finish(out, L"同步");
    }
    out.ran = true;
    const std::wstring operation = copyOnly ? L"备份" : L"同步";
    if (!ValidateRoots(out, true))
        return Finish(out, operation);
    Phase(L"mirror_inventory");
    fileops::ListResult source, destination;
    if (!ListManaged(*this, gameRoot, source, out))
        return Finish(out, operation);
    out.sourceNonempty = !source.files.empty();
    ParentResults parents;
    const bool ensured = fileops::EnsureDir(cacheRoot);
    const DWORD ensureCode = ensured ? ERROR_SUCCESS : GetLastError();
    parents.emplace(cacheRoot, ensureCode);
    if (!ensured) {
        Error(out, L"EnsureDir cache root", cacheRoot, ensureCode);
        return Finish(out, operation);
    }
    if (!copyOnly && !ListManaged(*this, cacheRoot, destination, out))
        return Finish(out, operation);
    std::vector<std::wstring> deletions;
    if (!copyOnly) {
        // Case-insensitive membership preserves Windows path semantics with O(n log n) planning.
        const std::set<std::wstring, Less> sourcePaths(source.files.begin(), source.files.end());
        for (const auto& relative : destination.files)
            if (!sourcePaths.count(relative))
                deletions.push_back(relative);
    }
    Diagnostic(LogLevel::Info, [&] {
        return L"event=mirror_plan code=complete_source direct=1 copies=" +
               std::to_wstring(source.files.size()) + L" deletes=" + std::to_wstring(deletions.size()) +
               L" sourceNonempty=" + (out.sourceNonempty ? L"1" : L"0") + L" protected_groups=0";
    });
    if (!PlanProgress(*this, source.files.size(), deletions.size(), out))
        return Finish(out, operation);
    Phase(L"mirror_copy");
    CancelContext cancel{*this};
    size_t attemptedCopies = 0;
    for (const auto& relative : source.files) {
        if (CheckCancel(*this, out))
            break;
        CopyDirect(*this, Join(gameRoot, relative), Join(cacheRoot, relative), L"mirror/" + relative,
                   parents, cancel, out);
        ++attemptedCopies;
        if (out.cancelled)
            break;
    }
    CopyCompletion(*this, L"mirror/verified", out);
    Phase(L"managed_deletions");
    size_t attemptedDeletes = 0;
    for (const auto& relative : deletions) {
        if (CheckCancel(*this, out))
            break;
        DeleteDirect(*this, relative, out);
        ++attemptedDeletes;
        if (out.cancelled)
            break;
    }
    if (out.cancelled)
        out.skipped = static_cast<int>(source.files.size() - attemptedCopies +
                                       deletions.size() - attemptedDeletes);
    Diagnostic(LogLevel::Info, [&] {
        return L"event=mirror_commit code=native_operations_finished direct=1 verification=none copied=" +
               std::to_wstring(out.copied) + L" deleted=" + std::to_wstring(out.deleted) +
               L" errors=" + std::to_wstring(out.errors);
    });
    return Finish(out, operation);
}
} // namespace core
