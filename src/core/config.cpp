#include "config.h"
#include "../common/limits_shared.h"
#include "../common/winutil.h"
#include "utf.h"
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>
#define JSON_DIAGNOSTICS 0
#include "../../third_party/json/json.hpp"
using json = nlohmann::json;

namespace core {
namespace {
thread_local std::wstring lastError;
constexpr size_t kMaxConfig = 1024 * 1024;
bool Fail(const std::wstring& message, DWORD code = ERROR_SUCCESS) {
    lastError = message;
    if (code)
        lastError += L" (Windows error " + std::to_wstring(code) + L")";
    return false;
}
bool CleanString(const std::wstring& value, bool path) {
    if (value.size() > (path ? 32700u : 65536u))
        return false;
    for (wchar_t ch : value)
        if (ch == 0 || (path && ch < 32))
            return false;
    std::string unused;
    return TryWideToUtf8(value, unused);
}
bool Canonical(const std::wstring& path, std::wstring& out) {
    if (path.empty() || !CleanString(path, true))
        return Fail(L"配置路径含无效字符或过长。");
    std::wstring normalized = path;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (normalized.compare(0, 4, L"\\\\.\\") == 0)
        return Fail(L"不支持设备路径。");
    DWORD n = GetFullPathNameW(normalized.c_str(), 0, nullptr, nullptr);
    if (!n || n > 32700)
        return Fail(L"无法解析配置路径。", GetLastError());
    std::vector<wchar_t> buffer(n);
    DWORD copied = GetFullPathNameW(normalized.c_str(), n, buffer.data(), nullptr);
    if (!copied || copied >= n)
        return Fail(L"无法解析配置路径。", GetLastError());
    out = winutil::LongPath(std::wstring(buffer.data(), copied));
    return true;
}
// A durable sidecar byte-range lock works across processes and Windows sessions.
// Do not unlink it: unlinking a lock file can create two independent lock domains.
class ConfigLock {
    winutil::ScopedHandle handle;
    OVERLAPPED overlapped{};
    bool locked = false;

