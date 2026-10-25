// Real Service integration tests. Every path is under an exclusively-created temporary root.
#include "../src/common/utf.h"
#include "config.h"
#include "file_ops.h"
#include "game_detect.h"
#include "logger.h"
#include "root_store.h"
#include "path_policy.h"
#include "../src/common/winutil.h"
#include "service.h"
#include "test_isolation.h"
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <functional>
#include <json.hpp>
#include <map>
#include <mutex>
#include <set>
#include <vector>
using namespace core;
using json = nlohmann::json;
static int passed = 0, failed = 0;
#define CHECK(c, n)                                                                                          \
    do {                                                                                                     \
        if (c) {                                                                                             \
            ++passed;                                                                                        \
            printf("[PASS] %s\n", n);                                                                        \
        } else {                                                                                             \
            ++failed;                                                                                        \
            printf("[FAIL] %s\n", n);                                                                        \
        }                                                                                                    \
    } while (0)
static std::map<int, json> replies;
static std::vector<json> observedEvents;
static int nextId = 200;
static void Collect(Service& svc) {
    for (const auto& event : svc.DrainEvents()) {
        auto j = json::parse(WideToUtf8(event.json));
        if (j.contains("event")) {
            if (observedEvents.size() >= 2048) observedEvents.erase(observedEvents.begin());
            observedEvents.push_back(j);
        }
        if (j.contains("id") && j["id"].is_number_integer()) {
            const int id = j["id"].get<int>(); // Evaluate before moving j (C++17 RHS precedes LHS).
            replies[id] = std::move(j);
        }
    }
}
static json Await(Service& svc, int id) {
    const auto deadline = GetTickCount64() + 7000;
    do {
        Collect(svc);
        auto it = replies.find(id);
        if (it != replies.end()) {
            auto r = it->second;
            replies.erase(it);
            return r;
        }
        Sleep(5);
    } while (GetTickCount64() < deadline);
    CHECK(false, "matching command response arrives before timeout");
    return json::object();
}
static json Call(Service& svc, const wchar_t* method, const std::wstring& params = L"{}") {
    const int id = ++nextId;
    svc.PostCommand(id, method, params);
    return Await(svc, id);
}
static json CallJson(Service& svc, const wchar_t* method, const json& params) {
    return Call(svc, method, Utf8ToWide(params.dump()));
}
static bool Error(const json& r, const char* code = nullptr) {
    return r.contains("ok") && !r["ok"].get<bool>() && r.contains("error") &&
           (!code || r["error"].value("code", "") == code);
}
static void Put(const std::wstring& path, const std::string& text) {
    const auto split = path.find_last_of(L"\\/");
    if (split != std::wstring::npos && !fileops::EnsureDir(path.substr(0, split)))
        throw std::runtime_error("test parent creation failed");
    HANDLE h = CreateFileW(testutil::Native(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("test file creation failed");
    DWORD n = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
    if (!ok || n != text.size())
        throw std::runtime_error("test file write failed");
}
static std::string Get(const std::wstring& path) {
    HANDLE h = CreateFileW(testutil::Native(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > 32 * 1024 * 1024) {
        CloseHandle(h);
        throw std::runtime_error("oversized test read");
    }
    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD n = 0;
    BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &n, nullptr);
    CloseHandle(h);
    if (!ok)
        throw std::runtime_error("test read failed");
    out.resize(n);
    return out;
}
// Raw Win32 enumeration is intentional: checked business inventories hide Internal names.
// Comparing the complete set also catches hidden files, stale temp files and future control names.
static bool RawChildrenEqual(const std::wstring& directory, const std::set<std::wstring>& expected) {
    std::set<std::wstring> actual;
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW(testutil::Native(directory + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND && fileops::IsDir(directory) && expected.empty();
    do {
        const std::wstring name = entry.cFileName;
        if (name != L"." && name != L"..")
            actual.insert(name);
    } while (FindNextFileW(find, &entry));
    const DWORD end = GetLastError();
    FindClose(find);
    if (end != ERROR_NO_MORE_FILES || actual != expected) {
        wprintf(L"[debug] raw children mismatch: %ls\n", directory.c_str());
        for (const auto& name : actual)
            wprintf(L"  %ls\n", name.c_str());
        return false;
    }
    return true;
}
static Service::StartOptions Options(const std::wstring& root) {
    Service::StartOptions o;
    o.allowAutostart = false;
    o.enableAutodetect = false;
    o.enableMonitoring = false;
    o.enableScheduling = false;
    o.validateGameRoot = false;
    return o;
}
static bool SameSettings(const Service::Status& a, const Service::Status& b) {
    return a.autoOn == b.autoOn && a.autostart == b.autostart && a.theme == b.theme &&
           a.closeAction == b.closeAction && a.syncMinutes == b.syncMinutes && a.gameRoot == b.gameRoot &&
           a.cacheRoot == b.cacheRoot;
}
// Selected ordinary roots are an explicit authority; unrelated root files and entire
// non-managed subtrees must survive preparation, import, copy and restore.
static void SeedPersonalFiles(const std::wstring& root) {
    Put(root + L"\\personal.txt", "PERSONAL-ROOT");
    Put(root + L"\\projects\\nested\\design.bin", "PERSONAL-SUBTREE");
}
static bool PersonalFilesIntact(const std::wstring& root) {
    return Get(root + L"\\personal.txt") == "PERSONAL-ROOT" &&
           Get(root + L"\\projects\\nested\\design.bin") == "PERSONAL-SUBTREE";
}
// Actual worker/monitor barriers: bounded waits synchronize phases, never Sleep races.
class PhaseHarness {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::wstring, int> counts;
    std::wstring blockedPhase;
    bool entered = false, released = true;

  public:
    void Observe(const std::wstring& phase) {
        std::unique_lock<std::mutex> lock(mutex);
        ++counts[phase];
        changed.notify_all();
        if (!released && phase == blockedPhase) {
            entered = true;
            changed.notify_all();
            changed.wait_for(lock, std::chrono::seconds(10), [&] { return released; });
        }
    }
    void Block(const std::wstring& phase) {
        std::lock_guard<std::mutex> lock(mutex);
        blockedPhase = phase;
        entered = false;
        released = false;
    }
    bool Entered() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(8), [&] { return entered; });
    }
    bool Wait(const std::wstring& phase, int count = 1) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(8), [&] { return counts[phase] >= count; });
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
};
struct ControlledStop {
    Service& service;
    PhaseHarness& phases;
    ~ControlledStop() {
        phases.Release();
        service.Stop();
    }
};
static bool WaitFor(const std::function<bool()>& predicate, DWORD timeoutMs) {
    const auto deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (predicate())
            return true;
        Sleep(10);
    }
    return predicate();
}
static void Test_Transactions() {
    testutil::Directory data(L"service-transactions");
    const auto root = data.path(), install = root + L"\\install", cache = root + L"\\cache",
               cfg = install + L"\\config.json", game = root + L"\\game";
    auto& s = Service::Instance();
    auto o = Options(root);
    CHECK(s.StartWithOptions(nullptr, install, cache, o),
          "isolated real service starts without registry, monitoring or discovery");
    struct Stop {
        Service& s;
        ~Stop() {
            s.Stop();
        }
    } stop{s};
    fileops::EnsureDir(game);
    CHECK(CallJson(s, L"set_game_root", {{"path", WideToUtf8(game)}}).value("ok", false),
          "fake game path accepted only through explicit test option");
    CHECK(
        Call(s, L"save_settings", L"{\"sync_minutes\":20,\"theme\":1,\"autostart\":true}").value("ok", false),
        "valid multi-field settings persist without registry writes");
    const auto original = Get(cfg);
    const auto status = s.GetStatus();
    const wchar_t* badSettings[] = {L"[]",
                                    L"null",
                                    L"{",
                                    L"{\"sync_minutes\":0}",
                                    L"{\"sync_minutes\":1441}",
                                    L"{\"sync_minutes\":1.5}",
                                    L"{\"sync_minutes\":18446744073709551615}",
                                    L"{\"theme\":2}",
                                    L"{\"theme\":false}",
                                    L"{\"close_action\":3}",
                                    L"{\"autostart\":1}",
                                    L"{\"theme\":0,\"sync_minutes\":\"bad\"}",
                                    L"{\"theme\":0,\"cache_root\":\"C:/\"}"};
    for (const auto* p : badSettings) {
        auto r = Call(s, L"save_settings", p);
        CHECK(Error(r), "invalid/mixed settings rejected");
        CHECK(Get(cfg) == original && SameSettings(status, s.GetStatus()),
              "rejected transaction leaves bytes and live settings unchanged");
    }
    for (const auto* p : {L"{}", L"{\"enabled\":1}", L"{\"enabled\":null}"}) {
        CHECK(Error(Call(s, L"set_auto", p)), "set_auto requires an actual Boolean");
        CHECK(Get(cfg) == original && SameSettings(status, s.GetStatus()),
              "invalid auto toggle cannot mutate config or status");
    }
    for (const auto& path : {game, game + L"\\nested", game + L"\\other"}) {
        CHECK(Error(CallJson(s, L"set_cache_root", {{"path", WideToUtf8(path)}})),
              "overlapping selected cache roots rejected before preparing the selected root");
        CHECK(Get(cfg) == original && SameSettings(status, s.GetStatus()),
              "rejected cache path is fully atomic");
    }
    HANDLE locked = CreateFileW(cfg.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE,
          "hold configuration against replacement to inject persistence failure");
    if (locked != INVALID_HANDLE_VALUE) {
        CHECK(Error(Call(s, L"save_settings", L"{\"theme\":0}"), "E_CONFIG"),
              "configuration IO failure is returned to caller");
        CHECK(SameSettings(status, s.GetStatus()) && Get(cfg) == original,
              "failed persistence cannot change live settings");
        CloseHandle(locked);
    }
    const auto parent = root + L"\\selected parent";
    Put(parent + L"\\unrelated.txt", "KEEP-UNRELATED");
    auto r = CallJson(s, L"set_cache_root", {{"path", WideToUtf8(parent)}});
    // Explicit product change: ordinary selections now use the root itself, not a wrapper.
    CHECK(r.value("ok", false), "nonempty ordinary selection is prepared directly");
    CHECK(s.GetStatus().cacheRoot == parent && !fileops::Exists(parent + L"\\LOLBackupV3Cache"),
          "selected cache path is applied without creating a wrapper child");
    CHECK(Get(parent + L"\\unrelated.txt") == "KEEP-UNRELATED",
          "unrelated selected-parent data remains byte-identical");
    CHECK(Call(s, L"cancel_op").value("ok", false), "cancellation is acknowledged even while idle");
    s.Stop();
    CHECK(!s.IsRunning() && !s.IsBusy(), "Stop joins workers and clears busy state");
    CHECK(Error(Call(s, L"get_status"), "E_STOPPED"),
          "commands after Stop return E_STOPPED instead of disappearing");
    CHECK(s.StartWithOptions(nullptr, install, cache, o), "same singleton restarts after Stop");
    CHECK(s.GetStatus().theme == 1 && s.GetStatus().autostart, "restart preserves committed settings");
}
static void Test_CorruptStartup() {
    testutil::Directory data(L"service-corrupt");
    const auto root = data.path(), install = root + L"\\install", cfg = install + L"\\config.json";
    const std::string broken = "{\"game_root\":\"important-path\",\"theme\":\"bad\"}";
    Put(cfg, broken);
    Put(install + L"\\game_root.txt", "legacy-path-must-not-overwrite");
    auto& s = Service::Instance();
    CHECK(!s.StartWithOptions(nullptr, install, root + L"\\cache", Options(root)),
          "corrupt startup is refused by real Service");
    CHECK(!s.IsRunning() && Get(cfg) == broken,
          "corrupt bytes remain exact and legacy migration cannot overwrite them");
    s.Stop();
}
// Manual restore ignores process policy; actual occupancy/permissions still come from Win32.
static void Test_DirectManualRestoreAndCopyOnlyBackup() {
    testutil::Directory data(L"manual-direct-policy");
    const auto root = data.path(), game = root + L"\\game", cache = root + L"\\cache";
    auto& s = Service::Instance();
    auto o = Options(root);
    std::atomic<int> lobby{-1}, gameProbe{1};
    o.monitorProbe = [&](const std::wstring&) { return lobby.load(); };
    o.gameProbe = [&](const std::wstring&) { return gameProbe.load(); };
    CHECK(s.StartWithOptions(nullptr, root + L"\\install", cache, o), "direct manual service starts");
    PhaseHarness phases;
    ControlledStop stop{s, phases};
    Put(game + L"\\saves\\one.dat", "GAME-OLD");
    Put(cache + L"\\saves\\one.dat", "CACHE-NEW");
    CHECK(CallJson(s, L"set_game_root", {{"path", WideToUtf8(game)}}).value("ok", false),
          "manual direct fixture selects its Game root");
    for (int probe : {-1, 1, 0}) {
        lobby = probe;
        gameProbe = probe;
        Put(game + L"\\saves\\one.dat", "OVERWRITE-ME");
        CHECK(Call(s, L"restore_now").value("ok", false), "manual restore allowed for unknown, running and offline probes");
        CHECK(Get(game + L"\\saves\\one.dat") == "CACHE-NEW", "manual restore directly overwrites the selected Game file");
        CHECK(!s.IsBusy() && s.GetStatus().progressPercent == -1, "manual restore clears busy/progress after every attempt");
    }
    lobby = 1;
    gameProbe = 1;
    Put(game + L"\\saves\\one.dat", "GAME-BACKUP");
    Put(cache + L"\\bot_5.dll", "COPY-ONLY-KEEP");
    CHECK(Call(s, L"backup_now").value("ok", false), "manual backup allowed online with game1");
    CHECK(Get(cache + L"\\saves\\one.dat") == "GAME-BACKUP" && Get(cache + L"\\bot_5.dll") == "COPY-ONLY-KEEP",
          "explicit manual backup remains copy-only rather than full timed mirror");
    CHECK(s.GetStatus().cacheEntries == 2 && s.GetStatus().lastSync != L"--", "manual backup refreshes physical cache count and last sync");
}
static void Test_ImportWithoutGameAndCancellationEpoch() {
    testutil::Directory data(L"service-import");
    const auto root = data.path(), install = root + L"\\install", cache = root + L"\\cache",
               source = root + L"\\approved old cache";
    auto& s = Service::Instance();
    auto options = Options(root);
    PhaseHarness phases;
    options.enableScheduling = true;
    options.onPhase = [&](const std::wstring& phase) { phases.Observe(phase); };
    CHECK(s.StartWithOptions(nullptr, install, cache, options),
          "import service starts without configured game");
    ControlledStop stop{s, phases};
    CHECK(s.GetStatus().gameRoot.empty() && s.GetStatus().cacheEntries == 0,
          "external management files never appear as business cache entries");
    CHECK(Error(Call(s, L"backup_now", L"{not valid json"), "E_INPUT"),
          "malformed backup job parameters return E_INPUT");
    CHECK(Error(Call(s, L"backup_now"), "E_GAME"),
          "malformed job released admission gate for subsequent job");
    Put(source + L"\\saves\\a.dat", "SAVE-A");
    Put(source + L"\\bot_5.dll", "BOT-A");
    Put(source + L"\\config.ini", "CONFIG-A");
    auto result = CallJson(s, L"import_cache", {{"path", WideToUtf8(source)}});
    CHECK(result.value("ok", false) && result.contains("data") && result["data"].value("copied", -1) == 3,
          "real import without game configuration reports exactly three copied files");
    CHECK(Get(cache + L"\\saves\\a.dat") == "SAVE-A" && Get(cache + L"\\bot_5.dll") == "BOT-A" &&
              Get(cache + L"\\config.ini") == "CONFIG-A",
          "imported business files have exact approved source bytes");
    CHECK(Get(source + L"\\saves\\a.dat") == "SAVE-A" && Get(source + L"\\bot_5.dll") == "BOT-A" &&
              Get(source + L"\\config.ini") == "CONFIG-A",
          "real import never changes or removes source data");
    CHECK(s.GetStatus().cacheEntries == 3,
          "cacheEntries counts only imported business files with management stored externally");
    CHECK(Error(CallJson(s, L"import_cache", {{"path", WideToUtf8(cache)}}), "E_PATH"),
          "source identical to active cache is refused");
    CHECK(Get(cache + L"\\saves\\a.dat") == "SAVE-A" && Get(cache + L"\\bot_5.dll") == "BOT-A" &&
              s.GetStatus().cacheEntries == 3,
          "rejected self-import leaves active data intact");
    const auto invalid = source + std::wstring(1, L'\0') + L"ignored suffix";
    CHECK(Error(CallJson(s, L"import_cache", {{"path", WideToUtf8(invalid)}}), "E_PATH"),
          "embedded NUL import path is rejected without prefix truncation");
    CHECK(Get(source + L"\\saves\\a.dat") == "SAVE-A" && Get(cache + L"\\saves\\a.dat") == "SAVE-A",
          "NUL rejection leaves source and destination byte-identical");
    const auto rootOnly = root + L"\\root files only";
    const std::string payload(8 * 1024 * 1024, 'R');
    Put(rootOnly + L"\\bot_5.dll", payload);
    Put(rootOnly + L"\\HID.dll", payload);
    phases.Block(L"scheduler-check");
    CHECK(phases.Entered(), "worker barrier holds import before dequeue for deterministic cancellation");
    const int importId = ++nextId;
    s.PostCommand(importId, L"import_cache", Utf8ToWide(json{{"path", WideToUtf8(rootOnly)}}.dump()));
    CHECK(Call(s, L"cancel_op").value("ok", false),
          "root-file-only import cancellation acknowledged out of worker queue");
    phases.Release();
    CHECK(Error(Await(s, importId), "E_CANCELLED"),
          "queued or active root-file-only import observes cancellation epoch");
    result = CallJson(s, L"import_cache", {{"path", WideToUtf8(rootOnly)}});
    CHECK(result.value("ok", false) && result.contains("data") && result["data"].value("copied", -1) == 2,
          "new import after cancellation is not poisoned by stale cancellation flag");
    CHECK(Get(cache + L"\\bot_5.dll") == payload && Get(cache + L"\\HID.dll") == payload,
          "subsequent root-file import commits both complete byte-identical files");
    CHECK(Get(rootOnly + L"\\bot_5.dll") == payload && Get(rootOnly + L"\\HID.dll") == payload,
          "cancellation and retry preserve exact original root-file source bytes");
}
static void Test_BoundedRealLogResponses() {
    testutil::Directory data(L"service-logs");
    const auto root = data.path();
    auto& service = Service::Instance();
    CHECK(service.StartWithOptions(nullptr, root + L"\\install", root + L"\\cache", Options(root)),
          "real isolated service log fixture starts");
    struct Stop {
        Service& service;
        ~Stop() {
            service.Stop();
        }
    } stop{service};
    const std::wstring unicodePayload(16 * 1024, L'中');
    bool allWritten = true;
    for (int i = 0; i < 80; ++i) {
        Log().Info(L"service-safety",
                   L"bounded-log-" + std::to_wstring(i) + L":" + unicodePayload + L" \U0001F680");
        allWritten = allWritten && Log().LastError().empty();
    }
    CHECK(allWritten, "80 real 16k Unicode log messages written only to the fixture's rotated logs");
    Log().Error(L"long-error-field", unicodePayload);
    auto response = Call(service, L"get_logs", L"{\"limit\":5}");
    CHECK(response.value("ok", false) && response.contains("data") && response["data"].contains("logs") &&
              response["data"]["logs"].is_array() && response["data"]["logs"].size() == 5,
          "get_logs limit5 returns the five newest records rather than an unbounded history");
    if (response.value("ok", false) && response.contains("data")) {
        const auto text = response["data"].value("text", std::string());
        bool keptError = false;
        for (const auto& record : response["data"]["logs"])
            keptError = keptError ||
                        (record.value("level", "") == "error" &&
                         record.value("module", "") == "long-error-field" && !record.value("ts", "").empty());
        CHECK(keptError && text.find("错误 · ") != std::string::npos,
              "long Unicode error record retains parsed level/module/timestamp before message truncation");
        CHECK(text.size() <= 256 * 1024 && response.dump().size() <= 256 * 1024,
              "both log text and serialized real response remain within 256KiB");
        CHECK(text.find("bounded-log-79:") != std::string::npos &&
                  text.find("bounded-log-0:") == std::string::npos,
              "bounded output retains the actual newest log messages");
        bool valid = true;
        for (const auto& record : response["data"]["logs"]) {
            if (!record.is_object() || !record.contains("msg") || !record["msg"].is_string()) {
                valid = false;
                continue;
            }
            const auto message = record["msg"].get<std::string>();
            std::wstring wide;
            std::string roundtrip;
            valid = valid && TryUtf8ToWide(message, wide) && TryWideToUtf8(wide, roundtrip) &&
                    roundtrip == message && message.find("\xef\xbf\xbd") == std::string::npos;
        }
        CHECK(valid, "every truncated Unicode record remains strict UTF8 without replacement characters");
    }
    response = Call(service, L"get_logs", L"{\"limit\":1000}");
    CHECK(response.value("ok", false) && response.contains("data") &&
              response["data"]["logs"].size() <= 1000 &&
              response["data"].value("text", std::string()).size() <= 256 * 1024 &&
              response.dump().size() <= 256 * 1024,
          "maximum valid limit is still bounded by the response byte budget");
    CHECK(Error(Call(service, L"get_logs", L"{\"limit\":0}"), "E_INPUT"),
          "zero log limit is rejected explicitly");
    CHECK(Error(Call(service, L"get_logs", L"{\"limit\":1001}"), "E_INPUT"),
          "over-1000 log limit is rejected explicitly");
}
static void Test_AdoptBusinessCacheFolder() {
    testutil::Directory data(L"service-adopt-cache");
    const auto root = data.path(), install = root + L"\\install", cache = root + L"\\cache",
               game = root + L"\\game";
    const auto business = root + L"\\my cache", mixed = root + L"\\mixed folder";
    Put(business + L"\\saves\\one.dat", "SAVE-1");
    Put(business + L"\\bot_5.dll", "BOT-5");
    Put(business + L"\\HID.dll", "HID-5"); // mixed-case whitelist entry must match case-insensitively
    if (!fileops::EnsureDir(business + L"\\LOLBackupV3Cache"))
        throw std::runtime_error("leftover directory fixture failed");
    Put(mixed + L"\\saves\\one.dat", "SAVE-1");
    Put(mixed + L"\\notes.txt", "KEEP-ME");
    auto& s = Service::Instance();
    auto o = Options(root);
    CHECK(s.StartWithOptions(nullptr, install, cache, o), "adoption service starts");
    struct Stop {
        Service& s;
        ~Stop() {
            s.Stop();
        }
    } stop{s};
    CHECK(CallJson(s, L"set_game_root", {{"path", WideToUtf8(game)}}).value("ok", false),
          "adoption fixture game configured");
    auto adopted = CallJson(s, L"set_cache_root", {{"path", WideToUtf8(business)}});
    if (!adopted.value("ok", false))
        printf("[debug] adopt reply=%s\n", adopted.dump().c_str());
    CHECK(adopted.value("ok", false), "legal business folder is accepted as a cache root");
    if (s.GetStatus().cacheRoot != business)
        printf("[debug] cacheRoot=%ls entries=%d expected=%ls\n", s.GetStatus().cacheRoot.c_str(),
               s.GetStatus().cacheEntries, business.c_str());
    CHECK(s.GetStatus().cacheRoot == business,
          "legal cache folder is used directly without nesting a subfolder");
    CHECK(s.GetStatus().cacheEntries == 3, "adopted cache counts its existing business files");
    CHECK(Get(business + L"\\bot_5.dll") == "BOT-5" && Get(business + L"\\saves\\one.dat") == "SAVE-1",
          "adoption never alters the user's existing business bytes");
    // Explicit product change: mixed business/user data is no longer redirected to a child.
    auto selected = CallJson(s, L"set_cache_root", {{"path", WideToUtf8(mixed)}});
    CHECK(selected.value("ok", false) && s.GetStatus().cacheRoot == mixed &&
              !fileops::Exists(mixed + L"\\LOLBackupV3Cache"),
          "mixed folder is used directly without generating a cache wrapper");
    CHECK(Get(mixed + L"\\notes.txt") == "KEEP-ME", "unrelated user data is never touched");
}
static void Test_StartupKeepsConfiguredNestedCache() {
    testutil::Directory data(L"service-keeps-nested-cache");
    const auto root = data.path(), install = root + L"\\install", fallback = root + L"\\cache",
               game = root + L"\\game", parent = root + L"\\real cache",
               nested = parent + L"\\LOLBackupV3Cache";
    Put(parent + L"\\bot_5.dll", "BOT-5");
    Put(parent + L"\\HID.dll", "HID-5");
    if (!fileops::EnsureDir(nested) || !fileops::EnsureDir(install))
        throw std::runtime_error("configured nested fixture failed");
    Put(nested + L"\\.lolbackup-owned-v1", "LOLBackup dedicated mirror root v1\r\n");
    Put(nested + L"\\.~lolbackup-service.lock", "");
    core::Config cfg;
    cfg.gameRoot = game;
    cfg.cacheRoot = nested;
    cfg.syncMinutes = 5;
    CHECK(cfg.Save(install + L"\\config.json"), "configured nested fixture configuration persisted");
    auto& s = Service::Instance();
    auto o = Options(root);
    CHECK(s.StartWithOptions(nullptr, install, fallback, o), "configured nested service starts");
    struct Stop {
        Service& s;
        ~Stop() {
            s.Stop();
        }
    } stop{s};
    CHECK(s.GetStatus().cacheRoot == nested && s.GetStatus().cacheEntries == 0,
          "startup uses exactly the configured nested cache, never its business-bearing parent");
    CHECK(Get(parent + L"\\bot_5.dll") == "BOT-5" && Get(parent + L"\\HID.dll") == "HID-5" &&
              fileops::IsDir(nested),
          "legacy controls do not change the explicitly configured folder or move parent business files");
    Config persisted;
    CHECK(Config::LoadSafe(install + L"\\config.json", persisted) && persisted.cacheRoot == nested,
          "startup does not silently rewrite configured cache path during legacy control migration");
}
static void Test_StartupClaimsBusinessCache() {
    testutil::Directory data(L"service-claim-start");
    const auto root = data.path(), install = root + L"\\install", fallback = root + L"\\cache",
               game = root + L"\\game", cache = root + L"\\files cache";
    Put(cache + L"\\bot_5.dll", "BOT-7");
    Put(cache + L"\\saves\\one.dat", "SAVE-7");
    if (!fileops::EnsureDir(install))
        throw std::runtime_error("claim-start fixture failed");
    core::Config cfg;
    cfg.gameRoot = game;
    cfg.cacheRoot = cache; // no ownership marker on purpose
    cfg.syncMinutes = 5;
    CHECK(cfg.Save(install + L"\\config.json"), "claim-start configuration persisted");
    auto& s = Service::Instance();
    auto o = Options(root);
    CHECK(s.StartWithOptions(nullptr, install, fallback, o),
          "startup claims a business-only cache folder that has no marker yet");
    struct Stop {
        Service& s;
        ~Stop() {
            s.Stop();
        }
    } stop{s};
    CHECK(RawChildrenEqual(cache, {L"bot_5.dll", L"saves"}) &&
              RawChildrenEqual(cache + L"\\saves", {L"one.dat"}),
          "existing business cache is managed externally without introducing root or saves markers");
    CHECK(s.GetStatus().cacheEntries == 2 && s.GetStatus().cacheRoot == cache,
          "claimed cache is used directly with its business files counted");
}
static bool CopySelfTo(const std::wstring& path) {
    wchar_t self[32768]{};
    if (!GetModuleFileNameW(nullptr, self, 32768))
        return false;
    if (!fileops::EnsureDir(path.substr(0, path.find_last_of(L'\\'))))
        return false;
    return CopyFileW(self, path.c_str(), FALSE) != FALSE;
}
static void Test_ForeignGameDetection() {
    testutil::Directory data(L"game-detect");
    const auto root = data.path();
    const auto foreign = root + L"\\Riot Games\\League of Legends\\Game";
    CHECK(CopySelfTo(foreign + L"\\League of Legends.exe") &&
              CopySelfTo(root + L"\\Riot Games\\League of Legends\\LeagueClient.exe"),
          "foreign layout fixture created");
    CHECK(GameDetect::IsGameRoot(foreign), "Riot Games layout is accepted as a foreign install");
    CHECK(GameDetect::Find(foreign, false) == foreign, "explicit foreign path is returned unchanged");

    const auto wegame = root + L"\\WeGameApps\\英雄联盟\\Game";
    CHECK(CopySelfTo(wegame + L"\\League of Legends.exe") &&
              CopySelfTo(root + L"\\WeGameApps\\英雄联盟\\LeagueClient.exe"),
          "China layout fixture created");
    CHECK(!GameDetect::IsGameRoot(wegame), "WeGame/hero-name path is rejected");
    CHECK(GameDetect::Find(wegame, false).empty(), "explicit China path is not adopted");

    const auto tcls = root + L"\\Games\\League of Legends\\Game";
    CHECK(CopySelfTo(tcls + L"\\League of Legends.exe") &&
              CopySelfTo(root + L"\\Games\\League of Legends\\LeagueClient.exe"),
          "China component fixture created");
    if (!fileops::EnsureDir(root + L"\\Games\\League of Legends\\TCLS"))
        throw std::runtime_error("TCLS fixture failed");
    CHECK(!GameDetect::IsGameRoot(tcls), "TCLS component marks the client as China-only");

    const auto tencent = root + L"\\腾讯游戏\\League of Legends\\Game";
    CHECK(CopySelfTo(tencent + L"\\League of Legends.exe") &&
              CopySelfTo(root + L"\\腾讯游戏\\League of Legends\\LeagueClient.exe"),
          "Tencent path fixture created");
    CHECK(!GameDetect::IsGameRoot(tencent), "Tencent Games path is rejected");
}
static void SeedServiceConfig(const std::wstring& root) {
    fileops::EnsureDir(root + L"\\install");
    Config cfg;
    cfg.gameRoot = root + L"\\game";
    cfg.cacheRoot = root + L"\\cache";
    CHECK(cfg.Save(root + L"\\install\\config.json"), "deterministic service configuration saved");
}
static void Test_RootDependencyCASRefusesStaleValidation() {
    testutil::Directory data(L"root-dependency-cas");
    const auto root = data.path(), install = root + L"\\install", cache = root + L"\\cache",
               selected = root + L"\\other-game", config = install + L"\\config.json";
    Put(root + L"\\game\\config.ini", "ACTIVE-GAME");
    Put(cache + L"\\config.ini", "ACTIVE-CACHE");
    Put(selected + L"\\config.ini", "UNTOUCHED-OTHER-GAME");
    SeedServiceConfig(root);
    PhaseHarness phases;
    std::atomic<bool> inject{true};
    auto o = Options(root);
    o.onPhase = [&](const std::wstring& phase) {
        if (phase == L"settings-before-commit" && inject.exchange(false)) {
            CHECK(Config::UpdateAtomic(
                      config,
                      [](Config& c, void* path) {
                          c.cacheRoot = *static_cast<const std::wstring*>(path);
                          return true;
                      },
                      const_cast<std::wstring*>(&selected)),
                  "inject root dependency change after service path validation");
        }
        phases.Observe(phase);
    };
    auto& s = Service::Instance();
    CHECK(s.StartWithOptions(nullptr, install, cache, o),
          "root CAS fixture starts with explicitly selected disjoint roots");
    ControlledStop stop{s, phases};
    CHECK(Error(CallJson(s, L"set_game_root", {{"path", WideToUtf8(selected)}}), "E_CONFIG"),
          "lock-held CAS conflict rejects externally changed overlapping root dependency");
    Config actual;
    CHECK(Config::LoadSafe(config, actual) && actual.gameRoot == root + L"\\game" &&
              actual.cacheRoot == selected,
          "service does not persist requested Game with an unvalidated overlapping external cache root");
    CHECK(s.GetStatus().gameRoot == root + L"\\game" && s.GetStatus().cacheRoot == cache &&
              Get(cache + L"\\config.ini") == "ACTIVE-CACHE" &&
              Get(selected + L"\\config.ini") == "UNTOUCHED-OTHER-GAME",
          "rejected dependency conflict leaves active roots and all directory bytes untouched");
}

