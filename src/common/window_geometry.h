#pragma once
#include <algorithm>
#include <limits>
#include <windows.h>
namespace window_geometry {
// Serialized coordinates may be any int. Never add them in 32-bit arithmetic.
inline bool TryRect(int x, int y, int width, int height, RECT& out) noexcept {
    if (width <= 0 || height <= 0)
        return false;
    const long long right = static_cast<long long>(x) + width;
    const long long bottom = static_cast<long long>(y) + height;
    if (right > std::numeric_limits<LONG>::max() || right < std::numeric_limits<LONG>::min() ||
        bottom > std::numeric_limits<LONG>::max() || bottom < std::numeric_limits<LONG>::min())
        return false;
    out = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(right), static_cast<LONG>(bottom)};
    return true;
}
inline bool FitToWorkArea(const RECT& desired, const RECT& work, RECT& out) noexcept {
    const long long dw = static_cast<long long>(desired.right) - desired.left;
    const long long dh = static_cast<long long>(desired.bottom) - desired.top;
    const long long ww = static_cast<long long>(work.right) - work.left;
    const long long wh = static_cast<long long>(work.bottom) - work.top;
    if (dw <= 0 || dh <= 0 || ww <= 0 || wh <= 0)
        return false;
    const auto width = std::min(dw, ww), height = std::min(dh, wh);
    const auto x = std::clamp<long long>(desired.left, work.left, static_cast<long long>(work.right) - width);
    const auto y = std::clamp<long long>(desired.top, work.top, static_cast<long long>(work.bottom) - height);
    out = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + width),
           static_cast<LONG>(y + height)};
    return true;
}
} // namespace window_geometry
