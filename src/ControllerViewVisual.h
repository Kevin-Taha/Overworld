#pragma once

#include "ControllerView.h"
#include "KeyboardAppearance.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace overworld {
inline constexpr int controller_view_width = 320;
inline constexpr int controller_view_height = 240;
inline constexpr int controller_view_min_width = 256;
inline constexpr int controller_view_min_height = 192;
inline constexpr int controller_view_max_width = 2048;
inline constexpr int controller_view_max_height = 1536;

enum class ControllerViewControl {
    Up, Down, Left, Right, Menu, View, LeftStick, RightStick,
    LeftShoulder, RightShoulder, A, B, X, Y, LeftTrigger, RightTrigger
};
struct ControllerViewRect {
    float left, top, right, bottom;
};
struct ControllerViewControlBounds {
    ControllerViewControl control;
    ControllerViewRect bounds;
};
struct ControllerViewStick {
    float center_x, center_y, cap_x, cap_y, travel, well_radius, cap_radius;
};
// All geometry is in frame pixels. Spans remain valid until the next render.
// Top-down premultiplied BGRA: clear outside the artwork, a mildly translucent
// body, and opaque controls. High contrast keeps the body opaque as well.
struct ControllerViewVisualFrame {
    int width = 0, height = 0;
    std::span<const std::uint32_t> pixels;
    std::span<const ControllerViewControlBounds> controls;
    std::array<ControllerViewStick, 2> sticks{};
    ControllerViewRect body_bounds{};
    std::uint64_t rasterizations = 0, surface_allocations = 0;
};

// Thread-confined, including construction/destruction. No HWND or reader is
// needed for offscreen rendering; the caller supplies an explicit theme.
class ControllerViewVisual {
public:
    ControllerViewVisual();
    ~ControllerViewVisual();
    ControllerViewVisual(const ControllerViewVisual&) = delete;
    ControllerViewVisual& operator=(const ControllerViewVisual&) = delete;

    bool render(int width, int height, const ControllerViewState& state, const KeyboardTheme& theme);
    [[nodiscard]] ControllerViewVisualFrame frame() const;
    bool save_png(const wchar_t* path);
    [[nodiscard]] const std::wstring& error() const { return error_; }

private:
    class Resources;
    bool fail(std::wstring_view message, HRESULT result);
    std::unique_ptr<Resources> resources_;
    std::wstring error_;
};
} // namespace overworld