// The production monitor samples lobby once per interval; worker I/O must never probe processes.
struct DirectHarness {
    PhaseHarness phases;
    std::atomic<int> lobby{0}, game{0}, samples{0};
    std::atomic<int> singleLobbyObservation{-2}, workerLobbyObservation{-2};
    std::atomic<int> lobbyProbeCalls{0}, gameProbeCalls{0}, workerLobbyCalls{0}, workerGameCalls{0};
    std::atomic<DWORD> monitorThread{0};
    std::atomic<ULONGLONG> tick{1000};
    std::atomic<int> backupBegins{0}, backupReturns{0}, restores{0}, restoreBegins{0};
    std::atomic<int> starts{0}, queued{0}, begins{0}, returned{0}, cleanupQueued{0};
    std::atomic<int> onlineEdges{0}, offlineEdges{0};
    Service::StartOptions MakeOptions(const std::wstring& root) {
        auto o = Options(root);
        o.enableScheduling = true;
        o.monitorIntervalMs = 50;
        o.syncPeriodMs = 24 * 60 * 60 * 1000;
        o.monotonicClock = [&] { return tick.load(); };
        o.monitorProbe = [&](const std::wstring&) {
            const DWORD tid = GetCurrentThreadId();
            DWORD absent = 0;
            monitorThread.compare_exchange_strong(absent, tid);
            ++lobbyProbeCalls;
            if (tid != monitorThread.load()) {
                ++workerLobbyCalls;
                const int fresh = workerLobbyObservation.load();
                return fresh != -2 ? fresh : lobby.load();
            }
            ++samples;
            const int once = singleLobbyObservation.exchange(-2);
            return once != -2 ? once : lobby.load();
        };
        o.gameProbe = [&](const std::wstring&) {
            ++gameProbeCalls;
            if (GetCurrentThreadId() != monitorThread.load())
                ++workerGameCalls;
            return game.load();
        };
        o.onPhase = [&](const std::wstring& phase) {
            if (phase == L"online-edge") ++onlineEdges;
            else if (phase == L"offline-edge") ++offlineEdges;
            else if (phase == L"restore-begin") ++restoreBegins;
            else if (phase == L"restore-returned") ++restores;
            else if (phase == L"backup-begin") ++backupBegins;
            else if (phase == L"backup-returned") ++backupReturns;
            else if (phase == L"match-started") ++starts;
            else if (phase == L"match-backup-queued") ++queued;
            else if (phase == L"match-backup-begin") ++begins;
            else if (phase == L"match-backup-returned") ++returned;
            else if (phase == L"cleanup-queued") ++cleanupQueued;
            phases.Observe(phase);
        };
        return o;
    }
    bool SampleWindow() {
        const int before = samples.load();
        return WaitFor([&] { return samples.load() >= before + 4; }, 3000);
    }
};
struct DirectFixture {
    testutil::Directory data;
    const std::wstring root, game, cache;
    Service& service;
    DirectHarness h;
    ControlledStop stop;
    explicit DirectFixture(const wchar_t* name)
        : data(name), root(data.path()), game(root + L"\\game"), cache(root + L"\\cache"),
          service(Service::Instance()), stop{service, h.phases} {
        Put(game + L"\\config.ini", "GAME-INITIAL");
        Put(cache + L"\\config.ini", "CACHE-INITIAL");
        SeedServiceConfig(root);
    }
    void Start(const Service::StartOptions& options) {
        CHECK(service.StartWithOptions(nullptr, root + L"\\install", cache, options),
              "isolated direct-policy fixture starts with virtual time and phase hooks");
    }
    void Launch(int periodMs = 24 * 60 * 60 * 1000) {
        auto o = h.MakeOptions(root);
        o.syncPeriodMs = periodMs;
        Start(o);
        CHECK(h.phases.Wait(L"offline-edge"), "launch fixture observes initial offline baseline");
        h.lobby = 1;
        CHECK(h.phases.Wait(L"restore-returned"), "launch restore attempt returns before live Game edits");
        CHECK(Get(game + L"\\config.ini") == "CACHE-INITIAL", "launch directly restores physical cache bytes");
    }
    void Settle() {
        CHECK(h.SampleWindow(), "actual monitor samples complete without guessed timing sleeps");
        CHECK(Call(service, L"echo").value("ok", false), "worker drains after observed monitor samples");
    }
};
static bool IsSyncCountdown(const std::wstring& text) {
    return !text.empty() && text[0] >= L'0' && text[0] <= L'9' &&
           text.find(L"分") != std::wstring::npos && text.find(L"秒") != std::wstring::npos;
}
static void CheckAutoPolicy(Service& service, bool periodicReady, bool matchBackupReady) {
    const auto status = service.GetStatus();
    CHECK(status.periodicReady == periodicReady && status.matchBackupReady == matchBackupReady,
          "status reports periodic readiness and never authorizes removed match-end backups");
    CHECK(!status.autoState.empty() && !status.autoReason.empty(),
          "automatic policy exposes an explicit state and human-readable reason");
    if (!periodicReady)
        CHECK(!IsSyncCountdown(status.nextSync), "a non-ready periodic policy never displays a countdown");
}
static void CheckFullVirtualPeriod(Service& service) {
    CHECK(service.GetStatus().nextSync == L"0分10秒",
          "status countdown uses the 10000ms virtual syncPeriodMs override, not syncMinutes");
}
static void SchedulerAt(DirectFixture& f, ULONGLONG tick) {
    f.h.phases.Block(L"scheduler-check");
    f.h.tick = tick;
    const int wakeId = ++nextId;
    f.service.PostCommand(wakeId, L"echo", L"{}");
    CHECK(f.h.phases.Entered(), "virtual clock reaches the actual worker scheduler decision");
    f.h.phases.Release();
    CHECK(Await(f.service, wakeId).value("ok", false) && Call(f.service, L"echo").value("ok", false),
          "worker completes the controlled scheduler decision before assertions");
}

