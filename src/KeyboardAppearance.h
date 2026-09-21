#pragma once

#include "KeyboardVisual.h"

namespace overworld {
struct KeyboardPalette {
    std::uint32_t surface, ink, secondary, selected, selected_ink, outline, error;
    float alpha, edge_alpha, shadow_alpha;
};

[[nodiscard]] inline KeyboardPalette keyboard_palette(const KeyboardTheme& theme)
{
    if (theme.high_contrast) {
        return {theme.window, theme.window_text, theme.window_text, theme.highlight,
            theme.highlight_text, theme.window_text, theme.window_text, 1, 1, 0};
    }
    if (theme.dark) {
        return {0x242A32, 0xF0F3F7, 0xC0C9D4, 0x304F70, 0xF1F7FF, 0xACBCD0, 0xFFC1AD,
            0.97f, 0.32f, 0.018f};
    }
    return {0xF9F9F6, 0x303640, 0x58616C, 0xDCE9F7, 0x194E83, 0x7F8F9E, 0x8D3826,
        0.97f, 0.26f, 0.012f};
}

[[nodiscard]] HRESULT read_keyboard_theme(KeyboardTheme& theme);
} // namespace overworld
