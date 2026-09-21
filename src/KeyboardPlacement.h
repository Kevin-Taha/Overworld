#pragma once

#include <Windows.h>
#include <optional>
#include <string_view>

namespace overworld {
struct KeyboardShellObservation {
    bool available = true;
    std::optional<RECT> obstruction;
};
inline constexpr unsigned keyboard_minimum_shell_percent = 80;
struct KeyboardPopupPlacement {
    RECT bounds{};
    unsigned percent = 100;
};
[[nodiscard]] std::optional<RECT> keyboard_popup_bounds(RECT work, UINT dpi, int logical_height,
    std::optional<RECT> anchor = {}, std::optional<RECT> obstruction = {}, unsigned percent = 100);
[[nodiscard]] std::optional<KeyboardPopupPlacement> keyboard_popup_placement(RECT work, UINT dpi, int logical_height,
    std::optional<RECT> anchor = {}, std::optional<RECT> obstruction = {});
[[nodiscard]] std::optional<RECT> keyboard_shell_obstruction(RECT frame, RECT window, std::optional<RECT> region = {});
[[nodiscard]] KeyboardShellObservation keyboard_shell_region(RECT frame, RECT window, int region_kind, RECT region);
[[nodiscard]] KeyboardShellObservation keyboard_shell_union(KeyboardShellObservation first,
    KeyboardShellObservation second, RECT work);
[[nodiscard]] bool keyboard_shell_image(std::wstring_view basename);
} // namespace overworld