static void Test_LobbyCycle() {
    CHECK(Service::AutoBackupDue(true, true, 1, 60000, 60000), "online elapsed period is due");
    CHECK(!Service::AutoBackupDue(true, true, 0, 600000, 60000), "offline is never due");
    CHECK(!Service::AutoBackupDue(true, true, -1, 600000, 60000), "unknown is never due");
    DirectFixture f(L"strict-multicycle-full-mirror");
    SeedPersonalFiles(f.cache);
    SeedPersonalFiles(f.game);
    f.Launch(10000);
    for (int cycle = 1; cycle <= 2; ++cycle) {
        const auto live = std::string("LIVE-CYCLE-") + std::to_string(cycle);
        Put(f.game + L"\\config.ini", live);
        Put(f.game + L"\\saves\\current.dat", "LIVE-SAVE");
        Put(f.cache + L"\\saves\\obsolete.dat", "REMOVE-ACTIVE-EXTRA");
        Put(f.cache + L"\\shards\\absent-group.dat", "REMOVE-ABSENT-GROUP");
        Put(f.cache + L"\\bot_5.dll", "REMOVE-ABSENT-ROOT-ENTRY");
        const auto due = f.h.tick.load() + 10000;
        SchedulerAt(f, due - 1);
        CHECK(f.h.backupBegins == cycle - 1 && Get(f.cache + L"\\config.ini") != live,
              "custom interval forbids early mirror in every cycle");
        SchedulerAt(f, due);
        CHECK(f.h.backupBegins == cycle && Get(f.cache + L"\\config.ini") == live &&
                  Get(f.cache + L"\\saves\\current.dat") == "LIVE-SAVE" &&
                  !fileops::Exists(f.cache + L"\\saves\\obsolete.dat") &&
                  !fileops::Exists(f.cache + L"\\shards\\absent-group.dat") &&
                  !fileops::Exists(f.cache + L"\\bot_5.dll"),
              "each periodic job fully mirrors active and wholly absent managed groups");
        CHECK(PersonalFilesIntact(f.cache) && PersonalFilesIntact(f.game),
              "full managed mirror preserves unrelated root files and nested subtrees");
        const int offline = f.h.offlineEdges.load();
        f.h.lobby = 0;
        CHECK(f.h.phases.Wait(L"offline-edge", offline + 1), "cycle observes confirmed lobby exit");
        f.Settle();
        CHECK(Get(f.game + L"\\config.ini") == live && f.h.cleanupQueued == 0,
              "lobby exit never automatically clears the Game tree");
        Put(f.game + L"\\config.ini", "OFFLINE-DO-NOT-CACHE");
        SchedulerAt(f, due + 50000);
        CHECK(Get(f.cache + L"\\config.ini") == live &&
                  Get(f.cache + L"\\saves\\current.dat") == "LIVE-SAVE" &&
                  PersonalFilesIntact(f.cache) && f.h.backupBegins == cycle && f.h.queued == 0,
              "offline overdue scheduler leaves the frozen cache bytes and unrelated data unchanged");
        const int restored = f.h.restores.load();
        f.h.lobby = 1;
        CHECK(f.h.phases.Wait(L"restore-returned", restored + 1), "next launch restores the frozen cache");
        f.Settle();
        CHECK(Get(f.game + L"\\config.ini") == live && f.h.backupBegins == cycle,
              "next cycle restores frozen bytes before any Game-to-cache mirror");
    }
}
static void Test_CustomSyncMinutesControlsRealScheduler() {
    DirectFixture f(L"production-custom-sync-minutes");
    auto cfg = Config::Load(f.root + L"\\install\\config.json");
    cfg.syncMinutes = 7;
    CHECK(cfg.Save(f.root + L"\\install\\config.json"), "persist a nondefault user interval before startup");
    auto o = f.h.MakeOptions(f.root);
    o.syncPeriodMs = 0; // Exercise production sync_minutes conversion, not a test period override.
    f.Start(o);
    CHECK(f.h.phases.Wait(L"offline-edge"), "custom interval fixture observes offline baseline");
    f.h.lobby = 1;
    CHECK(f.h.phases.Wait(L"restore-returned"), "custom interval begins only after frozen-cache restore");
    f.Settle();
    CHECK(f.service.GetStatus().syncMinutes == 7 && f.service.GetStatus().nextSync == L"7分0秒",
          "status and worker both use persisted seven-minute interval");
    Put(f.game + L"\\config.ini", "SEVEN-MINUTE-LIVE");
    SchedulerAt(f, 420999);
    CHECK(f.h.backupBegins == 0, "production interval does not fire one millisecond early");
    SchedulerAt(f, 421000);
    CHECK(f.h.backupBegins == 1 && Get(f.cache + L"\\config.ini") == "SEVEN-MINUTE-LIVE",
          "production interval fires at exactly seven minutes after restore");
    CHECK(CallJson(f.service, L"save_settings", {{"sync_minutes", 3}}).value("ok", false),
          "online user customizes interval through actual settings transaction");
    Put(f.game + L"\\config.ini", "THREE-MINUTE-LIVE");
    SchedulerAt(f, 600999);
    CHECK(f.h.backupBegins == 1, "updated production interval still waits its complete duration");
    SchedulerAt(f, 601000);
    CHECK(f.h.backupBegins == 2 && Get(f.cache + L"\\config.ini") == "THREE-MINUTE-LIVE" &&
              f.service.GetStatus().nextSync == L"3分0秒",
          "live custom interval controls subsequent worker deadlines and countdown");
}

