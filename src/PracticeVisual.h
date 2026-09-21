#pragma once

#include "AppUi.h"
#include "KeyboardVisual.h"
#include "Practice.h"
#include "RingPixels.h"

namespace overworld::practice {
enum ControlId {
    Title = 5000, InputStatus, Guidance, Feedback, Scene,
    StepFirst = 5010, Retry = 5020, Next, Help, Exit, Reset, OpenKeyboard, OneHanded,
    Group, FaceA, FaceB, FaceX, FaceY
};
struct Control {
    int id;
    live::Rect bounds;
    std::wstring text;
    bool button = false;
};
[[nodiscard]] std::vector<Control> controls(const Model& model, bool help, unsigned group);
[[nodiscard]] std::optional<int> clicked_control(const Model& model, const LocalClick& click, unsigned group);
struct Layout {
    float scale, left, top;
    [[nodiscard]] RECT rect(live::Rect bounds) const;
    [[nodiscard]] live::Point scene_point(int x, int y) const;
};
[[nodiscard]] Layout layout(int width, int height);
struct TextGeometry {
    float left, top, right, bottom;
    float ink_left, ink_top, ink_right, ink_bottom;
    std::wstring text;
};
struct Frame {
    int width, height;
    std::span<const std::uint32_t> pixels;
    std::span<const TextGeometry> text;
    std::uint64_t allocations, rasterizations;
};
class Visual {
public:
    Visual();
    ~Visual();
    bool render(int width, int height, const Model& model, const AppPalette& palette, bool help, unsigned group);
    bool save_png(const wchar_t* path);
    [[nodiscard]] Frame frame() const;
    [[nodiscard]] KeyboardVisualFrame keyboard_frame() const;
    [[nodiscard]] std::span<const std::uint32_t> cursor_frame() const;
    [[nodiscard]] const std::wstring& error() const { return error_; }
private:
    class Resources;
    std::unique_ptr<Resources> resources_;
    std::wstring error_;
};
} // namespace overworld::practice
