#include "KeyboardPlacement.h"
#include "KeyboardVisual.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <limits>

namespace gamepad {
bool keyboard_shell_image(std::wstring_view basename)
{
    for (const std::wstring_view expected : {L"startmenuexperiencehost.exe", L"searchhost.exe", L"searchapp.exe"}) {
        if (basename.size() != expected.size()) { continue; }
        bool same = true;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto c = basename[i];
            if ((c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c) != expected[i]) { same = false; break; }
        }
        if (same) { return true; }
    }
    return false;
}
std::optional<RECT> keyboard_popup_bounds(RECT work, UINT dpi, int logical_height,
    std::optional<RECT> anchor, std::optional<RECT> obstruction)
{
    const auto width_available = static_cast<long long>(work.right) - work.left;
    const auto height_available = static_cast<long long>(work.bottom) - work.top;
    if (dpi < 96 || dpi > 768 || width_available <= 0 || width_available > INT_MAX
        || height_available <= 0 || height_available > INT_MAX
        || (logical_height != keyboard_visual_height && logical_height != keyboard_prediction_height)) { return {}; }
    const auto full_width = MulDiv(keyboard_visual_width, dpi, 96);
    const auto full_height = MulDiv(logical_height, dpi, 96);
    const auto width = std::min<long long>(full_width, width_available);
    const auto height = std::min<long long>(full_height, height_available);
    const auto left = std::clamp<long long>(anchor ? anchor->left : work.left + (width_available - width) / 2,
        work.left, work.right - width);
    const auto top = std::clamp<long long>(anchor ? anchor->top : work.bottom - height - MulDiv(16, dpi, 96),
        work.top, work.bottom - height);
    const auto rect = [&](long long x, long long y) {
        return RECT{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
    };
    if (!obstruction) { return rect(left, top); }
    if (obstruction->right <= obstruction->left || obstruction->bottom <= obstruction->top) { return {}; }
    if (obstruction->right <= work.left || obstruction->left >= work.right
        || obstruction->bottom <= work.top || obstruction->top >= work.bottom) { return rect(left, top); }
    if (width != full_width || height != full_height) { return {}; }
    const auto gap = MulDiv(8, dpi, 96);
    const auto blocked_left = std::max<long long>(work.left, static_cast<long long>(obstruction->left) - gap);
    const auto blocked_top = std::max<long long>(work.top, static_cast<long long>(obstruction->top) - gap);
    const auto blocked_right = std::min<long long>(work.right, static_cast<long long>(obstruction->right) + gap);
    const auto blocked_bottom = std::min<long long>(work.bottom, static_cast<long long>(obstruction->bottom) + gap);
    const auto clear = [&](long long x, long long y) {
        return x + width <= blocked_left || x >= blocked_right || y + height <= blocked_top || y >= blocked_bottom;
    };
    if (clear(left, top)) { return rect(left, top); }
    const std::array<long long, 5> xs{left, work.left, work.right - width, blocked_left - width, blocked_right};
    const std::array<long long, 5> ys{top, work.top, work.bottom - height, blocked_top - height, blocked_bottom};
    std::optional<RECT> best;
    auto distance = std::numeric_limits<long long>::max();
    for (const auto y : ys) {
        for (const auto x : xs) {
            if (x < work.left || x + width > work.right || y < work.top || y + height > work.bottom || !clear(x, y)) { continue; }
            const auto moved = std::abs(x - left) + std::abs(y - top);
            if (!best || moved < distance || (moved == distance && (y < best->top || (y == best->top && x < best->left)))) {
                best = rect(x, y);
                distance = moved;
            }
        }
    }
    return best;
}
} // namespace gamepad