static void ToggleAuto(DirectFixture& f, bool enabled) {
    CHECK(CallJson(f.service, L"save_settings", {{"auto_on", enabled}}).value("ok", false),
          "explicit auto flag change commits through the real settings transaction");
}
static void Test_SingleOfflineSampleReopenRestoresFrozenCache() {
    DirectFixture f(L"one-offline-sample-quick-reopen");
    f.Launch(10000);
    Put(f.game + L"\\config.ini", "LIVE-NOT-YET-MIRRORED");
    f.h.phases.Block(L"offline-edge");
    f.h.singleLobbyObservation = 0;
    CHECK(f.h.phases.Entered() && f.h.offlineEdges == 2 && f.h.singleLobbyObservation == -2 && f.h.lobby == 1,
          "exactly one injected offline sample creates an immediate exit edge before lobby returns online");
    CHECK(Get(f.cache + L"\\config.ini") == "CACHE-INITIAL" && f.h.cleanupQueued == 0,
          "single offline sample freezes cache without final mirror or cleanup");
    f.h.phases.Release();
    CHECK(f.h.phases.Wait(L"online-edge", 2) && f.h.phases.Wait(L"restore-returned", 2),
          "two following online samples create a fresh launch even after only one offline sample");
    f.Settle();
    CHECK(f.h.restoreBegins == 2 && f.h.backupBegins == 0 && f.h.queued == 0 &&
              Get(f.game + L"\\config.ini") == "CACHE-INITIAL" && Get(f.cache + L"\\config.ini") == "CACHE-INITIAL",
          "quick reopen restores frozen cache instead of inheriting an unrestored live Game baseline");
}
static void Test_UnknownLobbyStatusNeverCountsDown() {
    DirectFixture f(L"unknown-lobby-policy-status");
    f.Launch(10000);
    CheckAutoPolicy(f.service, true, false);
    CheckFullVirtualPeriod(f.service);
    f.h.lobby = -1;
    f.Settle();
    const auto unknown = f.service.GetStatus();
    CHECK(!unknown.monitorKnown && unknown.monitorState == L"unknown",
          "synthetic unknown lobby is actually observed before checking readiness/countdown");
    CHECK(!unknown.periodicReady && !unknown.autoState.empty() && !unknown.autoReason.empty() &&
              !IsSyncCountdown(unknown.nextSync),
          "unknown lobby reports explicit non-ready policy rather than a misleading running countdown");
    SchedulerAt(f, 50000);
    CHECK(f.h.backupBegins == 0 && f.h.restoreBegins == 1 &&
              Get(f.cache + L"\\config.ini") == "CACHE-INITIAL",
          "unknown lobby never schedules a virtual-clock overdue mirror or fabricates another restore");
}
static void Test_QueuedAutoRestoreRetainsCancellationToken() {
    DirectFixture f(L"queued-restore-cancel-token");
    f.Start(f.h.MakeOptions(f.root));
    CHECK(f.h.phases.Wait(L"offline-edge"), "queued-restore fixture observes actual offline before launch");
    f.h.phases.Block(L"scheduler-check");
    CHECK(f.h.phases.Entered(), "worker scheduler barrier prevents dequeuing the upcoming automatic restore");
    f.h.lobby = 1;
    CHECK(f.h.phases.Wait(L"online-edge"), "monitor real offline-to-online edge queues restore behind held worker");
    CHECK(f.h.restoreBegins == 0 && f.h.restores == 0, "queued restore has not reached engine or worker return");
    CHECK(Call(f.service, L"cancel_op").value("ok", false), "out-of-queue cancel advances epoch after restore was queued");
    f.h.phases.Release();
    f.Settle();
    CHECK(f.h.restoreBegins == 0 && Get(f.game + L"\\config.ini") == "GAME-INITIAL" &&
              Get(f.cache + L"\\config.ini") == "CACHE-INITIAL" && f.h.backupBegins == 0,
          "cancel clears pending restore or rejects its original immutable token, never captures a fresh epoch at dequeue");
    CheckAutoPolicy(f.service, true, false); // Cancel ends this launch attempt, not a persistent protected state.
}
static void Test_MatchStartupOfflineIsNotAnEnd() {
    DirectFixture f(L"match-startup-offline");
    f.Start(f.h.MakeOptions(f.root));
    CHECK(f.h.phases.Wait(L"offline-edge"), "startup game0/lobby0 reaches its stable initial observation");
    f.Settle();
    CHECK(f.h.starts == 0 && f.h.queued == 0 && f.h.begins == 0 && f.h.returned == 0 &&
              f.h.backupBegins == 0 && f.h.restores == 0 && f.h.cleanupQueued == 0,
          "startup offline is not a match end, lobby launch or shutdown cleanup");
    CHECK(Get(f.game + L"\\config.ini") == "GAME-INITIAL" &&
              Get(f.cache + L"\\config.ini") == "CACHE-INITIAL",
          "initial stable game0 preserves both Game and the frozen cache byte-for-byte");
}
static void Test_AutoRestoreMixedRootIgnoresUnrelatedEntries() {
    DirectFixture f(L"auto-restore-mixed-unrelated");
    SeedPersonalFiles(f.cache);
    Put(f.cache + L"\\saves\\trusted.dat", "TRUSTED-CACHE-SAVE");
    const auto target = f.root + L"\\outside-unrelated.dat";
    const auto alias = f.cache + L"\\projects\\unrelated-link.dat";
    Put(target, "UNRELATED-HARDLINK-TARGET");
    CHECK(CreateHardLinkW(alias.c_str(), target.c_str(), nullptr),
          "ordinary cache fixture includes an unrelated hardlinked file");
    winutil::ScopedHandle locked(CreateFileW((f.cache + L"\\personal.txt").c_str(), GENERIC_READ,
                                             0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(locked.valid(), "unrelated ordinary file is exclusively locked for the whole restore");
    f.Launch();
    CHECK(Get(f.game + L"\\saves\\trusted.dat") == "TRUSTED-CACHE-SAVE" &&
              f.service.GetStatus().cacheEntries == 2 && f.h.restores == 1,
          "automatic restore and complete business count ignore unrelated locked/hardlinked entries");
    locked.reset();
    CHECK(PersonalFilesIntact(f.cache) && Get(alias) == "UNRELATED-HARDLINK-TARGET" &&
              Get(target) == "UNRELATED-HARDLINK-TARGET",
          "automatic restore never changes unrelated ordinary files or the external link target");
}
static void Test_StartupOnlineRestoresBeforePeriodic() {
    DirectFixture f(L"startup-online-restore-first");
    f.h.lobby = 1;
    auto o = f.h.MakeOptions(f.root);
    o.syncPeriodMs = 100;
    f.h.phases.Block(L"restore-begin");
    f.Start(o);
    CHECK(f.h.phases.Wait(L"online-edge") && f.h.phases.Entered(),
          "first online observation queues restore even without a prior offline edge");
    CHECK(f.h.backupBegins == 0 && Get(f.game + L"\\config.ini") == "GAME-INITIAL" &&
              Get(f.cache + L"\\config.ini") == "CACHE-INITIAL",
          "no reverse mirror runs before the startup restore writes Game");
    f.h.tick = 5000;
    f.h.phases.Release();
    CHECK(f.h.phases.Wait(L"restore-returned"), "startup restore completes through real worker");
    f.Settle();
    CHECK(Get(f.game + L"\\config.ini") == "CACHE-INITIAL" && f.h.restoreBegins == 1,
          "startup online establishes cache-to-Game direction first");
    Put(f.game + L"\\config.ini", "STARTUP-LIVE");
    SchedulerAt(f, 5099);
    CHECK(f.h.backupBegins == 0, "first online mirror waits a full period after restore completion");
    SchedulerAt(f, 5100);
    CHECK(f.h.backupBegins == 1 && Get(f.cache + L"\\config.ini") == "STARTUP-LIVE",
          "exactly one timed mirror follows successful startup restore");
}
static void Test_MatchAutomaticDisabled(bool schedulingDisabled) {
    DirectFixture f(schedulingDisabled ? L"match-scheduling-disabled" : L"match-auto-disabled");
    if (!schedulingDisabled) {
        auto cfg = Config::Load(f.root + L"\\install\\config.json");
        cfg.autoOn = false;
        CHECK(cfg.Save(f.root + L"\\install\\config.json"), "disabled auto flag is preconfigured before startup");
    }
    f.h.lobby = 1;
    auto o = f.h.MakeOptions(f.root);
    o.enableScheduling = !schedulingDisabled;
    f.Start(o);
    CHECK(f.h.phases.Wait(L"online-edge"), "disabled fixture observes startup online without restoring");
    CHECK(schedulingDisabled || !f.service.GetStatus().autoOn, "autoOff fixture really starts with auto disabled");
    f.h.game = 1;
    f.Settle();
    f.h.game = 0;
    f.Settle();
    CHECK(f.h.queued == 0 && f.h.begins == 0 && f.h.returned == 0 &&
              f.h.backupBegins == 0 && f.h.restores == 0,
          "autoOff or enableScheduling=false suppresses automatic match and periodic backups");
    f.h.lobby = 0;
    CHECK(f.h.phases.Wait(L"offline-edge"), "disabled fixture subsequently observes a real lobby shutdown");
    f.Settle();
    CHECK(f.h.cleanupQueued == 0 && Get(f.game + L"\\config.ini") == "GAME-INITIAL" &&
              Get(f.cache + L"\\config.ini") == "CACHE-INITIAL",
          "disabled automatic operation never cleans Game or mutates cache after a real match and shutdown");
}
static std::vector<json> ReadDiagnosticRecords(const std::wstring& path) {
    std::wstring raw, error;
    CHECK(Logger::TryReadRaw(path, raw, &error), "diagnostic JSONL is readable from the isolated user-data directory");
    std::vector<json> result;
    const auto text = WideToUtf8(raw);
    for (size_t first = 0; first < text.size();) {
        const auto last = text.find('\n', first);
        const auto line = text.substr(first, last == std::string::npos ? std::string::npos : last - first);
        if (!line.empty())
            result.push_back(json::parse(line));
        if (last == std::string::npos)
            break;
        first = last + 1;
    }
    return result;
}
static void Test_PeriodicDueSnapshotCannotBorrowNewSession() {
    DirectFixture f(L"periodic-dispatch-new-session-toctou");
    f.Launch(10000);
    Put(f.game + L"\\config.ini", "OLD-SESSION-LIVE-NOT-MIRRORED");
    f.h.phases.Block(L"periodic-before-dispatch");
    f.h.tick = 11000;
    f.service.PostCommand(++nextId, L"echo", L"{}");
    CHECK(f.h.phases.Entered() && f.h.backupBegins == 0,
          "worker holds an old session's due snapshot before atomic authority and token capture");
    f.h.lobby = 0;
    CHECK(f.h.phases.Wait(L"offline-edge", 2), "old due snapshot observes lobby exit while dispatch is held");
    Put(f.game + L"\\config.ini", "NEW-SESSION-UNRESTORED-GAME");
    f.h.lobby = 1;
    CHECK(f.h.phases.Wait(L"online-edge", 2), "new session queues launch restore before old due dispatch resumes");
    CHECK(Get(f.cache + L"\\config.ini") == "CACHE-INITIAL" && f.h.restoreBegins == 1,
          "held worker has not mirrored or restored either session yet");
    f.h.phases.Release();
    CHECK(f.h.phases.Wait(L"restore-returned", 2), "new session's frozen-cache restore completes after stale dispatch is rejected");
    f.Settle();
    CHECK(f.h.backupBegins == 0 && f.h.restoreBegins == 2 &&
              Get(f.cache + L"\\config.ini") == "CACHE-INITIAL" && Get(f.game + L"\\config.ini") == "CACHE-INITIAL",
          "old periodic due sample cannot capture new session tokens and mirror unrestored Game over frozen cache");
    Put(f.game + L"\\config.ini", "NEW-SESSION-RESTORED-LIVE");
    SchedulerAt(f, 20999);
    CHECK(f.h.backupBegins == 0, "stale due rejection cannot shorten new restore's full periodic window");
    SchedulerAt(f, 21000);
    CHECK(f.h.backupBegins == 1 && Get(f.cache + L"\\config.ini") == "NEW-SESSION-RESTORED-LIVE",
          "new session mirrors only after its own verified restore and complete interval");
}
static void Test_BehaviorDiagnosticChainAndQuietPolling() {
    testutil::Directory data(L"behavior-diagnostics");
    const auto root = data.path(), install = root + L"\\install", game = root + L"\\game",
               cache = root + L"\\cache", log = install + L"\\logs\\app.jsonl";
    const std::string secret = "PRIVATE-FILE-AND-ECHO-CONTENT-NOT-A-LOG-FIELD";
    Put(game + L"\\saves\\data.dat", secret);
    Config config;
    config.gameRoot = game;
    config.cacheRoot = cache;
    CHECK(fileops::EnsureDir(install) && config.Save(install + L"\\config.json"), "diagnostic fixture saves only isolated configuration");
    auto& service = Service::Instance();
    CHECK(service.StartWithOptions(nullptr, install, cache, Options(root)), "diagnostic fixture service starts");
    struct Stop { Service& service; ~Stop() { service.Stop(); } } stop{service};
    const int backup = ++nextId;
    service.PostCommand(backup, L"backup_now", L"{}");
    CHECK(Await(service, backup).value("ok", false), "diagnostic backup retains actual business success");
    const int setting = ++nextId;
    service.PostCommand(setting, L"save_settings", L"{\"sync_minutes\":7}");
    CHECK(Await(service, setting).value("ok", false), "diagnostic settings transaction retains success");
    const int cancel = ++nextId;
    service.PostCommand(cancel, L"cancel_op", L"{}");
    CHECK(Await(service, cancel).value("ok", false), "idle cancellation remains responsive with diagnostics enabled");
    CHECK(Call(service, L"echo").value("ok", false), "diagnostic worker barrier returns");
    auto records = ReadDiagnosticRecords(log);
    size_t backupRecords = 0, settingRecords = 0, cancelRecords = 0;
    bool metadata = !records.empty(), privateSafe = true, operationSeen = false;
    std::string run;
    unsigned long long previous = 0;
    for (const auto& record : records) {
        metadata = metadata && record.contains("pid") && record.contains("tid") && record.contains("seq") &&
                   record.contains("run_id") && record.contains("uptime_ms") && record.contains("trace") &&
                   record.contains("ts") && record.contains("module") && record.contains("msg");
        if (!record.contains("seq") || !record.contains("run_id"))
            continue;
        const auto currentRun = record.at("run_id").get<std::string>();
        const auto sequence = record.at("seq").get<unsigned long long>();
        metadata = metadata && !currentRun.empty() && (run.empty() || run == currentRun) && sequence > previous;
        run = currentRun;
        previous = sequence;
        const auto trace = record.value("trace", "");
        backupRecords += trace == "request:" + std::to_string(backup);
        settingRecords += trace == "request:" + std::to_string(setting);
        cancelRecords += trace == "request:" + std::to_string(cancel);
        const auto message = record.value("msg", "");
        operationSeen = operationSeen || (trace == "request:" + std::to_string(backup) &&
                                          (message.find("copied=") != std::string::npos ||
                                           message.find("operation=") != std::string::npos ||
                                           message.find("复制") != std::string::npos));
        privateSafe = privateSafe && record.dump().find(secret) == std::string::npos;
    }
    CHECK(metadata, "behavior records carry compatible metadata and one strictly ordered run identity");
    CHECK(backupRecords >= 3 && settingRecords >= 2 && cancelRecords >= 1,
          "backup/settings/cancel have correlated request chains rather than unrelated messages");
    CHECK(operationSeen && privateSafe, "backup result is traceable without logging business file contents");
    std::wstring before, after, ignored;
    CHECK(Logger::TryReadRaw(log, before, &ignored), "capture logs before status/log polling");
    bool polled = true;
    for (int i = 0; i < 20; ++i) {
        polled = polled && Call(service, L"get_status").value("ok", false);
        polled = polled && Call(service, L"get_logs", L"{\"limit\":20}").value("ok", false);
        polled = polled && CallJson(service, L"echo", {{"private_payload", secret}}).value("ok", false);
    }
    CHECK(polled && Call(service, L"echo").value("ok", false), "60 quiet read/echo calls remain responsive");
    CHECK(Logger::TryReadRaw(log, after, &ignored) && before == after &&
              WideToUtf8(after).find(secret) == std::string::npos,
          "status/log polling and echo do not self-generate logs or disclose payloads");
    service.Stop();
    records = ReadDiagnosticRecords(log);
    bool stopped = false;
    for (const auto& record : records) {
        const auto message = record.value("msg", "");
        stopped = stopped || message.find("STOP_COMPLETE") != std::string::npos ||
                  message.find("stop.complete") != std::string::npos || message.find("停止完成") != std::string::npos;
    }
    CHECK(stopped && Get(cache + L"\\saves\\data.dat") == secret,
          "shutdown summary survives close while business cache remains unchanged");
}
static void Test_StartupFailureIsLoggedBeforeConfigurationLoad() {
    testutil::Directory data(L"startup-failure-diagnostics");
    const auto root = data.path(), install = root + L"\\install";
    Put(install + L"\\config.json", "{invalid configuration");
    auto& service = Service::Instance();
    CHECK(!service.StartWithOptions(nullptr, install, root + L"\\cache", Options(root)),
          "invalid startup configuration is still rejected");
    service.Stop();
    const auto records = ReadDiagnosticRecords(install + L"\\logs\\app.jsonl");
    bool hasError = false;
    for (const auto& record : records)
        hasError = hasError || record.value("level", "") == "error";
    CHECK(hasError && Get(install + L"\\config.json") == "{invalid configuration",
          "startup failure is diagnosable without rewriting the invalid configuration");
}

static void Test_SchedulerChecksDoNotSpamBehaviorLogs() {
    testutil::Directory data(L"quiet-scheduler-diagnostics");
    const auto root = data.path(), install = root + L"\\install", log = install + L"\\logs\\app.jsonl";
    auto options = Options(root);
    options.enableScheduling = true;
    PhaseHarness phases;
    options.onPhase = [&](const std::wstring& phase) { phases.Observe(phase); };
    auto& service = Service::Instance();
    CHECK(service.StartWithOptions(nullptr, install, root + L"\\cache", options),
          "default-frequency scheduler diagnostics fixture starts without a configured Game");
    struct Stop { Service& service; ~Stop() { service.Stop(); } } stop{service};
    CHECK(phases.Wait(L"scheduler-check", 2) && Call(service, L"echo").value("ok", false),
          "scheduler checks actually execute before the baseline log snapshot");
    std::wstring before, after, ignored;
    CHECK(Logger::TryReadRaw(log, before, &ignored), "capture quiet scheduler baseline");
    CHECK(phases.Wait(L"scheduler-check", 5) && Call(service, L"echo").value("ok", false),
          "multiple normal scheduler ticks execute with identical gates");
    CHECK(Logger::TryReadRaw(log, after, &ignored) && before == after,
          "ordinary scheduler ticks and unchanged gate countdowns do not create diagnostic spam");
}

// Direct overwrite policy replaces 3.1.1 approval, match waiting and dirty/frozen state.
static void Test_InitialOnlineIgnoresGameProbe(int runningGame) {
    DirectFixture f(L"initial-online-no-game-veto");
    f.h.lobby = 1;
    f.h.game = runningGame;
    auto o = f.h.MakeOptions(f.root);
    o.syncPeriodMs = 10000;
    f.h.phases.Block(L"restore-begin");
    f.Start(o);
    CHECK(f.h.phases.Entered(), "initial online restores immediately even game1 or game unknown");
    CHECK(f.h.backupBegins == 0 && Get(f.game + L"\\config.ini") == "GAME-INITIAL",
          "restore direction is first and no periodic mirror overtakes the pending attempt");
    CheckAutoPolicy(f.service, false, false);
    f.h.tick = 5000;
    f.h.phases.Release();
    CHECK(f.h.phases.Wait(L"restore-returned"), "direct launch attempt completes without waiting for game exit");
    f.Settle();
    CHECK(Get(f.game + L"\\config.ini") == "CACHE-INITIAL" && f.h.restoreBegins == 1,
          "running or unknown game process never creates a deferred restore loop");
    CHECK(f.h.workerGameCalls == 0 && f.h.workerLobbyCalls == 0, "restore worker never invokes injected process probes");
    CheckAutoPolicy(f.service, true, false);
    CheckFullVirtualPeriod(f.service);
    Put(f.game + L"\\config.ini", "LIVE-AFTER-DIRECT-RESTORE");
    SchedulerAt(f, 14999);
    CHECK(f.h.backupBegins == 0, "interval starts after restore attempt finishes, not launch detection");
    SchedulerAt(f, 15000);
    CHECK(f.h.backupBegins == 1 && Get(f.cache + L"\\config.ini") == "LIVE-AFTER-DIRECT-RESTORE",
          "configured full mirror runs despite unchanged game1 or unknown probe");
}
static void Test_FailedRestoreStillEnablesNextTimedMirror(bool partial) {
    DirectFixture f(partial ? L"partial-restore-no-freeze" : L"failed-restore-no-freeze");
    const auto relative = partial ? L"\\hc.dat" : L"\\config.ini";
    if (partial) {
        Put(f.game + relative, "GAME-BEFORE-FAILURE");
        Put(f.cache + relative, "CACHE-BEFORE-FAILURE");
    }
    winutil::ScopedHandle locked(CreateFileW((f.game + relative).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(locked.valid(), "real Windows occupancy pins restore target against direct overwrite");
    f.h.lobby = 1;
    f.h.game = 1;
    auto o = f.h.MakeOptions(f.root);
    o.syncPeriodMs = 10000;
    f.h.phases.Block(partial ? L"restore/hc.dat" : L"restore/config.ini");
    f.Start(o);
    CHECK(f.h.phases.Entered(), "restore reaches locked target at deterministic per-file boundary");
    CHECK(f.h.backupBegins == 0, "no reverse writes while failed restore attempt is still in flight");
    if (partial)
        CHECK(Get(f.game + L"\\config.ini") == "CACHE-INITIAL", "first direct restore copy is visible before later file fails");
    f.h.tick = 8000;
    f.h.phases.Release();
    CHECK(f.h.phases.Wait(L"restore-returned"), "OS-denied restore attempt returns instead of entering a protected wait");
    f.Settle();
    const auto status = f.service.GetStatus();
    CHECK(!status.lastError.empty() && !f.service.IsBusy() && status.progressPercent == -1,
          "restore IO failure is reported and busy/progress admission is released");
    CHECK(Get(f.game + relative) == (partial ? "GAME-BEFORE-FAILURE" : "GAME-INITIAL"),
          "locked target failure respects Windows sharing; no OS bypass");
    CHECK(f.h.restores == 1, "failed restore does not spin or continually retry without a lifecycle event");
    CheckAutoPolicy(f.service, true, false);
    CheckFullVirtualPeriod(f.service);
    locked.reset();
    Put(f.game + L"\\config.ini", "LIVE-AFTER-FAILED-RESTORE");
    if (partial) Put(f.game + relative, "LIVE-PARTIAL-HC");
    SchedulerAt(f, 17999);
    CHECK(f.h.backupBegins == 0, "even failed restore waits a complete custom interval after its attempt");
    SchedulerAt(f, 18000);
    CHECK(f.h.backupBegins == 1 && Get(f.cache + L"\\config.ini") == "LIVE-AFTER-FAILED-RESTORE",
          "failed or partial restore never persistently freezes the next timed Game-to-cache mirror");
    if (partial)
        CHECK(Get(f.cache + relative) == "LIVE-PARTIAL-HC", "partial restore needs no approval before the next full mirror");
}
static void Test_FailedTimedMirrorCanRetryWithoutApproval() {
    DirectFixture f(L"timed-io-failure-no-frozen-mode");
    Put(f.cache + L"\\hc.dat", "CACHE-HC");
    f.Launch(10000);
    Put(f.game + L"\\config.ini", "FIRST-DIRECT-COMMIT");
    Put(f.game + L"\\hc.dat", "NEXT-HC");
    Put(f.cache + L"\\shards\\obsolete.dat", "DELETE-EVEN-IF-ONE-COPY-FAILS");
    winutil::ScopedHandle locked(CreateFileW((f.cache + L"\\hc.dat").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(locked.valid(), "pin cache target to inject real periodic IO failure");
    SchedulerAt(f, 11000);
    CHECK(f.h.backupBegins == 1 && !f.service.GetStatus().lastError.empty(), "periodic IO failure reaches worker result/status");
    CHECK(Get(f.cache + L"\\config.ini") == "FIRST-DIRECT-COMMIT" && Get(f.cache + L"\\hc.dat") == "CACHE-HC",
          "partial direct mirror retains committed first copy and respects locked second target");
    CHECK(!fileops::Exists(f.cache + L"\\shards\\obsolete.dat"), "copy IO failure does not protect unrelated stale managed files from full-mirror deletion");
    locked.reset();
    CheckAutoPolicy(f.service, true, false);
    Put(f.game + L"\\config.ini", "RETRY-WITHOUT-APPROVAL");
    SchedulerAt(f, 20999);
    CHECK(f.h.backupBegins == 1, "failed periodic attempt still keeps configured spacing");
    SchedulerAt(f, 21000);
    CHECK(f.h.backupBegins == 2 && Get(f.cache + L"\\config.ini") == "RETRY-WITHOUT-APPROVAL" &&
              Get(f.cache + L"\\hc.dat") == "NEXT-HC", "next timed mirror retries normally without manual rearming");
}
static void Test_EmptyFullGameClearsAllManagedCacheFiles() {
    DirectFixture f(L"empty-game-full-mirror-is-truth");
    SeedPersonalFiles(f.game);
    SeedPersonalFiles(f.cache);
    f.Launch(10000);
    CHECK(DeleteFileW((f.game + L"\\config.ini").c_str()), "fixture removes the last managed source file");
    Put(f.cache + L"\\saves\\nested\\one.dat", "STALE-SAVE");
    Put(f.cache + L"\\shards\\two.dat", "STALE-SHARD");
    Put(f.cache + L"\\bot_5.dll", "STALE-BOT");
    Put(f.cache + L"\\HID.dll", "STALE-HID");
    Put(f.cache + L"\\hc.dat", "STALE-HC");
    SchedulerAt(f, 11000);
    CHECK(f.h.backupBegins == 1 && f.service.GetStatus().lastError.empty(), "empty source full mirror is a successful intended operation");
    CHECK(!fileops::Exists(f.cache + L"\\config.ini") && !fileops::Exists(f.cache + L"\\hc.dat") &&
              !fileops::Exists(f.cache + L"\\bot_5.dll") && !fileops::Exists(f.cache + L"\\HID.dll") &&
              !fileops::Exists(f.cache + L"\\saves\\nested\\one.dat") && !fileops::Exists(f.cache + L"\\shards\\two.dat"),
          "empty full Game clears ALL managed cache groups, including absent directories and root whitelist files");
    CHECK(f.service.GetStatus().cacheEntries == 0 && PersonalFilesIntact(f.cache) && PersonalFilesIntact(f.game),
          "zero physical managed files are counted without touching unrelated selected-root data");
    f.h.lobby = 0;
    CHECK(f.h.phases.Wait(L"offline-edge", 2), "empty-cache cycle observes offline");
    f.h.lobby = 1;
    CHECK(f.h.phases.Wait(L"restore-returned", 2), "empty physical cache still completes next launch restore attempt");
    f.Settle();
    CheckAutoPolicy(f.service, true, false);
    CHECK(f.service.GetStatus().cacheEntries == 0, "empty restore does not resurrect historical metadata inventories");
}
static void Test_NoWorkerProbeLoopsForHundredFileRestore() {
    DirectFixture f(L"hundred-files-zero-worker-probes");
    CHECK(DeleteFileW((f.cache + L"\\config.ini").c_str()) && DeleteFileW((f.game + L"\\config.ini").c_str()),
          "hundred-file fixture removes the default seed pair");
    for (int i = 0; i < 100; ++i)
        Put(f.cache + L"\\saves\\file-" + std::to_wstring(i) + L".dat", "CACHE-" + std::to_string(i));
    auto o = f.h.MakeOptions(f.root);
    o.enableScheduling = false;
    f.h.phases.Block(L"offline-edge");
    f.Start(o);
    CHECK(f.h.phases.Entered(), "hold monitor at its first offline sample while real worker processes 100 files");
    const int lobbyCalls = f.h.lobbyProbeCalls.load(), gameCalls = f.h.gameProbeCalls.load();
    CHECK(lobbyCalls == 1 && gameCalls == 0, "production monitor samples lobby once, not game process");
    const auto restored = Call(f.service, L"restore_now");
    CHECK(restored.value("ok", false), "100-file restore runs while monitor is independently held");
    for (int i = 0; i < 100; ++i)
        CHECK(Get(f.game + L"\\saves\\file-" + std::to_wstring(i) + L".dat") == "CACHE-" + std::to_string(i),
              "every one of the 100 direct restore outputs has exact source bytes");
    CHECK(f.h.lobbyProbeCalls == lobbyCalls && f.h.gameProbeCalls == gameCalls &&
              f.h.workerGameCalls == 0 && f.h.workerLobbyCalls == 0,
          "RunOperation restores 100 files with ZERO worker lobby/game probes, not 1000 per-file probes");
    CHECK(Call(f.service, L"backup_now").value("ok", false), "100-file manual reverse copy also completes");
    CHECK(f.h.lobbyProbeCalls == lobbyCalls && f.h.gameProbeCalls == gameCalls,
          "reverse RunOperation also never calls process probes before or between files");
    CHECK(f.service.GetStatus().cacheEntries == 100, "physical cache count remains exactly 100 after reverse copy");
    f.h.phases.Release();
    f.Settle();
    CHECK(f.h.lobbyProbeCalls == f.h.samples && f.h.workerLobbyCalls == 0 && f.h.workerGameCalls == 0,
          "only subsequent production monitor intervals add probe calls");
}
// 0/1: monitor reports offline/unknown mid-copy; 2: rapid reopen; 3: offline just before pruning.
static void Test_MonitorLifecycleStopsAdditionalWrites(int mode) {
    DirectFixture f(L"direct-offline-inflight-lifecycle");
    Put(f.cache + L"\\hc.dat", "CACHED-HC");
    f.Launch(10000);
    Put(f.game + L"\\config.ini", "COMMITTED-BEFORE-EXIT");
    Put(f.game + L"\\hc.dat", "NOT-AFTER-EXIT");
    Put(f.cache + L"\\shards\\obsolete.dat", "KEEP-AFTER-EXIT");
    f.h.phases.Block(mode == 3 ? L"mirror/verified" : L"mirror/hc.dat");
    f.h.tick = 11000;
    f.service.PostCommand(++nextId, L"echo", L"{}");
    CHECK(f.h.phases.Entered(), "direct mirror pauses at a known copy or pre-delete lifecycle boundary");
    const auto atGate = Get(f.cache + L"\\hc.dat");
    CHECK(Get(f.cache + L"\\config.ini") == "COMMITTED-BEFORE-EXIT", "direct first copy is already visible without atomic-tree rollback");
    f.h.lobby = mode == 1 ? -1 : 0;
    if (mode == 1) {
        CHECK(f.h.SampleWindow() && !f.service.GetStatus().monitorKnown, "unknown is published by the monitor before worker resumes");
    } else {
        CHECK(f.h.phases.Wait(L"offline-edge", 2), "offline lifecycle invalidates active session before worker resumes");
    }
    if (mode == 2) {
        f.h.lobby = 1;
        CHECK(f.h.phases.Wait(L"online-edge", 2), "rapid new online lifecycle queues restore while old mirror is held");
    }
    f.h.phases.Release();
    CHECK(f.h.phases.Wait(L"backup-returned"), "invalidated automatic mirror returns promptly");
    f.Settle();
    CHECK(Get(f.cache + L"\\hc.dat") == atGate && Get(f.cache + L"\\shards\\obsolete.dat") == "KEEP-AFTER-EXIT",
          "observed offline/unknown stops all subsequent managed copies and deletes");
    CHECK(f.h.workerLobbyCalls <= 1 && f.h.workerGameCalls == 0 && f.h.cleanupQueued == 0,
          "lifecycle stop uses atomic state plus at most one full-mirror pre-delete lobby check, never per-file probes or Game cleanup");
    if (mode == 2) {
        CHECK(f.h.phases.Wait(L"restore-returned", 2) && Get(f.game + L"\\hc.dat") == "CACHED-HC",
              "old session token cannot revive on rapid reopen; new lifecycle restores current cache");
    } else {
        SchedulerAt(f, 90000);
        CHECK(f.h.backupBegins == 1, "offline/unknown overdue checks enqueue no additional mirror");
    }
}
static void Test_OnlineRootChangesRestoreImmediately() {
    DirectFixture f(L"online-new-game-and-cache-direct");
    f.Launch(10000);
    f.h.game = 1;
    const auto nextGame = f.root + L"\\next-game", nextCache = f.root + L"\\next-cache";
    Put(nextGame + L"\\config.ini", "NEW-GAME-OLD");
    Put(nextCache + L"\\config.ini", "NEW-CACHE");
    Put(f.game + L"\\config.ini", "OLD-GAME-MUST-STAY");
    SeedPersonalFiles(nextGame);
    CHECK(CallJson(f.service, L"set_game_root", {{"path", WideToUtf8(nextGame)}}).value("ok", false),
          "new selected Game commits while lobby remains online and game probe1");
    CHECK(f.h.phases.Wait(L"restore-returned", 2), "online Game root change immediately queues current cache restore");
    f.Settle();
    CHECK(Get(nextGame + L"\\config.ini") == "CACHE-INITIAL" && Get(f.game + L"\\config.ini") == "OLD-GAME-MUST-STAY" &&
              PersonalFilesIntact(nextGame) && f.h.offlineEdges == 1,
          "new selected Game is overwritten without offline-edge approval and old Game remains untouched");
    CHECK(CallJson(f.service, L"set_cache_root", {{"path", WideToUtf8(nextCache)}}).value("ok", false),
          "new selected physical cache commits online");
    CHECK(f.h.phases.Wait(L"restore-returned", 3), "online cache selection also immediately restores selected physical bytes");
    f.Settle();
    CHECK(Get(nextGame + L"\\config.ini") == "NEW-CACHE" && f.h.offlineEdges == 1 && f.h.backupBegins == 0,
          "online selections never enter wait_launch or requireOfflineEdge protection");
    CheckAutoPolicy(f.service, true, false);
    Put(nextGame + L"\\config.ini", "LIVE-NEW-ROOTS");
    SchedulerAt(f, 10999);
    CHECK(f.h.backupBegins == 0, "new roots receive a complete interval after direct restore");
    SchedulerAt(f, 11000);
    CHECK(Get(nextCache + L"\\config.ini") == "LIVE-NEW-ROOTS" && Get(f.cache + L"\\config.ini") == "CACHE-INITIAL",
          "only newly selected cache receives the next timed full mirror");
}
static void Test_QueuedOldRootRestoreCannotBorrowNewRoot() {
    DirectFixture f(L"queued-restore-root-token");
    f.Start(f.h.MakeOptions(f.root));
    CHECK(f.h.phases.Wait(L"offline-edge"), "queued old-root fixture observes offline");
    f.h.phases.Block(L"scheduler-check");
    CHECK(f.h.phases.Entered(), "hold worker before it can dequeue launch restore");
    f.h.lobby = 1;
    CHECK(f.h.phases.Wait(L"online-edge"), "old root launch restore is queued with immutable tokens");
    const auto nextGame = f.root + L"\\next-game";
    Put(nextGame + L"\\config.ini", "NEXT-GAME-OLD");
    const int setting = ++nextId;
    f.service.PostCommand(setting, L"set_game_root", Utf8ToWide(json{{"path", WideToUtf8(nextGame)}}.dump()));
    f.h.phases.Release();
    CHECK(Await(f.service, setting).value("ok", false), "root transaction commits before old pending launch can run");
    f.Settle();
    CHECK(WaitFor([&] { return Get(nextGame + L"\\config.ini") == "CACHE-INITIAL"; }, 3000),
          "fresh new-root lifecycle directly restores current cache");
    CHECK(Get(f.game + L"\\config.ini") == "GAME-INITIAL" && f.h.restoreBegins == 1,
          "old queued restore never targets old Game or launders old tokens into an extra new-root copy");
}
static void Test_AutoReenableNoApprovalAndGameDoesNotCreateJobs() {
    DirectFixture f(L"reenable-direct-no-match-policy");
    auto cfg = Config::Load(f.root + L"\\install\\config.json");
    cfg.autoOn = false;
    CHECK(cfg.Save(f.root + L"\\install\\config.json"), "direct enable fixture starts explicitly autoOff");
    f.h.lobby = 1;
    f.h.game = 1;
    auto o = f.h.MakeOptions(f.root);
    o.syncPeriodMs = 10000;
    f.Start(o);
    CHECK(f.h.phases.Wait(L"online-edge"), "autoOff still publishes initial online lifecycle");
    f.Settle();
    CHECK(f.h.restoreBegins == 0 && f.h.backupBegins == 0, "autoOff performs no automatic copies");
    ToggleAuto(f, true);
    CHECK(f.h.phases.Wait(L"restore-returned"), "enabling auto immediately restores online Game despite game1");
    f.Settle();
    CHECK(Get(f.game + L"\\config.ini") == "CACHE-INITIAL", "auto enable needs no approval baseline");
    for (int state : {0, -1, 1, 0}) {
        f.h.game = state;
        f.Settle();
        CHECK(f.h.backupBegins == 0 && f.h.restoreBegins == 1, "game changes alone never enqueue untimed mirrors or repeat restores");
    }
    Put(f.game + L"\\config.ini", "LIVE-AFTER-GAME-CHANGES");
    SchedulerAt(f, 11000);
    CHECK(f.h.backupBegins == 1 && Get(f.cache + L"\\config.ini") == "LIVE-AFTER-GAME-CHANGES",
          "only custom interval authorizes the reverse mirror");
}
// Legacy metadata is fixture input only, never authorization for the Service business path.
static void Test_LegacyMetadataCannotFreezeOrOverridePhysicalCache(int mode) {
    DirectFixture f(L"ignore-legacy-root-and-restore-guards");
    std::wstring error;
    rootstore::Record record;
    CHECK(rootstore::Resolve(f.root + L"\\install\\management-v1", f.cache, record, error),
          "construct an old-version external root record only as isolated legacy test input");
    auto lower = f.game;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    const auto digest = rootstore::Digest(WideToUtf8(lower), error);
    CHECK(!digest.empty() && !record.directory.empty(), "derive isolated legacy guard filename");
    const auto owner = record.directory + L"\\owner.json";
    // Direct Service preparation deliberately creates no metadata. This is legacy fixture input.
    if (!fileops::Exists(owner))
        Put(owner, json{{"magic", "LOLBackup managed root record"}, {"version", 1},
                        {"root", WideToUtf8(record.root)}, {"physicalIdentity", WideToUtf8(record.identity)},
                        {"scope", "managed-business-and-snapshot-store"}}.dump());
    CHECK(fileops::Exists(owner) && !Get(owner).empty(), "legacy fixture explicitly contains an owner record before unreadable/corrupt/changed tests");
    const auto guard = record.directory + L"\\restore-guard-v1-" + Utf8ToWide(digest) + L".json";
    Put(guard, json{{"magic", "BBSRestoreGuard"}, {"version", 1}, {"scope", "cache-to-game-restore"},
                    {"cache_identity", WideToUtf8(record.identity)}, {"game_root", WideToUtf8(f.game)},
                    {"dirty", true}, {"reason", "old partial restore must not freeze direct mode"}}.dump());
    const auto previous = f.root + L"\\previous-physical-cache";
    if (mode == 1) {
        Put(owner, "{corrupt owner record");
        Put(guard, "{corrupt restore guard");
    } else if (mode == 2) {
        CHECK(MoveFileW(f.cache.c_str(), previous.c_str()), "replace selected cache physical directory after old identity registration");
        Put(f.cache + L"\\config.ini", "NEW-PHYSICAL-CACHE");
    }
    const auto ownerBefore = Get(owner), guardBefore = Get(guard);
    winutil::ScopedHandle ownerLock, guardLock;
    if (mode == 0) {
        ownerLock.reset(CreateFileW(testutil::Native(owner).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        const DWORD ownerError = ownerLock.valid() ? ERROR_SUCCESS : GetLastError();
        guardLock.reset(CreateFileW(testutil::Native(guard).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        const DWORD guardError = guardLock.valid() ? ERROR_SUCCESS : GetLastError();
        if (!ownerLock.valid() || !guardLock.valid())
            printf("[debug] legacy lock owner Win32=%lu guard Win32=%lu\n", ownerError, guardError);
        CHECK(ownerLock.valid() && guardLock.valid(), "hold legacy owner/dirty guard unreadable for the entire Service operation");
        winutil::ScopedHandle deniedOwner(CreateFileW(testutil::Native(owner).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        const DWORD deniedOwnerError = deniedOwner.valid() ? ERROR_SUCCESS : GetLastError();
        winutil::ScopedHandle deniedGuard(CreateFileW(testutil::Native(guard).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        const DWORD deniedGuardError = deniedGuard.valid() ? ERROR_SUCCESS : GetLastError();
        CHECK(!deniedOwner.valid() && !deniedGuard.valid() && deniedOwnerError == ERROR_SHARING_VIOLATION &&
                  deniedGuardError == ERROR_SHARING_VIOLATION,
              "legacy owner and dirty guard are actually unreadable while direct Service work is tested");
    }
    f.h.lobby = 1;
    f.h.game = 1;
    auto o = f.h.MakeOptions(f.root);
    o.syncPeriodMs = 10000;
    f.Start(o);
    CHECK(f.h.phases.Wait(L"restore-returned"), "unreadable, corrupt or changed-identity metadata never blocks startup direct restore");
    f.Settle();
    CHECK(Get(f.game + L"\\config.ini") == (mode == 2 ? "NEW-PHYSICAL-CACHE" : "CACHE-INITIAL"),
          "physical current cache alone determines restore bytes; old guard dirty status is irrelevant");
    Put(f.cache + L"\\config.ini", "EXTERNAL-PHYSICAL-EDIT");
    Put(f.cache + L"\\saves\\external.dat", "EXTERNAL-NEW-FILE");
    CHECK(Call(f.service, L"restore_now").value("ok", false), "manual restore uses externally edited physical cache without reapproval");
    CHECK(Get(f.game + L"\\config.ini") == "EXTERNAL-PHYSICAL-EDIT" && Get(f.game + L"\\saves\\external.dat") == "EXTERNAL-NEW-FILE",
          "no hash manifest, absence history or root identity can substitute old bytes for current files");
    Put(f.game + L"\\config.ini", "TIMED-WRITE-IGNORES-LEGACY-METADATA");
    SchedulerAt(f, 11000);
    CHECK(Get(f.cache + L"\\config.ini") == "TIMED-WRITE-IGNORES-LEGACY-METADATA" && f.h.backupBegins == 1,
          "legacy metadata never creates persistent freeze or requires approval for reverse timed writes");
    ownerLock.reset();
    guardLock.reset();
    CHECK(Get(owner) == ownerBefore && Get(guard) == guardBefore, "business copies do not repair, rewrite or mark old metadata clean");
    if (mode == 2)
        CHECK(Get(previous + L"\\config.ini") == "CACHE-INITIAL", "old physical cache identity is not silently adopted or modified");
}
static void Test_CancelAndStopAtRealWorkerFileBoundary(bool stopService) {
    DirectFixture f(stopService ? L"stop-direct-worker" : L"cancel-direct-worker");
    auto o = f.h.MakeOptions(f.root);
    o.enableScheduling = false;
    f.Start(o);
    CHECK(f.h.phases.Wait(L"offline-edge"), "manual lifecycle fixture publishes offline before copying");
    Put(f.game + L"\\config.ini", "FIRST-COPY");
    Put(f.game + L"\\hc.dat", "SECOND-COPY");
    Put(f.cache + L"\\hc.dat", "OLD-SECOND");
    f.h.phases.Block(L"mirror/hc.dat");
    const int operation = ++nextId;
    f.service.PostCommand(operation, L"backup_now", L"{}");
    CHECK(f.h.phases.Entered() && f.service.IsBusy(), "active data job holds deterministic second-copy phase");
    CHECK(Error(Call(f.service, L"restore_now"), "E_BUSY"), "admission still serializes data jobs during direct IO");
    CHECK(Get(f.cache + L"\\config.ini") == "FIRST-COPY", "direct copy commits completed file before cancellation");
    if (stopService) {
        f.service.RequestStop();
        CHECK(!f.service.IsRunning(), "RequestStop is nonblocking while worker phase is held");
        CHECK(Error(Call(f.service, L"echo"), "E_STOPPED"), "stopping service responds instead of dropping new requests");
    } else {
        CHECK(Call(f.service, L"cancel_op").value("ok", false), "cancel is acknowledged out of the blocked IO queue");
    }
    f.h.phases.Release();
    if (stopService) f.service.Stop();
    CHECK(Error(Await(f.service, operation), "E_CANCELLED"), "stopped or explicitly cancelled operation reports cancelled token");
    CHECK(Get(f.cache + L"\\hc.dat") == "OLD-SECOND" && Get(f.game + L"\\hc.dat") == "SECOND-COPY" && !f.service.IsBusy(),
          "actual lifecycle cancellation stops additional files without reverting completed copies or clearing Game");
    CHECK(f.service.GetStatus().progressPercent == -1, "cancel/Stop releases progress and busy admission");
    if (stopService)
        CHECK(f.service.StartWithOptions(nullptr, f.root + L"\\install", f.cache, o), "same singleton restarts after stopped IO worker joins");
    CHECK(Call(f.service, L"backup_now").value("ok", false) && Get(f.cache + L"\\hc.dat") == "SECOND-COPY",
          "fresh command captures current lifecycle token; prior cancellation never poisons future work");
}
static void Test_ClearIsExplicitWhitelistOperationWithLifecycleCancellation() {
    DirectFixture f(L"explicit-clear-no-process-guard");
    auto o = f.h.MakeOptions(f.root);
    o.enableScheduling = false;
    f.h.lobby = 1;
    f.h.game = 1;
    f.Start(o);
    CHECK(f.h.phases.Wait(L"online-edge"), "explicit clear fixture remains online without automatic jobs");
    SeedPersonalFiles(f.game);
    Put(f.game + L"\\hc.dat", "HC");
    Put(f.game + L"\\saves\\nested.dat", "SAVE");
    Put(f.game + L"\\shards\\nested.dat", "SHARD");
    Put(f.game + L"\\HID.dll", "HID");
    CHECK(Error(Call(f.service, L"clear_game_config", L"{"), "E_INPUT"), "malformed clear remains strict input validation");
    f.h.phases.Block(L"clear/hc.dat");
    const int clearing = ++nextId;
    f.service.PostCommand(clearing, L"clear_game_config", L"{}");
    CHECK(f.h.phases.Entered(), "explicit online clear reaches a managed deletion without game/lobby approval");
    CHECK(Call(f.service, L"cancel_op").value("ok", false), "explicit clear still honors actual user lifecycle cancellation");
    f.h.phases.Release();
    CHECK(Error(Await(f.service, clearing), "E_CANCELLED"), "cancelled clear reports cancelled instead of a protected mode");
    CHECK(Get(f.game + L"\\hc.dat") == "HC" && Get(f.cache + L"\\config.ini") == "CACHE-INITIAL",
          "cancel stops next deletion and explicit clear never touches cache");
    CHECK(Call(f.service, L"clear_game_config").value("ok", false), "fresh explicit clear succeeds while lobby/game remain online");
    CHECK(!fileops::Exists(f.game + L"\\hc.dat") && !fileops::Exists(f.game + L"\\HID.dll") &&
              !fileops::Exists(f.game + L"\\saves\\nested.dat") && !fileops::Exists(f.game + L"\\shards\\nested.dat"),
          "clear deletes only selected Game whitelist files and managed subtrees");
    CHECK(PersonalFilesIntact(f.game) && Get(f.cache + L"\\config.ini") == "CACHE-INITIAL", "clear preserves all unrelated files and current cache");
}
// Registry is redirected for this process to a uniquely-created volatile test key, never real Run.
class IsolatedUserRegistry {
    HKEY original = nullptr, isolated = nullptr;
    std::wstring name;
    bool redirected = false;
  public:
    IsolatedUserRegistry() {
        if (RegOpenCurrentUser(KEY_ALL_ACCESS, &original) != ERROR_SUCCESS)
            throw std::runtime_error("cannot open original HKCU for isolated registry fixture");
        name = L"Software\\BBS-ServiceTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        DWORD disposition = 0;
        if (RegCreateKeyExW(original, name.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr,
                            &isolated, &disposition) != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
            RegCloseKey(original);
            original = nullptr;
            throw std::runtime_error("cannot create exclusive isolated registry key");
        }
        redirected = RegOverridePredefKey(HKEY_CURRENT_USER, isolated) == ERROR_SUCCESS;
        if (!redirected) {
            RegCloseKey(isolated);
            isolated = nullptr;
            RegDeleteTreeW(original, name.c_str());
            RegCloseKey(original);
            original = nullptr;
            throw std::runtime_error("cannot redirect HKCU; real Run key will not be touched");
        }
    }
    ~IsolatedUserRegistry() {
        if (redirected) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (isolated) RegCloseKey(isolated);
        if (original) {
            RegDeleteTreeW(original, name.c_str());
            RegCloseKey(original);
        }
    }
};
static void Test_RunKeyRollbackWhenConfigurationCommitFails() {
    testutil::Directory data(L"runkey-config-rollback");
    IsolatedUserRegistry registry;
    const auto root = data.path(), config = root + L"\\install\\config.json";
    // LoadSafe on a fresh install is intentionally read-only: seed the existing replacement target.
    Config initial;
    initial.cacheRoot = root + L"\\cache";
    CHECK(fileops::EnsureDir(root + L"\\install") && initial.Save(config),
          "persist initial rollback fixture configuration before testing replacement failure");
    auto& s = Service::Instance();
    auto o = Options(root);
    o.allowAutostart = true;
    CHECK(s.StartWithOptions(nullptr, root + L"\\install", root + L"\\cache", o), "isolated Run-key rollback service starts");
    struct Stop { Service& s; ~Stop() { s.Stop(); } } stop{s};
    HKEY run = nullptr;
    // Descendants of a volatile registry parent must also be created volatile (Win32 1021 otherwise).
    const LONG createRun = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
                                          REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &run, nullptr);
    if (createRun != ERROR_SUCCESS)
        printf("[debug] redirected volatile Run creation Win32=%ld\n", createRun);
    CHECK(createRun == ERROR_SUCCESS, "create Run only inside redirected volatile registry");
    if (!run) return;
    struct Close { HKEY key; ~Close() { RegCloseKey(key); } } close{run};
    const BYTE original[] = {0x42, 0x00, 0xff, 0x7f, 0x00};
    CHECK(RegSetValueExW(run, L"LOLBackupV3", 0, REG_BINARY, original, sizeof(original)) == ERROR_SUCCESS,
          "seed binary Run value to prove rollback preserves original type and exact bytes");
    const auto bytes = Get(config);
    const auto status = s.GetStatus();
    winutil::ScopedHandle locked(CreateFileW(testutil::Native(config).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!locked.valid()) printf("[debug] rollback configuration lock Win32=%lu\n", GetLastError());
    CHECK(locked.valid(), "pin configuration replacement after Run-key update");
    CHECK(Error(Call(s, L"save_settings", L"{\"autostart\":true,\"theme\":1}"), "E_CONFIG"), "failed configuration commit is reported after attempted autostart change");
    BYTE restored[32]{};
    DWORD type = 0, size = sizeof(restored);
    CHECK(RegQueryValueExW(run, L"LOLBackupV3", nullptr, &type, restored, &size) == ERROR_SUCCESS &&
              type == REG_BINARY && size == sizeof(original) && std::equal(original, original + sizeof(original), restored),
          "configuration failure rolls Run value back with original type and exact nonstring bytes");
    CHECK(Get(config) == bytes && SameSettings(status, s.GetStatus()), "failed registry/config transaction publishes no partial settings");
    locked.reset();
    CHECK(Call(s, L"save_settings", L"{\"autostart\":true,\"theme\":1}").value("ok", false), "fresh transaction commits after persistence failure is removed");
    Config committed;
    CHECK(Config::LoadSafe(config, committed) && committed.autostart && committed.theme == 1 && s.GetStatus().autostart,
          "autostart configuration and live settings commit together after retry");
}
static void Test_StatusProgressEventsAndResponseCounts() {
    DirectFixture f(L"direct-status-progress-events");
    auto o = f.h.MakeOptions(f.root);
    o.enableScheduling = false;
    f.Start(o);
    CHECK(f.h.phases.Wait(L"offline-edge"), "status fixture monitor is initialized");
    CHECK(Call(f.service, L"echo").value("ok", false), "drain bootstrap events before observing a data operation");
    observedEvents.clear();
    f.h.phases.Block(L"mirror/config.ini");
    const int id = ++nextId;
    f.service.PostCommand(id, L"backup_now", L"{}");
    CHECK(f.h.phases.Entered(), "status fixture holds actual direct file copy");
    const auto during = f.service.GetStatus();
    CHECK(during.backupState == L"running" && during.operationId > 0 && during.progressPercent >= 0 && f.service.IsBusy(),
          "active operation exposes real running/busy/id/progress status");
    Collect(f.service);
    f.h.phases.Release();
    const auto result = Await(f.service, id);
    CHECK(result.value("ok", false) && result.contains("data") && result["data"].value("copied", -1) == 1,
          "direct response reports exactly one copied managed file");
    CHECK(Call(f.service, L"echo").value("ok", false), "drain operation completion events");
    bool running = false, ready = false, progress = false;
    for (const auto& event : observedEvents) {
        if (!event.contains("data") || !event["data"].is_object()) continue;
        const auto& data = event["data"];
        if (event.value("event", "") == "status") {
            running = running || data.value("backup_state", "") == "running";
            ready = ready || (data.value("backup_state", "") == "ready" && data.value("progress_percent", 0) == -1);
        }
        if (event.value("event", "") == "progress") {
            const auto percent = data.value("percent", -1);
            progress = progress || (percent >= 0 && percent <= 100 && data.value("total", 0) > 0);
        }
    }
    CHECK(running && ready && progress, "actual operation emits running status, bounded progress and final ready status");
    const auto response = Call(f.service, L"get_status");
    CHECK(response.value("ok", false) && response["data"].value("cache_entries", -1) == 1 &&
              !response["data"].value("match_backup_ready", true) && !response["data"].value("auto_reason", "").empty(),
          "status command reports physical count and explicit direct-policy readiness/reason");
    CHECK(f.service.GetStatus().operationId == during.operationId && !f.service.IsBusy(), "operation identity stays observable after completion without retaining busy state");
}
static void Test_OfflineEmptyGameNeverChangesCache() {
    DirectFixture f(L"offline-empty-game-keeps-all-cache");
    f.Launch(10000);
    SeedPersonalFiles(f.cache);
    const std::vector<std::wstring> files = {L"config.ini", L"hc.dat", L"bot_5.dll", L"HID.dll",
                                             L"saves\\nested\\keep.dat", L"shards\\keep.dat"};
    std::map<std::wstring, std::string> before;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto bytes = std::string("OFFLINE-CACHE-") + std::to_string(i);
        Put(f.cache + L"\\" + files[i], bytes);
        before[files[i]] = bytes;
    }
    f.h.lobby = 0;
    CHECK(f.h.phases.Wait(L"offline-edge", 2), "monitor confirms offline before Game becomes empty");
    CHECK(DeleteFileW((f.game + L"\\config.ini").c_str()), "Game becomes completely managed-empty only after offline");
    SchedulerAt(f, 50000);
    SchedulerAt(f, 90000);
    f.Settle();
    CHECK(f.h.backupBegins == 0 && f.h.restoreBegins == 1 && !f.service.IsBusy(),
          "overdue offline checks never start copies or deletions even for completely empty Game");
    for (const auto& file : before)
        CHECK(Get(f.cache + L"\\" + file.first) == file.second,
              "every managed cache file remains byte-identical while offline Game is empty");
    CHECK(RawChildrenEqual(f.cache, {L"config.ini", L"hc.dat", L"bot_5.dll", L"HID.dll", L"saves", L"shards", L"personal.txt", L"projects"}) &&
              RawChildrenEqual(f.cache + L"\\saves\\nested", {L"keep.dat"}) &&
              RawChildrenEqual(f.cache + L"\\shards", {L"keep.dat"}),
          "offline empty Game never creates, replaces or deletes managed cache paths");
    CHECK(PersonalFilesIntact(f.cache) && !fileops::Exists(f.game + L"\\config.ini") && f.h.cleanupQueued == 0,
          "offline cache and unrelated data stay intact without automatic Game cleanup or restore");
    CheckAutoPolicy(f.service, false, false);
}
static void Test_FreshPreDeleteLobbyVetoPreventsEmptyGameWipe(int freshLobby) {
    DirectFixture f(L"empty-game-fresh-predelete-offline-veto");
    f.Launch(10000);
    CHECK(DeleteFileW((f.game + L"\\config.ini").c_str()), "online mirror starts with a completely empty managed Game");
    Put(f.cache + L"\\saves\\keep.dat", "KEEP-SAVE");
    Put(f.cache + L"\\hc.dat", "KEEP-HC");
    f.h.workerLobbyObservation = freshLobby;
    f.h.phases.Block(L"mirror/verified");
    f.h.tick = 11000;
    f.service.PostCommand(++nextId, L"echo", L"{}");
    CHECK(f.h.phases.Entered(), "empty full mirror reaches deterministic pre-delete phase before pruning");
    CHECK(f.service.GetStatus().lobbyOnline && f.service.GetStatus().monitorKnown && f.h.workerLobbyCalls == 0,
          "monitor still caches known-online while no fresh deletion-boundary probe has run yet");
    f.h.phases.Release();
    CHECK(f.h.phases.Wait(L"backup-returned") && Call(f.service, L"echo").value("ok", false),
          "fresh offline/unknown pre-delete veto returns through the actual worker");
    CHECK(f.h.workerLobbyCalls == 1 && f.h.workerGameCalls == 0,
          "full mirror makes exactly ONE fresh lobby probe at pre-delete, never per-file/game probes");
    CHECK(Get(f.cache + L"\\config.ini") == "CACHE-INITIAL" && Get(f.cache + L"\\hc.dat") == "KEEP-HC" &&
              Get(f.cache + L"\\saves\\keep.dat") == "KEEP-SAVE",
          "fresh offline or unknown result forbids deleting any cache bytes despite stale online monitor state");
    CHECK(!fileops::Exists(f.game + L"\\config.ini") && f.h.backupBegins == 1 && !f.service.IsBusy(),
          "pre-delete veto never restores historical bytes or starts additional copies");
}
int main() {
    try {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        Test_MatchStartupOfflineIsNotAnEnd();
        Test_AutoRestoreMixedRootIgnoresUnrelatedEntries();
        Test_StartupOnlineRestoresBeforePeriodic();
        Test_InitialOnlineIgnoresGameProbe(1);
        Test_InitialOnlineIgnoresGameProbe(-1);
        Test_NoWorkerProbeLoopsForHundredFileRestore();
        Test_FailedRestoreStillEnablesNextTimedMirror(false);
        Test_FailedRestoreStillEnablesNextTimedMirror(true);
        Test_FailedTimedMirrorCanRetryWithoutApproval();
        Test_EmptyFullGameClearsAllManagedCacheFiles();
        Test_OfflineEmptyGameNeverChangesCache();
        Test_FreshPreDeleteLobbyVetoPreventsEmptyGameWipe(0);
        Test_FreshPreDeleteLobbyVetoPreventsEmptyGameWipe(-1);
        Test_LobbyCycle();
        Test_CustomSyncMinutesControlsRealScheduler();
        Test_SingleOfflineSampleReopenRestoresFrozenCache();
        Test_UnknownLobbyStatusNeverCountsDown();
        Test_QueuedAutoRestoreRetainsCancellationToken();
        Test_PeriodicDueSnapshotCannotBorrowNewSession();
        for (int mode = 0; mode < 4; ++mode) Test_MonitorLifecycleStopsAdditionalWrites(mode);
        Test_OnlineRootChangesRestoreImmediately();
        Test_QueuedOldRootRestoreCannotBorrowNewRoot();
        Test_AutoReenableNoApprovalAndGameDoesNotCreateJobs();
        Test_MatchAutomaticDisabled(false);
        Test_MatchAutomaticDisabled(true);
        for (int mode = 0; mode < 3; ++mode) Test_LegacyMetadataCannotFreezeOrOverridePhysicalCache(mode);
        Test_CancelAndStopAtRealWorkerFileBoundary(false);
        Test_CancelAndStopAtRealWorkerFileBoundary(true);
        Test_ClearIsExplicitWhitelistOperationWithLifecycleCancellation();
        Test_DirectManualRestoreAndCopyOnlyBackup();
        Test_StatusProgressEventsAndResponseCounts();
        Test_RootDependencyCASRefusesStaleValidation();
        Test_RunKeyRollbackWhenConfigurationCommitFails();
        Test_Transactions();
        Test_CorruptStartup();
        Test_ImportWithoutGameAndCancellationEpoch();
        Test_BoundedRealLogResponses();
        Test_BehaviorDiagnosticChainAndQuietPolling();
        Test_StartupFailureIsLoggedBeforeConfigurationLoad();
        Test_SchedulerChecksDoNotSpamBehaviorLogs();
        Test_AdoptBusinessCacheFolder();
        Test_StartupKeepsConfiguredNestedCache();
        Test_StartupClaimsBusinessCache();
        Test_ForeignGameDetection();
        printf("\n%d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] Unhandled test fixture/implementation exception: %s\n", error.what());
        Service::Instance().Stop();
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[FAIL] Unknown test fixture/implementation exception\n");
        Service::Instance().Stop();
        return 1;
    }
}

