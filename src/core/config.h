// Strict JSON configuration with missing-field migration and cooperative file locking.
#pragma once
#include <mutex>
#include <string>

namespace core {
struct Config {
    std::wstring gameRoot;
    bool autoOn = true;
    int theme = 0;       // 0..1
    int closeAction = 0; // 0..2
    bool autostart = false;
    int syncMinutes = 5;    // 1..1440
    std::wstring cacheRoot; // Empty until the user explicitly selects a cache directory.
    std::wstring lastSync = L"--";
    int winX = -1;
    int winY = -1;
    int winW = 0; // 0..32767; 0 means unset.
    int winH = 0;

    // Missing known fields migrate to defaults; existing invalid fields fail closed.
    // Unknown fields survive Parse -> Save/UpdateAtomic. Failure leaves out unchanged.
    static bool Parse(const std::string& raw, Config& out, std::wstring* error = nullptr);
    static bool IsContentSafe(const std::string& raw);
    static bool LoadSafe(const std::wstring& path, Config& out);
    static Config Load(const std::wstring& path);
    // V3 Save to an existing file: Load/Parse baselines enable three-way field merging.
    // Unedited fields/unknown data keep the latest disk values; divergent edits to the
    // same field fail with LastError and leave file bytes intact. Reload/retry conflicts.
    // const Save keeps its original baseline; repeated equal-value edits are idempotent.
    // Fresh Config (no baseline) explicitly replaces ALL known fields: single-writer only,
    // never use default Config as recovery from a failed Load. New files initialize fully;
    // missing-target creation is allowed, with no CAS guarantee against a prior deletion.
    // This is not a transaction across Load/Save; production mutations use UpdateAtomic.
    bool Save(const std::wstring& path) const;
    static bool UpdateAtomic(const std::wstring& path, bool (*mutate)(Config&, void*), void* userData);
    // Compatibility aliases: now acquire locks too. Do not call within mutate.
    static Config LoadUnlocked(const std::wstring& path);
    bool SaveUnlocked(const std::wstring& path) const;
    // Latest calling-thread friendly error. False + empty error in UpdateAtomic = no change.
    static std::wstring LastError();
    // Resolve LocalAppData without creation or implicit shared-directory fallback.
    static std::wstring UserDataRoot();
    // Explicit first-run import; never overwrite destination or delete source.
    // Destination directory must exist. Missing source is a successful no-op.
    static bool ImportLegacyIfMissing(const std::wstring& legacyPath, const std::wstring& userPath);
    static Config MigrateFromLegacy(const std::wstring& legacyGameRootTxt);

  private:
    std::string m_preservedJson;
    std::string Serialize() const;
    static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }
};
} // namespace core
