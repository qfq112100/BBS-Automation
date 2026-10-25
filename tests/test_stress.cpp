// test_stress.cpp — 压力与竞态检测 (替代 ASan/UBSan 的运行时反馈回路)
// 目标: 本项目真实风险面 = 并发配置写 + 文件系统边界 + 大数据量, 而非堆溢出。
//       这里用高密度重复 + 多线程并发把潜在竞态/数据丢失逼出来。
#include "../common/winutil.h"
#include "config.h"
#include "file_ops.h"
#include "sync_engine.h"
#include "test_isolation.h"
#include "test_fixture.h"
#include <atomic>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <vector>
#include <winioctl.h>

using namespace core;

static int g_pass = 0, g_fail = 0;
static void CHECK(bool c, const char* n) {
    printf("  [%s] %s\n", c ? "PASS" : "FAIL", n);
    if (c)
        g_pass++;
    else
        g_fail++;
}

// ---------- 1) 配置并发写: 400 次跨线程读-改-写, 统计静默丢失 ----------
static void Test_ConfigConcurrency() {
    printf("[压力] 配置并发 400 次跨线程读-改-写\n");
    testutil::Directory isolated(L"stress_cfg");
    std::wstring dir = isolated.path();
    fileops::EnsureDir(dir);
    std::wstring cfg = dir + L"/config.json";
    DeleteFileW(cfg.c_str());

    // 初始配置
    {
        struct R {
            int v;
        } r{7};
        auto m = [](Config& c, void* ud) -> bool {
            c.syncMinutes = ((R*)ud)->v;
            return true;
        };
        Config::UpdateAtomic(cfg, m, &r);
    }

    const int N = 400;
    std::atomic<int> success{0}, fail{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++) {
        ts.emplace_back([&, t]() {
            for (int i = 0; i < N / 4; i++) {
                struct R {
                    int v;
                } r{(t * 1000 + i) % 1440 + 1};
                auto m = [](Config& c, void* ud) -> bool {
                    c.syncMinutes = ((R*)ud)->v;
                    return true;
                };
                if (Config::UpdateAtomic(cfg, m, &r))
                    success++;
                else
                    fail++;
            }
        });
    }
    for (auto& th : ts)
        th.join();

    // 关键断言: 原子更新不允许静默失败 (历史缺陷: 400 写仅 13 成功)
    printf("    成功=%d 失败=%d\n", success.load(), fail.load());
    CHECK(fail.load() == 0, "无一次写入被静默丢弃 (原子更新可靠)");
    CHECK(success.load() == N, "全部 400 次写入都成功");

    // 读回必须是合法 JSON (无半截文件)
    Config c = Config::Load(cfg);
    CHECK(c.syncMinutes >= 1 && c.syncMinutes <= 1440, "读回的配置值在合法区间 (无损坏)");
}

// ---------- 2) 大文件量镜像: 600 个文件 ----------
static void Test_ManyFiles() {
    printf("[压力] 600 文件镜像 + 取消\n");
    testutil::Directory isolated(L"stress_many");
    std::wstring base = isolated.path();
    std::wstring src = base + L"/src", dst = base + L"/dst";
    fileops::EnsureDir(src);
    std::wstring ownershipError;
    CHECK(fileops::PrepareDedicatedRoot(dst, ownershipError), "stress mirror destination explicitly owned");

    const int NF = 600;
    for (int i = 0; i < NF; i++) {
        // 分散到子目录, 模拟真实 saves/shards 结构
        std::wstring sub = src + L"/d" + std::to_wstring(i % 20);
        fileops::EnsureDir(sub);
        std::wstring p = sub + L"/f" + std::to_wstring(i) + L".dat";
        HANDLE h = CreateFileW(winutil::LongPath(p).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            const char* d = "0123456789abcdef";
            WriteFile(h, d, 16, nullptr, nullptr);
            CloseHandle(h);
        }
    }
    CHECK(fileops::CountFiles(src) == NF, "构造 600 个源文件");

    int calls = 0, previousDone = 0;
    bool progressValid = true;
    fileops::ProgressFn pf = [&](int done, int total, const std::wstring&) {
        calls++;
        progressValid = progressValid && done >= previousDone && total >= done;
        previousDone = done;
        return true;
    };
    fileops::MirrorResult mr = fileops::MirrorDir(src, dst, pf);
    CHECK(mr.copied == NF, "600 个文件全部复制");
    CHECK(calls >= NF && previousDone == NF && progressValid && mr.complete && mr.errors.empty(),
          "600 completed files reported monotonically without false completion");
    CHECK(fileops::CountFiles(dst) == NF, "目标目录文件数一致");

    // 取消: 第 100 个文件后中止
    int c2 = 0, completed = 0;
    fileops::ProgressFn pf2 = [&](int done, int total, const std::wstring&) {
        c2++;
        completed = done;
        return total == 0 || done < 100;
    };
    fileops::MirrorResult mr2 = fileops::MirrorDir(src, dst, pf2);
    CHECK(mr2.cancelled, "第 101 次回调后取消生效");
    CHECK(mr2.copied == 100 && completed == 100 && !mr2.complete && mr2.deleted == 0,
          "exactly 100 commits then cancellation, no deletion or false success");
}

