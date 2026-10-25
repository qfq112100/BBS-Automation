// test_experience.cpp — 体验优化相关回归 (P0 还原挂起 / 进度 / 取消 / 目录配置)
#include "../common/winutil.h"
#include "config.h"
#include "file_ops.h"
#include "sync_engine.h"
#include "test_isolation.h"
#include <atomic>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <vector>

using namespace core;
using fileops::ProgressFn;

static int g_pass = 0, g_fail = 0;
static void CHECK(bool cond, const char* name) {
    if (cond) {
        g_pass++;
        printf("  [PASS] %s\n", name);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", name);
    }
}

// ---------- P0: 大厅沿变时若正忙, 还原请求必须挂起而不是丢弃 ----------
// 复刻 service.cpp 的边缘检测逻辑 (m_restorePending 标志)
struct EdgeDetector {
    std::atomic<bool> m_syncBusy{false};
    std::atomic<int> m_restoreRetry{0};
    std::atomic<bool> m_restorePending{false};

    // 模拟一个 tick: 返回 true 表示本 tick 执行了还原
    bool Tick(bool edge) {
        if (edge && m_restoreRetry == 0)
            m_restorePending = true;
        if (m_restorePending && !m_syncBusy) {
            m_restorePending = false;
            m_restoreRetry = 1;
            return true; // 还原执行
        }
        return false;
    }
};

static void Test_P0_RestorePending() {
    printf("[P0] 大厅沿变 + 备份占用 → 还原不丢\n");
    EdgeDetector d;
    d.m_syncBusy = true;          // 备份进行中
    bool executed = d.Tick(true); // 大厅上线沿变
    CHECK(!executed, "忙时本 tick 不执行还原 (不冲突)");
    CHECK(d.m_restorePending, "还原请求已挂起 (关键: 不能丢)");

    d.m_syncBusy = false;     // 备份结束
    executed = d.Tick(false); // 下一 tick (edge 已过)
    CHECK(executed, "空闲后自动执行挂起的还原");
    CHECK(!d.m_restorePending, "挂起标志已清");
}

static void Test_P0_CompareOldBehaviour() {
    printf("[P0] 旧逻辑对照 (证明这是真缺陷, 修复必要)\n");
    // 旧逻辑: edge && retry==0 && !busy — 忙时直接跳过, 无挂起
    std::atomic<bool> busy{true};
    std::atomic<int> retry{0};
    bool edge = true;
    bool executed = (edge && retry.load() == 0 && !busy.load());
    CHECK(!executed, "旧逻辑: 忙时还原被跳过");
    // 后续 tick: edge=false 且 retry==0 → 永远不执行
    edge = false;
    bool executed2 = (edge && retry.load() == 0 && !busy.load());
    CHECK(!executed2, "旧逻辑: 后续 tick 也不再执行 (机会彻底丢失)");
    CHECK(retry.load() == 0, "旧逻辑: retry 保持 0, 重试机制不启动");
}

// ---------- P2: 进度回调 ----------
static void Test_ProgressCallback() {
    printf("[P2] 进度回调: done/total/current 正确递增\n");
    // 构造一个小的目录树
    testutil::Directory isolated(L"test_prog");
    std::wstring base = isolated.path();
    fileops::EnsureDir(base + L"/src/sub");
    for (int i = 0; i < 5; i++) {
        HANDLE h = CreateFileW((base + L"/src/f" + std::to_wstring(i) + L".dat").c_str(), GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            const char* d = "xxxx";
            WriteFile(h, d, 4, nullptr, nullptr);
            CloseHandle(h);
        }
    }

    std::wstring ownershipError;
    CHECK(fileops::PrepareDedicatedRoot(base + L"/dst", ownershipError),
          "progress destination explicitly owned");
    int lastDone = -1, lastTotal = -1, calls = 0;
    bool validProgress = true, sawCurrent = false;
    std::wstring lastCur;
    fileops::ProgressFn pf = [&](int done, int total, const std::wstring& cur) {
        validProgress = validProgress && done >= 0 && total >= done &&
                        (lastTotal <= 0 || total == lastTotal) && done >= lastDone;
        lastDone = done;
        lastTotal = total;
        lastCur = cur;
        calls++;
        sawCurrent = sawCurrent || !cur.empty();
        return true;
    };
    fileops::MirrorResult mr = fileops::MirrorDir(base + L"/src", base + L"/dst", pf);
    CHECK(calls > 0, "进度回调被调用");
    CHECK(lastTotal == 5, "total 正确 (5 个文件)");
    CHECK(mr.copied == 5, "5 个文件全部复制");
    CHECK(validProgress && lastDone == 5 && mr.complete && mr.errors.empty(),
          "progress remains monotonic and completes at done==total");
    CHECK(sawCurrent, "progress names actual current files");
}

