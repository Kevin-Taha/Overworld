#pragma once

#include <Windows.h>
#include <optional>
#include <string_view>

namespace gamepad {
[[nodiscard]] std::optional<RECT> keyboard_popup_bounds(RECT work, UINT dpi, int logical_height,
    std::optional<RECT> anchor = {}, std::optional<RECT> obstruction = {});
[[nodiscard]] bool keyboard_shell_image(std::wstring_view basename);
} // namespace gamepad