// ---------- 3) 极端参数 ----------
static void Test_ExtremeParams() {
    printf("[压力] 极端参数与空输入\n");
    // 空路径
    CHECK(fileops::CountFiles(L"") == 0, "空路径 CountFiles = 0 (不崩)");
    CHECK(!fileops::EnsureDir(L""), "空路径 EnsureDir = false");
    CHECK(fileops::FileSize(L"") == -1, "空路径 FileSize = -1");
    CHECK(!fileops::FileReadable(L""), "空路径 FileReadable = false");

    // 不存在路径
    testutil::Directory missing(L"missing");
    std::wstring ghost = missing.path() + L"/不存在";
    CHECK(fileops::CountFiles(ghost) == 0, "不存在路径不崩");
    CHECK(!fileops::IsDir(ghost), "不存在路径 IsDir=false");

    // 超长路径 (>32767, 超出 Win32 上限)
    std::wstring huge(40000, L'a');
    CHECK(fileops::IsDir(huge) == false, "40000 字符路径不崩 (返回 false)");

    // 特殊字符路径
    testutil::Directory isolated(L"st_weird");
    std::wstring weird = isolated.path();
    fileops::EnsureDir(weird);
    std::wstring f1 = weird + L"/带空格 和#特殊&字符.dat";
    HANDLE h = CreateFileW(winutil::LongPath(f1).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "创建含空格/#/& 的文件");
    if (h != INVALID_HANDLE_VALUE) {
        const char* d = "xy";
        WriteFile(h, d, 2, nullptr, nullptr);
        CloseHandle(h);
    }
    CHECK(fileops::Exists(f1), "Exists 识别特殊字符路径");
    CHECK(fileops::FileSize(f1) == 2, "FileSize 正确");

    // SyncEngine 空 gameRoot
    SyncEngine e;
    e.cacheRoot = weird;
    e.installDir = weird;
    SyncOutcome o = e.RunOnce(true, true); // gameRoot 为空
    CHECK(o.ran && !o.success && o.errors > 0 && fileops::FileSize(f1) == 2,
          "invalid empty gameRoot fails closed without touching unrelated data");

    // Exercise >32-bit sizes without allocating or copying 5 GiB on disk.
    // FAIL rather than silently falling back to a non-sparse multi-gigabyte file.
    std::wstring big = weird + L"/big.bin";
    HANDLE hb = CreateFileW(winutil::LongPath(big).c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    bool sparse = false, sized = false;
    if (hb != INVALID_HANDLE_VALUE) {
        DWORD returned = 0;
        sparse = DeviceIoControl(hb, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
        if (sparse) {
            LARGE_INTEGER sz;
            sz.QuadPart = 5LL * 1024 * 1024 * 1024;
            sized = SetFilePointerEx(hb, sz, NULL, FILE_BEGIN) && SetEndOfFile(hb);
        }
        CloseHandle(hb);
    }
    CHECK(sparse && sized, "FSCTL_SET_SPARSE succeeded before extending file");
    CHECK(fileops::FileSize(big) == 5368709120LL, "5GB logical FileSize is exact");
    CHECK(fileops::FileReadable(big), "sparse file readable without reading 5GB");
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(winutil::LongPath(big).c_str(), &high);
    CHECK(high == 0 && low != INVALID_FILE_SIZE && low < 1024 * 1024,
          "sparse file physical allocation stays below 1 MiB");
    CHECK(DeleteFileW(winutil::LongPath(big).c_str()) != FALSE, "large sparse file removed immediately");
}

// ---------- 4) 配置损坏场景 ----------
static void Test_CorruptConfig() {
    printf("[压力] 配置损坏与恢复\n");
    testutil::Directory isolated(L"stress_corrupt");
    std::wstring dir = isolated.path();
    fileops::EnsureDir(dir);
    std::wstring cfg = dir + L"/config.json";

    const char* cases[] = {
        "",
        "not json at all",
        "{",
        "{\"a\":}",
        "[]",
        "{\"sync_minutes\": \"abc\"}",
        "{\"game_root\": 123}",
        "{\"theme\": null, \"close_action\": 999}",
    };
    for (const char* c : cases) {
        std::string raw(c);
        CHECK(!Config::IsContentSafe(raw),
              std::string("损坏配置被拒: ").append(c[0] ? c : "(空)").substr(0, 40).c_str());
    }
    // 合法配置应通过 (必须含全部 14 个必需字段, 与 IsContentSafe 的检查列表一致)
    CHECK(Config::IsContentSafe("{\"theme\":0,\"close_action\":1,\"sync_minutes\":5,"
                                "\"win_x\":0,\"win_y\":0,\"win_w\":720,\"win_h\":640,"
                                "\"auto_on\":true,\"autostart\":false,"
                                "\"game_root\":\"\",\"last_sync\":\"--\","
                                "\"cache_root\":\"\"}"),
          "完整合法配置 (14 字段) 被接受");
    // Missing fields are valid legacy input, but migration must preserve user and unknown fields.
    const auto legacyPath = dir + L"/legacy.json";
    const std::string legacy = "{\"theme\":1,\"game_root\":\"C:/Riot/Game\",\"future\":{\"answer\":42}}";
    HANDLE legacyFile = CreateFileW(legacyPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    CHECK(legacyFile != INVALID_HANDLE_VALUE, "create isolated legacy migration fixture");
    if (legacyFile != INVALID_HANDLE_VALUE) {
        WriteFile(legacyFile, legacy.data(), static_cast<DWORD>(legacy.size()), &wrote, nullptr);
        CloseHandle(legacyFile);
    }
    CHECK(Config::IsContentSafe(legacy), "missing-field legacy is valid instead of silently discarded");
    Config migrated;
    CHECK(Config::LoadSafe(legacyPath, migrated) && migrated.gameRoot == L"C:/Riot/Game" &&
              migrated.theme == 1,
          "legacy known user values survive loading");
    migrated.syncMinutes = 17;
    CHECK(migrated.Save(legacyPath), "explicit legacy migration saves atomically");
    auto saved = nlohmann::json::parse(testutil::ReadBytes(legacyPath));
    CHECK(saved["future"]["answer"] == 42 && saved["game_root"] == "C:/Riot/Game" && saved["theme"] == 1 &&
              saved["sync_minutes"] == 17,
          "real migration preserves nested unknown and known fields");

    // 半截文件 (断电场景): 写到一半的 JSON
    HANDLE h = CreateFileW(cfg.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        const char* half = "{\"theme\":0,\"close_ac"; // 断电截断
        WriteFile(h, half, (DWORD)strlen(half), nullptr, nullptr);
        CloseHandle(h);
    }
    CHECK(!Config::IsContentSafe("{\"theme\":0,\"close_ac"), "半截 JSON 判定为损坏");
    Config out;
    CHECK(!Config::LoadSafe(cfg, out), "LoadSafe 拒绝半截配置 (不会用默认值覆盖)");
}

int main() {
    try {
        printf("=== 压力与竞态回归测试 ===\n\n");
        Test_ConfigConcurrency();
        printf("\n");
        Test_ManyFiles();
        printf("\n");
        Test_ExtremeParams();
        printf("\n");
        Test_CorruptConfig();
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