// ---------- P2: 取消 ----------
static void Test_Cancel() {
    printf("[P2] 取消: 回调返回 false 即中止\n");
    testutil::Directory isolated(L"test_cancel");
    std::wstring base = isolated.path();
    fileops::EnsureDir(base + L"/src");
    for (int i = 0; i < 10; i++) {
        HANDLE h = CreateFileW((base + L"/src/f" + std::to_wstring(i) + L".dat").c_str(), GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            const char* d = "xxxx";
            WriteFile(h, d, 4, nullptr, nullptr);
            CloseHandle(h);
        }
    }
    std::wstring ownershipError;
    CHECK(fileops::PrepareDedicatedRoot(base + L"/dst", ownershipError),
          "cancel destination explicitly owned");
    int calls = 0, greatestDone = 0;
    fileops::ProgressFn pf = [&](int done, int total, const std::wstring&) {
        calls++;
        greatestDone = (std::max)(greatestDone, done);
        return total == 0 || done < 3; // cancellation follows completed work, not callback count
    };
    fileops::MirrorResult mr = fileops::MirrorDir(base + L"/src", base + L"/dst", pf);
    CHECK(mr.cancelled, "结果标记为已取消");
    CHECK(mr.copied == 3, "只复制了 3 个 (回调被调用 3 次后取消)");
    CHECK(calls > 0 && greatestDone == 3 && fileops::CountFiles(base + L"/dst") == 3 && !mr.complete,
          "cancellation commits exactly the completed files and never reports completion");
}

// ---------- P1: 目录配置 ----------
static void Test_DirConfig() {
    printf("[P1] 目录配置: 写入 / 读取 / 损坏保护\n");
    testutil::Directory isolated(L"dirs");
    std::wstring cfgPath = isolated.path() + L"/config.json";
    // 清掉上次的残留 (损坏测试会留下非法 JSON — UpdateAtomic 会正确地拒绝在
    // 不可信配置上写入, 所以必须先删, 否则本测试从第二次运行起就失败)
    DeleteFileW(cfgPath.c_str());

    // 写: 通过 SaveSettingsReq 类似的 mutator
    struct Req {
        std::wstring cacheRoot, snapRoot;
        int snapHours;
    };
    Req r;
    r.cacheRoot = L"D:/MyCache";
    auto mut = [](Config& c, void* ud) -> bool {
        Req* q = (Req*)ud;
        c.cacheRoot = q->cacheRoot;
        return true;
    };
    bool ok = Config::UpdateAtomic(cfgPath, mut, (void*)&r);
    CHECK(ok, "配置原子写入成功");

    Config c = Config::Load(cfgPath);
    CHECK(c.cacheRoot == L"D:/MyCache", "cache_root 读回一致");

    // 损坏保护: cache_root 写成数字 → IsContentSafe 应判为不可信
    HANDLE h =
        CreateFileW(cfgPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    const char* bad = "{\"cache_root\": 123}";
    WriteFile(h, bad, (DWORD)strlen(bad), nullptr, nullptr);
    CloseHandle(h);
    CHECK(!Config::IsContentSafe(bad), "cache_root 类型错 → 判定为不可信 (拒绝覆盖)");
    Config out;
    CHECK(!Config::LoadSafe(cfgPath, out), "LoadSafe 拒绝加载损坏配置");
}

// ---------- 长路径下的目录配置 ----------
static void Test_LongPathDir() {
    printf("[P1] 长路径目录 (>260 字符)\n");
    testutil::Directory isolated(L"test_longdir");
    std::wstring base = isolated.path();
    std::wstring p = base;
    fileops::EnsureDir(base);
    int i = 0;
    while (p.size() < 300) {
        p += L"/第" + std::to_wstring(i) + L"层";
        if (!fileops::EnsureDir(p)) {
            break;
        }
        i++;
    }
    CHECK(p.size() > 260, "构造出 >260 字符路径");
    CHECK(fileops::IsDir(p), "长路径目录 IsDir 成功");
    CHECK(fileops::CountFiles(p) == 0, "长路径目录可枚举 (0 文件)");
}

int main() {
    try {
        printf("=== 体验优化回归测试 ===\n\n");
        Test_P0_RestorePending();
        Test_P0_CompareOldBehaviour();
        Test_ProgressCallback();
        Test_Cancel();
        Test_DirConfig();
        Test_LongPathDir();
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
