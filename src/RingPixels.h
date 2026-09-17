#pragma once

#include "Pointer.h"

#include <cstdint>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace gamepad {
enum class CursorStyle { Ring, Dot, Brackets, Arrow };
struct CursorStyleChoice {
    CursorStyle style;
    std::wstring_view key;
    const wchar_t* label;
};
inline constexpr std::array cursor_styles{
    CursorStyleChoice{CursorStyle::Ring, L"ring", L"Ring - compact hollow circle"},
    CursorStyleChoice{CursorStyle::Dot, L"dot", L"Precision dot - compact target"},
    CursorStyleChoice{CursorStyle::Brackets, L"brackets", L"Open brackets - clear center"},
    CursorStyleChoice{CursorStyle::Arrow, L"arrow", L"Pointer - compact rounded head"}};
[[nodiscard]] bool valid_cursor_style(CursorStyle style);

struct RingGeometry {
    int left;
    int top;
    int side;
    int hotspot_x;
    int hotspot_y;
};

[[nodiscard]] std::optional<RingGeometry> ring_geometry(live::Point hotspot, unsigned dpi, int diameter);
// Top-down BGRA, premultiplied alpha. Pure CPU rendering; no HWND or input adapter.
[[nodiscard]] std::vector<std::uint32_t> ring_pixels(unsigned dpi, int diameter);
[[nodiscard]] std::optional<RingGeometry> cursor_geometry(live::Point hotspot, unsigned dpi, int size, CursorStyle style);
[[nodiscard]] std::vector<std::uint32_t> cursor_pixels(unsigned dpi, int size, CursorStyle style);
} // namespace gamepad
