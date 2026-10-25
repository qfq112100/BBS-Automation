#include "../src/app/ui_geometry.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

int main() {
    unsigned checks = 0;
    auto require = [&](bool okay) {
        ++checks;
        if (!okay) {
            std::fprintf(stderr, "UI geometry failure at check %u\n", checks);
            std::exit(1);
        }
    };
    for (int dpi : {96, 113, 120, 144, 168, 192}) {
        const RECT viewport{MulDiv(224, dpi, 96), MulDiv(160, dpi, 96), MulDiv(983, dpi, 96),
                            MulDiv(679, dpi, 96)};
        for (int y = -240; y < 1280; y += 7) {
            for (int x : {200, 280, 815, 1000}) {
                const int px = MulDiv(x, dpi, 96), py = MulDiv(y, dpi, 96);
                const RECT child{px, py, px + MulDiv(252, dpi, 96), py + MulDiv(28, dpi, 96)};
                const RECT clip = nativeui::ChildViewportClip(child, viewport);
                require(clip.left >= 0 && clip.top >= 0);
                require(clip.right >= clip.left && clip.bottom >= clip.top);
                require(clip.right <= child.right - child.left && clip.bottom <= child.bottom - child.top);
                RECT physical = clip;
                OffsetRect(&physical, child.left, child.top);
                RECT expected{};
                const bool visible = IntersectRect(&expected, &child, &viewport) != FALSE;
                require(visible ? EqualRect(&physical, &expected) != FALSE : IsRectEmpty(&clip) != FALSE);
            }
        }
    }
    int remainder = 0;
    for (int delta : {30, 30, 30})
        require(nativeui::AccumulateWheelTicks(remainder, delta) == 0);
    require(nativeui::AccumulateWheelTicks(remainder, 30) == 1 && remainder == 0);
    require(nativeui::AccumulateWheelTicks(remainder, -60) == 0 && remainder == -60);
    require(nativeui::AccumulateWheelTicks(remainder, -60) == -1 && remainder == 0);
    require(nativeui::AccumulateWheelTicks(remainder, 360) == 3 && remainder == 0);
    require(nativeui::AccumulateWheelTicks(remainder, 90) == 0 && remainder == 90);
    require(nativeui::AccumulateWheelTicks(remainder, -30) == 0 && remainder == 60);
    require(nativeui::AccumulateWheelTicks(remainder, -180) == -1 && remainder == 0);
    std::printf("UI geometry/wheel: %u checks passed\n", checks);
}
