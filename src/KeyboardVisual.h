#pragma once

#include <Windows.h>
#include "VoicePresentation.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace gamepad {
class Daisywheel;

inline constexpr int keyboard_visual_width = 392;
inline constexpr int keyboard_visual_height = 350;

struct KeyboardVisualState {
    bool active = false;
    bool preview = true;
    std::wstring_view message;
    std::wstring_view target;
    VoicePresentation voice{};
};

struct KeyboardGlyphBounds {
    float left, top, right, bottom;
    unsigned petal, face;
    wchar_t character;
};

struct KeyboardTheme {
    bool dark = false;
    bool high_contrast = false;
    std::uint32_t window = 0xFFFFFF;
    std::uint32_t window_text = 0;
    std::uint32_t highlight = 0x000080;
    std::uint32_t highlight_text = 0xFFFFFF;
    bool operator==(const KeyboardTheme&) const = default;
};

// A supplied source must outlive the renderer. Tests inject snapshots without
// modifying Windows preferences; production reads only after system notifications.
class KeyboardThemeSource {
public:
    virtual ~KeyboardThemeSource() = default;
    virtual HRESULT read(KeyboardTheme& theme) = 0;
};

// Pixels are top-down premultiplied BGRA, including transparent margins.
struct KeyboardVisualFrame {
    int width = 0;
    int height = 0;
    std::span<const std::uint32_t> pixels;
    std::span<const KeyboardGlyphBounds> glyphs;
    std::uint64_t rasterizations = 0;
    std::uint64_t surface_allocations = 0;
};

class KeyboardVisual {
public:
    explicit KeyboardVisual(KeyboardThemeSource* theme_source = nullptr);
    ~KeyboardVisual();
    KeyboardVisual(const KeyboardVisual&) = delete;
    KeyboardVisual& operator=(const KeyboardVisual&) = delete;

    // Validates WM_PAINT and presents to a borderless WS_EX_LAYERED window.
    // Neither the renderer nor its offscreen/export path changes focus or sends input.
    bool paint(HWND window, const Daisywheel& model, const KeyboardVisualState& state);
    bool render(int width, int height, const Daisywheel& model, const KeyboardVisualState& state);
    bool system_theme_changed(UINT message);
    [[nodiscard]] KeyboardVisualFrame frame() const;
    // PNG stores straight alpha; WIC converts the same premultiplied frame losslessly
    // where representable. An optional RGB background produces an opaque review image.
    bool save_png(const wchar_t* path, std::optional<std::uint32_t> background = {});
    [[nodiscard]] const std::wstring& error() const { return error_; }

private:
    class Resources;
    bool fail(std::wstring_view reason, HRESULT result);
    std::unique_ptr<Resources> resources_;
    std::wstring error_;
};
} // namespace gamepad