  public:
    bool Acquire(const std::wstring& path) {
        handle.reset(CreateFileW((path + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.valid())
            return Fail(L"无法锁定配置；请确认配置目录存在且可写。", GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.get(), &info))
            return Fail(L"无法检查配置锁。", GetLastError());
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.nNumberOfLinks != 1)
            return Fail(L"配置锁不是普通文件。");
        const ULONGLONG start = GetTickCount64();
        while (!LockFileEx(handle.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                           &overlapped)) {
            const DWORD code = GetLastError();
            if (code != ERROR_LOCK_VIOLATION && code != ERROR_IO_PENDING)
                return Fail(L"配置锁定失败。", code);
            if (GetTickCount64() - start >= common::kConfigReadTimeoutMs)
                return Fail(L"其他进程正在更新配置，请稍后重试。", ERROR_TIMEOUT);
            Sleep(10);
        }
        locked = true;
        return true;
    }
    ~ConfigLock() {
        if (locked)
            UnlockFileEx(handle.get(), 0, 1, 0, &overlapped);
    }
};
struct Stamp {
    bool exists = false;
    BY_HANDLE_FILE_INFORMATION info{};
};
bool Same(const Stamp& a, const Stamp& b) {
    return a.exists == b.exists &&
           (!a.exists ||
            (a.info.dwVolumeSerialNumber == b.info.dwVolumeSerialNumber &&
             a.info.nFileIndexHigh == b.info.nFileIndexHigh && a.info.nFileIndexLow == b.info.nFileIndexLow &&
             a.info.nFileSizeHigh == b.info.nFileSizeHigh && a.info.nFileSizeLow == b.info.nFileSizeLow &&
             CompareFileTime(&a.info.ftLastWriteTime, &b.info.ftLastWriteTime) == 0));
}
bool OpenRead(const std::wstring& path, winutil::ScopedHandle& handle, Stamp& stamp) {
    handle.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            stamp = Stamp{};
            return true;
        }
        return Fail(L"无法读取配置文件；不会使用默认值覆盖。", code);
    }
    if (!GetFileInformationByHandle(handle.get(), &stamp.info))
        return Fail(L"无法检查配置文件。", GetLastError());
    if ((stamp.info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        stamp.info.nNumberOfLinks != 1)
        return Fail(L"配置路径必须是普通文件，不能是目录或重解析点。");
    stamp.exists = true;
    return true;
}
bool ReadRaw(const std::wstring& path, std::string& raw, Stamp& stamp) {
    winutil::ScopedHandle handle;
    if (!OpenRead(path, handle, stamp))
        return false;
    if (!stamp.exists) {
        raw.clear();
        return true;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size))
        return Fail(L"无法读取配置大小。", GetLastError());
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaxConfig))
        return Fail(L"配置文件为空或超过 1 MiB；不会覆盖。");
    raw.resize(static_cast<size_t>(size.QuadPart));
    DWORD count = 0;
    if (!ReadFile(handle.get(), &raw[0], static_cast<DWORD>(raw.size()), &count, nullptr))
        return Fail(L"配置文件读取失败；不会覆盖。", GetLastError());
    if (count != raw.size())
        return Fail(L"配置文件读取不完整；不会覆盖。", ERROR_HANDLE_EOF);
    return true;
}
struct TempFile {
    std::wstring path;
    winutil::ScopedHandle handle;
    bool committed = false;
    ~TempFile() {
        handle.reset(); // Always close BEFORE deleting, including exceptions.
        if (!committed && !path.empty() && !DeleteFileW(path.c_str())) {
            DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) {
                try {
                    lastError += L" 临时文件清理失败 (Windows error " + std::to_wstring(code) + L")";
                } catch (...) {
                    OutputDebugStringW(L"[config] temporary file cleanup failed");
                }
            }
        }
    }
};
bool WriteAtomic(const std::wstring& path, const std::string& bytes, const Stamp& expected) {
    if (bytes.empty() || bytes.size() > kMaxConfig)
        return Fail(L"配置序列化为空或超过 1 MiB。");
    static std::atomic<unsigned long long> sequence{0};
    TempFile tmp;
    for (int attempt = 0; attempt < 32; ++attempt) {
        tmp.path = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                   std::to_wstring(GetCurrentThreadId()) + L"." + std::to_wstring(sequence.fetch_add(1));
        tmp.handle.reset(CreateFileW(tmp.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
        if (tmp.handle.valid())
            break;
        DWORD code = GetLastError();
        // Never delete another writer's preexisting temporary file.
        tmp.path.clear();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
            return Fail(L"无法创建配置临时文件。", code);
    }
    if (!tmp.handle.valid())
        return Fail(L"无法创建唯一配置临时文件。");
    DWORD count = 0;
    if (!WriteFile(tmp.handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
        return Fail(L"配置写入失败，原配置未改动。", GetLastError());
    if (count != bytes.size())
        return Fail(L"配置写入不完整，原配置未改动。", ERROR_WRITE_FAULT);
    if (!FlushFileBuffers(tmp.handle.get()))
        return Fail(L"配置刷盘失败，原配置未改动。", GetLastError());
    HANDLE raw = tmp.handle.release();
    if (!CloseHandle(raw))
        return Fail(L"配置临时文件关闭失败，原配置未改动。", GetLastError());
    // Metadata-only optimistic conflict check; never read or parse configuration twice.
    Stamp current;
    {
        winutil::ScopedHandle existing;
        if (!OpenRead(path, existing, current))
            return false;
    }
    if (!Same(expected, current))
        return Fail(L"配置已被外部程序修改，请重新加载后重试。", ERROR_REVISION_MISMATCH);
    // Cooperative lock covers the check and replace. Noncooperating writers are not a security boundary.
    if (!MoveFileExW(tmp.path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return Fail(L"无法替换配置文件，原配置未改动。", GetLastError());
    tmp.committed = true;
    return true;
}
} // namespace

std::wstring Config::LastError() {
    return lastError;
}

bool Config::Parse(const std::string& raw, Config& out, std::wstring* error) {
    lastError.clear();
    try {
        auto reject = [&](const std::wstring& message) {
            Fail(message);
            if (error)
                *error = lastError;
            return false;
        };
        if (raw.empty() || raw.size() > kMaxConfig)
            return reject(L"配置为空或超过 1 MiB。");
        // Reject duplicate object keys rather than silently discarding user fields.
        std::vector<std::set<std::string>> objects;
        bool duplicate = false;
        auto callback = [&](int depth, json::parse_event_t event, json& parsed) {
            if (depth > 64)
                throw std::runtime_error("configuration nesting exceeds 64");
            if (event == json::parse_event_t::object_start)
                objects.emplace_back();
            else if (event == json::parse_event_t::key && !objects.empty()) {
                if (!objects.back().insert(parsed.get<std::string>()).second)
                    duplicate = true;
            } else if (event == json::parse_event_t::object_end && !objects.empty())
                objects.pop_back();
            return true;
        };
        json j = json::parse(raw, callback);
        if (duplicate || !j.is_object())
            return reject(L"配置必须是对象且不能包含重复字段。");
        Config candidate;
        const char* checkingField = "";
        auto integer = [&](const char* key, int& value, int low, int high) {
            checkingField = key;
            auto it = j.find(key);
            if (it == j.end())
                return true;
            if (!it->is_number_integer())
                return false;
            if (it->is_number_unsigned()) {
                auto n = it->get<json::number_unsigned_t>();
                if (n > static_cast<json::number_unsigned_t>(high))
                    return false;
                if (low > 0 && n < static_cast<json::number_unsigned_t>(low))
                    return false;
                value = static_cast<int>(n);
            } else {
                auto n = it->get<json::number_integer_t>();
                if (n < low || n > high)
                    return false;
                value = static_cast<int>(n);
            }
            return true;
        };
        auto boolean = [&](const char* key, bool& value) {
            checkingField = key;
            auto it = j.find(key);
            if (it == j.end())
                return true;
            if (!it->is_boolean())
                return false;
            value = it->get<bool>();
            return true;
        };
        auto text = [&](const char* key, std::wstring& value, bool path) {
            checkingField = key;
            auto it = j.find(key);
            if (it == j.end())
                return true;
            if (!it->is_string())
                return false;
            std::wstring converted;
            if (!TryUtf8ToWide(it->get_ref<const std::string&>(), converted) || !CleanString(converted, path))
                return false;
            value.swap(converted);
            return true;
        };
        if (!integer("theme", candidate.theme, 0, 1) ||
            !integer("close_action", candidate.closeAction, 0, 2) ||
            !integer("sync_minutes", candidate.syncMinutes, 1, 1440) ||
            !integer("win_x", candidate.winX, (std::numeric_limits<int>::min)(),
                     (std::numeric_limits<int>::max)()) ||
            !integer("win_y", candidate.winY, (std::numeric_limits<int>::min)(),
                     (std::numeric_limits<int>::max)()) ||
            !integer("win_w", candidate.winW, 0, 32767) || !integer("win_h", candidate.winH, 0, 32767) ||
            !boolean("auto_on", candidate.autoOn) || !boolean("autostart", candidate.autostart) ||
            !text("game_root", candidate.gameRoot, true) || !text("cache_root", candidate.cacheRoot, true) ||
            !text("last_sync", candidate.lastSync, false))
            return reject(L"配置字段 " + Utf8ToWide(checkingField) +
                          L" 的类型、整数范围、Unicode 或路径字符无效；原文件将保持不变。");
        candidate.m_preservedJson = j.dump();
        out = std::move(candidate);
        if (error)
            error->clear();
        return true;
    } catch (...) {
        Fail(L"配置 JSON/Unicode 无效、嵌套超过 64 层或内存不足；原文件将保持不变。");
        if (error)
            *error = lastError;
        return false;
    }
}
bool Config::IsContentSafe(const std::string& raw) {
    Config ignored;
    return Parse(raw, ignored);
}
std::string Config::Serialize() const {
    json j = m_preservedJson.empty() ? json::object() : json::parse(m_preservedJson);
    auto text = [&](const char* key, const std::wstring& value, bool path) {
        std::string converted;
        if (!CleanString(value, path) || !TryWideToUtf8(value, converted))
            throw std::runtime_error("invalid configuration string");
        j[key] = converted;
    };
    text("game_root", gameRoot, true);
    text("cache_root", cacheRoot, true);
    text("last_sync", lastSync, false);
    j["auto_on"] = autoOn;
    j["theme"] = theme;
    j["close_action"] = closeAction;
    j["autostart"] = autostart;
    j["sync_minutes"] = syncMinutes;
    j["win_x"] = winX;
    j["win_y"] = winY;
    j["win_w"] = winW;
    j["win_h"] = winH;
    std::string bytes = j.dump(2);
    Config checked;
    if (!Parse(bytes, checked))
        throw std::runtime_error("invalid configuration values");
    return bytes;
}
bool Config::LoadSafe(const std::wstring& path, Config& out) {
    std::lock_guard<std::mutex> guard(Mutex());
    lastError.clear();
    try {
        std::wstring canonical;
        if (!Canonical(path, canonical))
            return false;
        ConfigLock lock;
        if (!lock.Acquire(canonical))
            return false;
        std::string raw;
        Stamp stamp;
        if (!ReadRaw(canonical, raw, stamp))
            return false;
        Config candidate;
        if (stamp.exists && !Parse(raw, candidate))
            return false;
        out = std::move(candidate);
        return true;
    } catch (...) {
        return Fail(L"读取配置时发生错误；不会覆盖原文件。");
    }
}
Config Config::Load(const std::wstring& path) {
    Config config;
    LoadSafe(path, config);
    return config;
}
Config Config::LoadUnlocked(const std::wstring& path) {
    return Load(path);
}
bool Config::SaveUnlocked(const std::wstring& path) const {
    return Save(path);
}
bool Config::Save(const std::wstring& path) const {
    std::lock_guard<std::mutex> guard(Mutex());
    lastError.clear();
    try {
        std::wstring canonical;
        if (!Canonical(path, canonical))
            return false;
        ConfigLock lock;
        if (!lock.Acquire(canonical))
            return false;
        std::string raw;
        Stamp stamp;
        if (!ReadRaw(canonical, raw, stamp))
            return false;
        Config existing;
        if (stamp.exists && !Parse(raw, existing))
            return false;
        Config merged = *this;
        if (stamp.exists && !m_preservedJson.empty()) {
            // V3 compatibility: a loaded/parsed object carries its original value baseline.
            // Rebase only caller-edited fields onto the latest disk object under the lock.
            // This is optimistic field merging, not a transaction across Load and Save.
            Config baseline;
            if (!Parse(m_preservedJson, baseline))
                return false;
            merged = existing;
            auto merge = [&](auto member, const char* key) {
                const auto& wanted = this->*member;
                const auto& original = baseline.*member;
                const auto& latest = existing.*member;
                if (wanted == original)
                    return true; // Unedited field: retain the newest on-disk value.
                if (latest != original && latest != wanted)
                    return Fail(L"配置字段 " + Utf8ToWide(key) +
                                    L" 已被其他写入者修改，本次保存未提交；请重新加载后重试。",
                                ERROR_REVISION_MISMATCH);
                merged.*member = wanted; // Same-value concurrent updates are idempotent.
                return true;
            };
            if (!merge(&Config::gameRoot, "game_root") || !merge(&Config::autoOn, "auto_on") ||
                !merge(&Config::theme, "theme") || !merge(&Config::closeAction, "close_action") ||
                !merge(&Config::autostart, "autostart") || !merge(&Config::syncMinutes, "sync_minutes") ||
                !merge(&Config::cacheRoot, "cache_root") || !merge(&Config::lastSync, "last_sync") ||
                !merge(&Config::winX, "win_x") || !merge(&Config::winY, "win_y") ||
                !merge(&Config::winW, "win_w") || !merge(&Config::winH, "win_h"))
                return false;
        } else if (stamp.exists) {
            // A fresh Config has no baseline: explicitly replace all known fields.
            // Unknown fields still come from the latest disk object and are never dropped.
            merged.m_preservedJson = existing.m_preservedJson;
        }
        return WriteAtomic(canonical, merged.Serialize(), stamp);
    } catch (...) {
        if (lastError.empty())
            Fail(L"保存配置失败；字段无效或内存不足。");
        return false;
    }
}
bool Config::UpdateAtomic(const std::wstring& path, bool (*mutate)(Config&, void*), void* userData) {
    std::lock_guard<std::mutex> guard(Mutex());
    lastError.clear();
    try {
        if (!mutate)
            return Fail(L"配置更新回调不能为空。");
        std::wstring canonical;
        if (!Canonical(path, canonical))
            return false;
        ConfigLock lock;
        if (!lock.Acquire(canonical))
            return false;
        std::string raw;
        Stamp stamp;
        if (!ReadRaw(canonical, raw, stamp))
            return false;
        Config config;
        if (stamp.exists && !Parse(raw, config))
            return false;
        if (!mutate(config, userData)) {
            lastError.clear();
            return false;
        }
        return WriteAtomic(canonical, config.Serialize(), stamp);
    } catch (...) {
        if (lastError.empty())
            Fail(L"配置更新失败；字段无效或回调异常。");
        return false;
    }
}
std::wstring Config::UserDataRoot() {
    lastError.clear();
    PWSTR value = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &value);
    if (FAILED(hr) || !value) {
        if (value)
            CoTaskMemFree(value);
        Fail(L"无法定位当前用户的 LocalAppData。", static_cast<DWORD>(hr));
        return L"";
    }
    try {
        std::wstring root(value);
        CoTaskMemFree(value);
        value = nullptr;
        if (root.empty() || !CleanString(root, true)) {
            Fail(L"当前用户数据目录无效。");
            return L"";
        }
        return root + L"\\LOLBackupV3";
    } catch (...) {
        if (value)
            CoTaskMemFree(value);
        Fail(L"无法分配用户目录路径。");
        return L"";
    }
}
bool Config::ImportLegacyIfMissing(const std::wstring& legacyPath, const std::wstring& userPath) {
    std::lock_guard<std::mutex> guard(Mutex());
    lastError.clear();
    try {
        std::wstring source, target;
        if (!Canonical(legacyPath, source) || !Canonical(userPath, target))
            return false;
        if (_wcsicmp(source.c_str(), target.c_str()) == 0)
            return Fail(L"旧配置和新配置不能是同一文件。");
        // Only the destination is mutated. A deny-write read handle snapshots the source;
        // do not require permission to create a lock beside a read-only legacy file.
        ConfigLock targetLock;
        if (!targetLock.Acquire(target))
            return false;
        winutil::ScopedHandle existing;
        Stamp targetStamp;
        if (!OpenRead(target, existing, targetStamp))
            return false;
        if (targetStamp.exists)
            return true;
        DWORD attributes = GetFileAttributesW(source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                return true;
            return Fail(L"无法访问旧配置；不会修改目标。", code);
        }
        std::string raw;
        Stamp sourceStamp;
        if (!ReadRaw(source, raw, sourceStamp))
            return false;
        if (!sourceStamp.exists)
            return true;
        Config config;
        if (!Parse(raw, config))
            return false;
        return WriteAtomic(target, config.Serialize(), targetStamp);
    } catch (...) {
        if (lastError.empty())
            Fail(L"旧配置迁移失败；旧文件未改动。");
        return false;
    }
}
Config Config::MigrateFromLegacy(const std::wstring& legacyGameRootTxt) {
    lastError.clear();
    Config config;
    try {
        std::wstring path;
        if (!Canonical(legacyGameRootTxt, path))
            return config;
        std::string raw;
        Stamp stamp;
        if (!ReadRaw(path, raw, stamp) || !stamp.exists)
            return config;
        if (raw.compare(0, 3, "\xEF\xBB\xBF") == 0)
            raw.erase(0, 3);
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' '))
            raw.pop_back();
        if (!TryUtf8ToWide(raw, config.gameRoot) || !CleanString(config.gameRoot, true)) {
            config.gameRoot.clear();
            Fail(L"旧版游戏目录文本编码或字符无效。");
        }
    } catch (...) {
        Fail(L"无法读取旧版游戏目录。");
    }
    return config;
}
} // namespace core
