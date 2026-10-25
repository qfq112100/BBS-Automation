// test_core_bugs.cpp — 核心层 BUG 复现测试 (RED 用例)
// 编译: g++ -std=c++17 -O1 test_core_bugs.cpp ../src/core/config.cpp ../src/core/file_ops.cpp
// ../src/core/logger.cpp -I../src/core -I../third_party/json -o test_core_bugs.exe
#include "../src/core/config.h"
#include "../src/core/file_ops.h"
#include "../src/core/logger.h"
#include "../src/core/service.h"
#include "../src/core/sync_engine.h"
#include "test_isolation.h"
#include "test_fixture.h"
#include <cstdio>
#include <exception>
#include <string>
#include <vector>
#define JSON_DIAGNOSTICS 0
#include <json.hpp>

using namespace core;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                                                                                    \
    do {                                                                                                     \
        if (cond) {                                                                                          \
            printf("  [PASS] %s\n", name);                                                                   \
            g_pass++;                                                                                        \
        } else {                                                                                             \
            printf("  [FAIL] %s\n", name);                                                                   \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

static void WriteUtf8(const std::wstring& path, const std::string& s) {
    fileops::EnsureDir(path.substr(0, path.find_last_of(L"\\/")));
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, s.data(), (DWORD)s.size(), &w, NULL);
        CloseHandle(f);
    }
}
static std::string ReadAll(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return "";
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    std::string s;
    if (sz.QuadPart > 0 && sz.QuadPart < 1024 * 1024) {
        s.resize((size_t)sz.QuadPart);
        DWORD r = 0;
        ReadFile(f, &s[0], (DWORD)s.size(), &r, NULL);
        s.resize(r);
    }
    CloseHandle(f);
    return s;
}
int main() {
    try {
        testutil::Directory isolated(L"test_core_bugs");
        std::wstring root = isolated.path();
        testutil::ResetOwnedDirectory(root);
        fileops::EnsureDir(root);

        printf("=== BUG-1: invalid existing fields fail closed without replacing caller state ===\n");
        {
            const auto p = root + L"\\b1\\config.json";
            const std::string raw = "{\"game_root\":\"C:/Riot/Game\",\"theme\":\"light\",\"auto_on\":true}";
            WriteUtf8(p, raw);
            Config c;
            c.gameRoot = L"sentinel-game";
            c.theme = 1;
            c.syncMinutes = 77;
            c.autoOn = false;
            CHECK(!Config::LoadSafe(p, c),
                  "type-invalid configuration is explicitly refused, not partly accepted");
            CHECK(c.gameRoot == L"sentinel-game" && c.theme == 1 && c.syncMinutes == 77 && !c.autoOn,
                  "failed LoadSafe leaves the entire caller state unchanged");
            CHECK(!Config::LastError().empty() && ReadAll(p) == raw,
                  "load failure has a diagnostic and preserves exact configuration bytes");
        }
        printf("\n=== BUG-2: valid missing-field legacy migration preserves known and unknown fields ===\n");
        {
            const auto p = root + L"\\b2\\config.json";
            const std::string raw =
                "{\"game_root\":\"C:/Riot/Game\",\"theme\":1,\"vendor\":{\"keep\":true},\"custom_id\":[1,2]}";
            WriteUtf8(p, raw);
            CHECK(Config::IsContentSafe(raw),
                  "missing known fields are recognized as a valid legacy configuration");
            Config c;
            CHECK(Config::LoadSafe(p, c) && c.gameRoot == L"C:/Riot/Game" && c.theme == 1 &&
                      c.syncMinutes == 5,
                  "legacy migration retains specified values and initializes only missing fields");
            CHECK(ReadAll(p) == raw, "reading legacy configuration never rewrites it implicitly");
            c.closeAction = 1;
            CHECK(c.Save(p), "explicit migrated configuration save succeeds");
            const auto after = nlohmann::json::parse(ReadAll(p));
            CHECK(after["game_root"] == "C:/Riot/Game" && after["theme"] == 1 && after["close_action"] == 1,
                  "migration cannot silently replace user fields with defaults");
            CHECK(after["vendor"] == nlohmann::json({{"keep", true}}) &&
                      after["custom_id"] == nlohmann::json({1, 2}),
                  "nested unknown fields survive real Parse/Save migration intact");
        }
        printf(
            "\n=== BUG-3: real atomic update refuses corrupt configuration before invoking a mutator ===\n");
        {
            const auto p = root + L"\\b3\\config.json";
            const std::string raw = "{\"game_root\":\"C:/Riot/Game\",\"theme\":\"light\"}";
            WriteUtf8(p, raw);
            int called = 0;
            auto mutate = [](Config& c, void* context) -> bool {
                ++*static_cast<int*>(context);
                c.autoOn = false;
                return true;
            };
            CHECK(!Config::UpdateAtomic(p, mutate, &called),
                  "atomic update returns failure for invalid persisted types");
            CHECK(called == 0 && ReadAll(p) == raw,
                  "corrupt config bytes remain exact and mutator is never executed");
        }
        printf("\n=== BUG-4: real startup refuses corrupt configuration before legacy discovery/migration "
               "===\n");
        {
            const auto install = root + L"\\b4", p = install + L"\\config.json";
            const std::string raw = "{\"game_root\":\"C:/Riot/Game\",\"theme\":\"light\"}";
            WriteUtf8(p, raw);
            WriteUtf8(install + L"\\game_root.txt", "C:/Legacy/Game");
            Service::StartOptions opt;
            opt.allowAutostart = false;
            opt.enableMonitoring = false;
            opt.enableScheduling = false;
            opt.enableAutodetect = false;
            opt.validateGameRoot = false;
            auto& service = Service::Instance();
            const bool started = service.StartWithOptions(nullptr, install, root + L"\\b4-cache", opt);
            CHECK(!started && !service.IsRunning(), "real Service cannot start over invalid configuration");
            CHECK(ReadAll(p) == raw && !service.GetStatus().lastError.empty(),
                  "startup leaves original bad file exact and reports repair-needed diagnostic");
            service.Stop();
        }
        printf("\n=== BUG-5: out-of-range settings are rejected rather than clamped or silently defaulted "
               "===\n");
        {
            const auto p = root + L"\\b5\\config.json";
            for (const auto& raw :
                 {std::string("{\"sync_minutes\":9999}"), std::string("{\"sync_minutes\":0}")}) {
                WriteUtf8(p, raw);
                Config c;
                c.syncMinutes = 77;
                c.gameRoot = L"unchanged";
                CHECK(!Config::IsContentSafe(raw) && !Config::LoadSafe(p, c),
                      "both excessive and zero minute values are invalid");
                CHECK(c.syncMinutes == 77 && c.gameRoot == L"unchanged" && ReadAll(p) == raw,
                      "invalid range cannot mutate caller output or disk bytes");
            }
        }

        printf("\n=== BUG-7: MirrorDir 目标文件只读时复制失败 (无属性清除) ===\n");
        {
            std::wstring src = root + L"\\b7\\src";
            std::wstring dst = root + L"\\b7\\dst";
            fileops::EnsureDir(src);
            fileops::EnsureDir(dst);
            WriteUtf8(src + L"\\a.txt", "new");
            WriteUtf8(dst + L"\\a.txt", "old");
            SetFileAttributesW((dst + L"\\a.txt").c_str(), FILE_ATTRIBUTE_READONLY);
            bool ok = fileops::CopyFileForce(src + L"\\a.txt", dst + L"\\a.txt");
            std::string got = ReadAll(dst + L"\\a.txt");
            printf("  覆盖只读目标文件 CopyFileForce=%s 内容=[%s]\n", ok ? "true" : "false", got.c_str());
            CHECK(
                (ok && got == "new") ||
                    (!ok && got == "old" &&
                     GetFileAttributesW((dst + L"\\a.txt").c_str()) != INVALID_FILE_ATTRIBUTES),
                "read-only destination either commits whole new content or safely retains whole old content");
        }

        printf("\n=== BUG-8: ListFiles 相对路径在 dir 带尾反斜杠时错位 ===\n");
        {
            std::wstring d = root + L"\\b8\\d";
            fileops::EnsureDir(d);
            WriteUtf8(d + L"\\f.txt", "x");
            auto f1 = fileops::ListFiles(d);
            auto f2 = fileops::ListFiles(d + L"\\");
            printf("  ListFiles(\"...\\\\d\")      -> [%ls]\n", f1.empty() ? L"(空)" : f1[0].c_str());
            printf("  ListFiles(\"...\\\\d\\\\\")   -> [%ls]\n", f2.empty() ? L"(空)" : f2[0].c_str());
            CHECK(!f2.empty() && f2[0] == L"f.txt", "尾反斜杠目录返回正确相对路径 (期望: f.txt)");
        }

        printf("\n=== 结果: %d passed, %d failed ===\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] Unhandled test fixture/implementation exception: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[FAIL] Unknown test fixture/implementation exception\n");
        return 1;
    }
}
