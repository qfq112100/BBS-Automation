// test_winver.cpp — Win10 版本判定逻辑验证
// Test the same shared platform policy and real OS query used by native main.
#include "../src/common/platform.h"
#include "../src/common/window_geometry.h"
#include <climits>
#include <cstddef>
#include <winternl.h>
#include <cstdio>
#include <exception>
#include <windows.h>

static int g_pass = 0, g_fail = 0;
static void CHECK(bool c, const char* n) {
    printf("  [%s] %s\n", c ? "PASS" : "FAIL", n);
    if (c)
        g_pass++;
    else
        g_fail++;
}

static bool Supported(DWORD major, DWORD build) {
    return platform::IsSupportedWindowsVersion(major, build);
}

int main() {
    try {
        printf("=== Win10 版本判定验证 ===\n\n");

        printf("[目标] Windows 10 1507 (build 10240) 及以上，x86/x64\n");
        CHECK(Supported(10, 14393), "Win10 1607 (14393) — 应通过");
        CHECK(Supported(10, 15063), "Win10 1703 (15063) — 应通过");
        CHECK(Supported(10, 16299), "Win10 1709 (16299) — 应通过");
        CHECK(Supported(10, 17134), "Win10 1803 (17134) — 应通过");
        CHECK(Supported(10, 17763), "Win10 1809 (17763) — 应通过");
        CHECK(Supported(10, 19041), "Win10 2004 (19041) — 应通过");
        CHECK(Supported(10, 19045), "Win10 22H2 (19045) — 应通过");
        CHECK(Supported(10, 22000), "Win11 21H2 (22000) — 应通过");

        printf("\n[拒绝] 低于最低支持\n");
        CHECK(!Supported(6, 9200), "Win8 (6.2/9200) — 应拒绝");
        CHECK(!Supported(6, 9600), "Win8.1 (6.3/9600) — 应拒绝");
        CHECK(!Supported(10, 10239), "Win10 pre-release (10239) — 应拒绝");
        CHECK(!Supported(10, 0), "Win10 invalid build 0 — 应拒绝");
        CHECK(Supported(10, 10240), "Win10 1507 (10240) — 最低支持, 应通过");
        CHECK(Supported(10, 10586), "Win10 1511 (10586) — 应通过");
        CHECK(!Supported(5, 3790), "WinXP — 应拒绝");

        printf("\n[未来版本] 11/12 应放行 (major > 10)\n");
        CHECK(Supported(11, 22000), "major=11 — 应放行");
        CHECK(Supported(12, 30000), "major=12 — 应放行");

        // SDK-native pointer-sized NT structures are essential under WOW64. Never
        // serialize x64 layouts into an x86 NtCreateFile/NtSetInformationFile call.
        constexpr size_t pointer = sizeof(void*);
        CHECK(pointer == 4 || pointer == 8, "supported x86/x64 pointer model");
        CHECK(sizeof(LARGE_INTEGER) == 8 && sizeof(LONGLONG) == 8 && alignof(LONG64) >= 8,
              "64-bit file lengths and 8-byte-aligned Interlocked64 storage on x86/x64");
        CHECK(sizeof(IO_STATUS_BLOCK) == pointer * 2 && sizeof(ULONG_PTR) == pointer,
              "native IO_STATUS_BLOCK and ULONG_PTR follow target pointer width");
        CHECK(sizeof(UNICODE_STRING) == (pointer == 4 ? 8 : 16) &&
                  offsetof(UNICODE_STRING, Buffer) == (pointer == 4 ? 4 : 8),
              "native UNICODE_STRING buffer has SDK-correct size and offset");
        CHECK(sizeof(OBJECT_ATTRIBUTES) == (pointer == 4 ? 24 : 48) &&
                  offsetof(OBJECT_ATTRIBUTES, RootDirectory) == (pointer == 4 ? 4 : 8),
              "native OBJECT_ATTRIBUTES layout is target-specific, not hard-coded x64");
        CHECK(offsetof(FILE_RENAME_INFO, RootDirectory) == (pointer == 4 ? 4 : 8) &&
                  offsetof(FILE_RENAME_INFO, FileNameLength) == (pointer == 4 ? 8 : 16) &&
                  offsetof(FILE_RENAME_INFO, FileName) == (pointer == 4 ? 12 : 20),
              "native relative rename buffer layout is SDK-correct on both architectures");

        platform::WindowsVersion host{};
        CHECK(platform::QueryWindowsVersion(host), "real host version query succeeds through system ntdll");
        CHECK(host.major > 0 && host.build > 0, "real host version query returns populated data");
        RECT sentinel{1, 2, 3, 4}, out = sentinel;
        CHECK(!window_geometry::TryRect(INT_MAX, 0, 100, 100, out),
              "INT_MAX x+width overflow rejected by real placement helper");
        CHECK(EqualRect(&out, &sentinel), "failed x-overflow conversion leaves output unchanged");
        CHECK(!window_geometry::TryRect(0, INT_MAX, 100, 100, out) && EqualRect(&out, &sentinel),
              "INT_MAX y+height overflow rejected without output mutation");
        CHECK(window_geometry::TryRect(INT_MIN, 0, 100, 100, out) && out.left == INT_MIN &&
                  out.right == INT_MIN + 100,
              "INT_MIN plus positive width is legal without signed overflow");
        out = sentinel;
        CHECK(!window_geometry::TryRect(0, 0, 0, 100, out) && !window_geometry::TryRect(0, 0, 100, -1, out) &&
                  EqualRect(&out, &sentinel),
              "nonpositive dimensions rejected without output mutation");
        RECT normal{};
        CHECK(window_geometry::TryRect(120, 130, 640, 480, normal),
              "ordinary serialized rectangle converts exactly");
        const RECT work{0, 0, 800, 600};
        CHECK(window_geometry::FitToWorkArea(normal, work, out) && out.left >= work.left &&
                  out.top >= work.top && out.right <= work.right && out.bottom <= work.bottom &&
                  out.right - out.left == 640 && out.bottom - out.top == 480,
              "fitted ordinary rectangle is contained in work area with size preserved");
        const RECT oversized{-2000, -2000, 2000, 2000};
        CHECK(window_geometry::FitToWorkArea(oversized, work, out) && EqualRect(&out, &work),
              "oversized placement is safely reduced to the available work area");
        const RECT emptyWork{0, 0, 0, 600}, invertedWork{800, 0, 0, 600}, invertedDesired{10, 0, 0, 100};
        out = sentinel;
        CHECK(!window_geometry::FitToWorkArea(normal, emptyWork, out) && EqualRect(&out, &sentinel),
              "empty work area rejected without output mutation");
        CHECK(!window_geometry::FitToWorkArea(normal, invertedWork, out) && EqualRect(&out, &sentinel),
              "inverted work area rejected without output mutation");
        CHECK(!window_geometry::FitToWorkArea(invertedDesired, work, out) && EqualRect(&out, &sentinel),
              "inverted desired rectangle rejected without output mutation");
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
