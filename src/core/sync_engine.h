// Direct native copies between the Game directory and the managed physical cache (UTF-8).
#pragma once
#include "root_store.h" // Compatibility types only; the engine never reads or writes root-store metadata.
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace core {

struct SyncOutcome {
    // ran distinguishes an attempted operation from an offline, non-forced skip.
    bool ran = false;
    bool cancelled = false;
    std::wstring summary;

    // A completely executed operation succeeds, including a complete empty no-op.
    bool success = false;
    bool partial = false; // Observable copies/deletes or transferred bytes before failure/cancellation.
    int errors = 0;
    int copied = 0;  // Successful native Game->cache / cache->Game copies.
    int deleted = 0; // Successful native mirror deletions.
    int skipped = 0; // Unexecuted planned file operations after cancellation.
    bool verifiedNoop = false; // Legacy field: successful empty no-op; no content verification is performed.
    std::vector<std::wstring> errorDetails;
    std::vector<std::wstring> warnings;
    bool sourceNonempty = false; // A complete source listing contained at least one managed file.
};

class SyncEngine {
  public:
    std::wstring gameRoot;
    std::wstring cacheRoot;
    std::wstring installDir; // Compatibility only; unused by the direct engine.
    std::wstring managementRoot; // Compatibility only; no owner/state/history is accessed.
    rootstore::Lease* cacheAuthority = nullptr; // Compatibility only; never acquired or inspected.
    bool PrepareManagement(std::wstring& error);
    bool autoOn = true;

    // Successful file operations / planned work, plus one copy-phase completion marker.
    // A failed file does not advance done. Callbacks must not throw.
    using ProgressFn = std::function<void(int, int, const std::wstring&)>;
    ProgressFn onProgress;
    std::atomic<bool>* cancelFlag = nullptr;
    std::function<bool()> shouldCancel; // Service stop / lobby-online gate, also checked during CopyFileExW.
    int progressDone = 0;
    int progressTotal = 0;
    std::wstring progressCurrent;

    // copyOnly is manual backup: copy every listed Game file without deleting cache files.
    // Otherwise mirror the entire managed set, including absent groups and a completely empty Game.
    // fullMirror remains for source compatibility; direct mirrors always cover the complete whitelist.
    SyncOutcome RunOnce(bool online, bool force = false, bool copyOnly = false, bool fullMirror = false);
    int RestoreFromCache();         // Compatibility: successful native cache->Game copy count.
    SyncOutcome RestoreDetailed(); // Copies the current physical cache; never inventories Game.
    bool ReconcileCacheAbsences(const std::vector<std::wstring>& reintroduced, std::wstring& error);
    const SyncOutcome& LastOutcome() const {
        return lastOutcome_;
    }

    void ReportProgress(int done, int total, const std::wstring& current);
    bool Cancelled() const {
        return (cancelFlag && cancelFlag->load()) || (shouldCancel && shouldCancel());
    }

  private:
    SyncOutcome lastOutcome_;
    bool ValidateRoots(SyncOutcome& out, bool requireGame);
    SyncOutcome Finish(SyncOutcome out, const std::wstring& operation);
};

} // namespace core
