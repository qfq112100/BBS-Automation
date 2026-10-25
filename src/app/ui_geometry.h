#pragma once
#include <windows.h>

namespace nativeui {
// All values are physical window pixels. Scale positions once before intersecting,
// so independently rounded logical offsets cannot leak across a fixed UI boundary.
inline RECT ChildViewportClip(const RECT& child, const RECT& viewport) {
    RECT intersection{};
    if (!IntersectRect(&intersection, &child, &viewport))
        return RECT{0, 0, 0, 0};
    OffsetRect(&intersection, -child.left, -child.top);
    return intersection;
}
inline int AccumulateWheelTicks(int& remainder, int delta) {
    remainder += delta;
    const int ticks = remainder / WHEEL_DELTA;
    remainder %= WHEEL_DELTA;
    return ticks;
}
} // namespace nativeui
