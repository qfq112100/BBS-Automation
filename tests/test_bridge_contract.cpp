// JSON command contract, exercised through the real isolated Service worker.
#include "../src/common/utf.h"
#include "../src/core/config.h"
#include "../src/core/file_ops.h"
#include "../src/core/service.h"
#include "test_isolation.h"
#include <cstdio>
#include <exception>
#include <json.hpp>
#include <string>
#include <vector>
using namespace core;
using json = nlohmann::json;
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                                                                                    \
    do {                                                                                                     \
        if (cond) {                                                                                          \
            ++g_pass;                                                                                        \
            printf("  [PASS] %s\n", name);                                                                   \
        } else {                                                                                             \
            ++g_fail;                                                                                        \
            printf("  [FAIL] %s\n", name);                                                                   \
        }                                                                                                    \
    } while (0)
static std::string U8(const std::wstring& s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0, nullptr,
                                nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}
static std::wstring Wide(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &out[0], n);
    return out;
}
static json Call(Service& svc, int id, const wchar_t* method, const std::wstring& params) {
    svc.PostCommand(id, method, params);
    const auto deadline = GetTickCount64() + 5000;
    do {
        for (const auto& e : svc.DrainEvents()) {
            auto j = json::parse(U8(e.json));
            if (j.contains("id") && j["id"] == id)
                return j;
        }
        Sleep(5);
    } while (GetTickCount64() < deadline);
    CHECK(false, "Service returns the matching request ID before timeout");
    return json::object();
}
int main() {
    try {
        testutil::Directory isolated(L"bridge");
        const auto root = isolated.path();
        fileops::EnsureDir(root + L"\\install");
        fileops::EnsureDir(root + L"\\cache");
        fileops::EnsureDir(root + L"\\game 空格\\Game");
        Service& svc = Service::Instance();
        Service::StartOptions opt;
        opt.allowAutostart = false;
        opt.enableMonitoring = false;
        opt.enableScheduling = false;
        opt.enableAutodetect = false;
        opt.validateGameRoot = false;
        CHECK(svc.StartWithOptions(nullptr, root + L"\\install", root + L"\\cache", opt),
              "isolated service starts");
        struct StopGuard {
            Service& s;
            ~StopGuard() {
                s.Stop();
            }
        } stop{svc};
        svc.DrainEvents();
        const auto cfg = root + L"\\install\\config.json";
        const auto game = root + L"\\game 空格\\Game";
        auto r = Call(svc, 101, L"set_game_root", Wide(json{{"path", U8(game)}}.dump()));
        CHECK(r.value("ok", false), "set_game_root reports success");
        CHECK(Config::Load(cfg).gameRoot == game, "escaped backslashes and Unicode survive JSON persistence");
        CHECK(svc.GetStatus().gameRoot == game, "game_root status equals persisted value");
        r = Call(svc, 102, L"save_settings", L"{\"sync_minutes\":30}");
        CHECK(r.value("ok", false), "save_settings reports success");
        CHECK(Config::Load(cfg).syncMinutes == 30, "sync_minutes=30 persists exactly");
        CHECK(svc.GetStatus().syncMinutes == 30, "sync_minutes applies to live status");
        r = Call(svc, 103, L"save_settings", L"{\"autostart\":true}");
        CHECK(r.value("ok", false), "test-mode autostart preference can be saved without registry access");
        r = Call(svc, 104, L"save_settings", L"{\"close_action\":1}");
        CHECK(r.value("ok", false), "partial close_action update succeeds");
        const auto c = Config::Load(cfg);
        CHECK(c.autostart, "partial settings preserve autostart");
        CHECK(c.closeAction == 1, "close_action persists");
        r = Call(svc, 105, L"set_auto", L"{\"enabled\":false}");
        CHECK(r.value("ok", false) && !svc.GetStatus().autoOn, "set_auto(false) applies");
        r = Call(svc, 106, L"get_status", L"{}");
        CHECK(r.value("ok", false) && r.contains("data"), "get_status returns data");
        const char* required[] = {"installed",     "auto_on",      "lobby_online",  "backup_state",
                                  "monitor_state", "next_sync",    "last_sync",     "last_result",
                                  "game_root",     "game_ok",      "cache_entries", "theme",
                                  "autostart",     "close_action", "sync_minutes"};
        bool all = r.contains("data");
        for (auto k : required)
            if (!r.contains("data") || !r["data"].contains(k)) {
                all = false;
                printf("missing status field: %s\n", k);
            }
        CHECK(all, "get_status retains all 15 command-contract fields");
        r = Call(svc, 107, L"set_game_root", L"{ this is not json");
        CHECK(r.contains("ok") && !r["ok"].get<bool>(), "malformed JSON returns error");
        r = Call(svc, 108, L"no_such_method", L"{}");
        CHECK(!r.value("ok", true) && r.contains("error") && r["error"].value("code", "") == "E_UNKNOWN",
              "unknown method returns E_UNKNOWN exactly");
        printf("\n=== results: %d passed, %d failed ===\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] Unhandled test fixture/implementation exception: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[FAIL] Unknown test fixture/implementation exception\n");
        return 1;
    }
}
