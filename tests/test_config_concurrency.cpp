// Config snapshot Save compatibility: deterministic three-way merges and explicit conflicts.
// Production UI/Service continue to use UpdateAtomic for their transactional callbacks.
#include "../src/core/config.h"
#include "../src/core/file_ops.h"
#include "test_isolation.h"
#include <atomic>
#include <cstdio>
#include <exception>
#include <json.hpp>
#include <string>
#include <thread>
#include <vector>
using namespace core;
static int g_pass = 0, g_fail = 0;
#define CHECK(c, n)                                                                                          \
    do {                                                                                                     \
        if (c) {                                                                                             \
            ++g_pass;                                                                                        \
            printf("[PASS] %s\n", n);                                                                        \
        } else {                                                                                             \
            ++g_fail;                                                                                        \
            printf("[FAIL] %s\n", n);                                                                        \
        }                                                                                                    \
    } while (0)
static std::string ReadAll(const std::wstring& path) {
    HANDLE f = CreateFileW(testutil::Native(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot read isolated configuration");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(f);
        throw std::runtime_error("Invalid isolated configuration size");
    }
    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD n = 0;
    const BOOL ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &n, nullptr);
    CloseHandle(f);
    if (!ok || n != out.size())
        throw std::runtime_error("Incomplete isolated configuration read");
    return out;
}
static void WriteInitial(const std::wstring& path, const std::string& bytes) {
    HANDLE f = CreateFileW(testutil::Native(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create isolated configuration");
    DWORD n = 0;
    const BOOL ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr);
    CloseHandle(f);
    if (!ok || n != bytes.size())
        throw std::runtime_error("Incomplete isolated configuration write");
}
static void Test_DeterministicStaleSnapshots(const std::wstring& root) {
    const auto path = root + L"\\deterministic.json";
    const auto unknown =
        nlohmann::json{{"keep", nlohmann::json::array({1, 2, nlohmann::json{{"enabled", true}}})}};
    WriteInitial(path, nlohmann::json{{"game_root", "C:/fixture/Game"},
                                      {"win_x", 10},
                                      {"win_y", 20},
                                      {"last_sync", "initial"},
                                      {"future", unknown}}
                           .dump());
    Config ui, service;
    CHECK(Config::LoadSafe(path, ui) && Config::LoadSafe(path, service),
          "both snapshots are loaded before either disjoint writer commits");
    ui.winX = 123;
    ui.winY = 234;
    service.lastSync = L"new-sync";
    CHECK(ui.Save(path), "first stale snapshot commits only its window delta");
    CHECK(service.Save(path), "second stale snapshot merges a disjoint last_sync delta");
    Config merged;
    CHECK(Config::LoadSafe(path, merged) && merged.winX == 123 && merged.winY == 234 &&
              merged.lastSync == L"new-sync",
          "deterministic interleaving preserves exact fields from both writers");
    CHECK(nlohmann::json::parse(ReadAll(path))["future"] == unknown,
          "three-way disjoint merge preserves nested unknown fields");
    Config first, second;
    CHECK(Config::LoadSafe(path, first) && Config::LoadSafe(path, second),
          "both conflicting snapshots share the same baseline");
    first.winX = 500;
    second.winX = 600;
    CHECK(first.Save(path), "first same-field writer commits successfully");
    const auto committed = ReadAll(path);
    const bool rejected = !second.Save(path);
    const auto conflictError = Config::LastError();
    CHECK(rejected && !conflictError.empty(),
          "stale same-field writer fails explicitly with a conflict diagnostic");
    CHECK(ReadAll(path) == committed, "rejected same-field conflict keeps first commit byte-identical");
    Config retry;
    CHECK(Config::LoadSafe(path, retry), "conflicting caller can explicitly reload a fresh baseline");
    retry.winX = 600;
    CHECK(retry.Save(path), "explicit reload-and-retry succeeds");
    Config final;
    CHECK(Config::LoadSafe(path, final) && final.winX == 600 && final.winY == 234 &&
              final.lastSync == L"new-sync" && nlohmann::json::parse(ReadAll(path))["future"] == unknown,
          "retry updates only requested field and retains the other writer and unknown fields");
}
static void Test_DisjointWriters(const std::wstring& root) {
    const auto path = root + L"\\config.json";
    Config init;
    init.gameRoot = L"C:/Riot/Game";
    CHECK(init.Save(path), "initialize isolated concurrent configuration");
    std::atomic<int> uiWrites{0}, serviceWrites{0}, uiFailures{0}, serviceFailures{0}, readFailures{0};
    std::thread ui([&] {
        for (int i = 0; i < 200; ++i) {
            Config c;
            if (!Config::LoadSafe(path, c)) {
                ++readFailures;
                continue;
            }
            c.winX = 100 + i;
            c.winY = 200 + i;
            if (c.Save(path))
                ++uiWrites;
            else
                ++uiFailures;
        }
    });
    std::thread service([&] {
        for (int i = 0; i < 200; ++i) {
            Config c;
            if (!Config::LoadSafe(path, c)) {
                ++readFailures;
                continue;
            }
            c.lastSync = L"2026-10-22 10:00:0" + std::to_wstring(i % 10);
            if (c.Save(path))
                ++serviceWrites;
            else
                ++serviceFailures;
        }
    });
    ui.join();
    service.join();
    printf("UI saves=%d failed=%d; service saves=%d failed=%d; read failures=%d\n", uiWrites.load(),
           uiFailures.load(), serviceWrites.load(), serviceFailures.load(), readFailures.load());
    CHECK(readFailures == 0, "all 400 snapshot reads succeed");
    CHECK(uiWrites == 200 && serviceWrites == 200 && uiFailures == 0 && serviceFailures == 0,
          "all 200+200 disjoint Save returns report real successful commits");
    Config final;
    CHECK(Config::LoadSafe(path, final), "load final concurrent result safely");
    CHECK(final.winX == 299 && final.winY == 399 && final.lastSync == L"2026-10-22 10:00:09",
          "concurrent disjoint updates preserve exact final window and last_sync values");
}
static void Test_ContendedSameField(const std::wstring& root) {
    const auto path = root + L"\\race.json";
    Config init;
    init.gameRoot = L"C:/x";
    CHECK(init.Save(path), "initialize same-field contention fixture");
    std::atomic<int> committed{0}, conflicts{0}, undiagnosed{0}, readFailures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&, i] {
            for (int j = 0; j < 50; ++j) {
                Config c;
                if (!Config::LoadSafe(path, c)) {
                    ++readFailures;
                    continue;
                }
                c.winX = i * 1000 + j;
                if (c.Save(path))
                    ++committed;
                else {
                    ++conflicts;
                    if (Config::LastError().empty())
                        ++undiagnosed;
                }
            }
        });
    for (auto& thread : threads)
        thread.join();
    printf("Same-field Save commits=%d explicit conflicts=%d missing diagnostics=%d\n", committed.load(),
           conflicts.load(), undiagnosed.load());
    CHECK(
        readFailures == 0 && committed + conflicts == 400 && committed > 0 && undiagnosed == 0,
        "every contended write either commits or returns an observable conflict, never silently disappears");
    const auto parsed = nlohmann::json::parse(ReadAll(path));
    Config final;
    CHECK(parsed.is_object() && Config::LoadSafe(path, final) && final.winX >= 0 && final.winX <= 7049,
          "same-field contention preserves complete valid configuration, never a partial temporary file");
}
int main() {
    try {
        testutil::Directory isolated(L"config-concurrency");
        Test_DeterministicStaleSnapshots(isolated.path());
        Test_DisjointWriters(isolated.path());
        Test_ContendedSameField(isolated.path());
        printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] Configuration regression exception: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[FAIL] Unknown configuration regression exception\n");
        return 1;
    }
}
