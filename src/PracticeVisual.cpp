#include "PracticeVisual.h"
#include "PracticeLaunch.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace gamepad::practice {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::array<std::wstring_view, 5> step_names{L"1  Aim", L"2  Click", L"3  Drag", L"4  Scroll", L"5  Type"};
std::uint32_t rgb(COLORREF value)
{
    return (static_cast<std::uint32_t>(GetRValue(value)) << 16) | (GetGValue(value) << 8) | GetBValue(value);
}
D2D1_COLOR_F color(COLORREF value) { return D2D1::ColorF(rgb(value)); }
class Theme final : public KeyboardThemeSource {
public:
    KeyboardTheme value;
    HRESULT read(KeyboardTheme& theme) override { theme = value; return S_OK; }
};
struct RenderKey {
    int width, height, scroll;
    Lesson lesson;
    InputState input;
    live::Point point;
    live::Rect card;
    unsigned targets, group;
    std::array<bool, lesson_count> completed;
    bool clicked, context, dragging, keyboard, help, dark, contrast;
    std::array<COLORREF, 7> colors;
    std::wstring sample, feedback;
    KeyboardPage page;
    KeyboardPhase phase;
    KeyboardInputMode mode;
    KeyboardFlickStage stage;
    std::optional<unsigned> petal, face;
    bool operator==(const RenderKey&) const = default;
};
}
Layout layout(int width, int height)
{
    const float scale = std::min(static_cast<float>(width) / canvas_width, static_cast<float>(height) / canvas_height);
    return {scale, (width - canvas_width * scale) / 2, (height - canvas_height * scale) / 2};
}
RECT Layout::rect(live::Rect bounds) const
{
    return {static_cast<LONG>(std::lround(left + bounds.left * scale)),
        static_cast<LONG>(std::lround(top + bounds.top * scale)),
        static_cast<LONG>(std::lround(left + (bounds.right + 1) * scale)),
        static_cast<LONG>(std::lround(top + (bounds.bottom + 1) * scale))};
}
live::Point Layout::scene_point(int x, int y) const
{
    return {static_cast<int>((x - left) / scale) - scene_left, static_cast<int>((y - top) / scale) - scene_top};
}
std::vector<Control> controls(const Model& model, bool help, unsigned group)
{
    const auto& lesson = lesson_text(model.lesson());
    std::wstring guide{lesson.title};
    guide += L"\n\n"; guide += lesson.instruction; guide += L"\n\n"; guide += lesson.tip;
    if (model.lesson() == Lesson::Type && model.keyboard().input_mode() == KeyboardInputMode::OneHanded) {
        guide = L"One-handed flicks\n\nFlick UP and center to choose a-d. Then flick DOWN and center to type a.\n\n"
            L"Two separate gestures: group, then character. The highlighted choice commits on centering.";
    }
    if (help) {
        guide = L"Your safe practice space\n\nNo desktop clicks, typing, recording, or settings changes.\n\n"
            L"View closes help. Point and tap A on any button. Leaving this window pauses the controller.";
    }
    const auto scene_name = model.keyboard_open() && !help
        ? L"Local keyboard sample: " + model.keyboard().text()
            + L". Use the group and A, B, X, Y buttons below, or keys 1 to 4. View closes the keyboard."
        : std::wstring(L"Practice scene. Arrow keys move; Enter clicks; hold Space to drag; X right clicks; Page Down scrolls; Y opens keyboard.");
    std::vector<Control> result{
        {Title, {30, 10, 969, 51}, window_title},
        {InputStatus, {30, 54, 969, 80}, std::wstring(input_text(model.input()))},
        {Guidance, {678, 150, 969, help ? 529 : 443}, std::move(guide)},
        {Feedback, {30, 594, 969, 648}, std::wstring(model.feedback())},
        {Scene, {30, 150, 649, 529}, scene_name, true},
        {Retry, {30, 679, 189, 720}, L"Retry skill", true},
        {Next, {204, 679, 423, 720}, L"Next skill  /  Menu", true},
        {Help, {438, 679, 587, 720}, help ? L"Close help" : L"Controls / Help", true},
        {Reset, {602, 679, 786, 720}, L"Reset all progress", true},
        {Exit, {801, 679, 969, 720}, L"Exit practice", true}
    };
    for (unsigned i = 0; i < lesson_count; ++i) {
        const int x = 30 + static_cast<int>(i) * 190;
        result.push_back({StepFirst + static_cast<int>(i), {x, 96, x + 179, 135},
            std::wstring(step_names[i]) + (model.completed()[i] ? L"  [done]" : L""), true});
    }
    if (model.lesson() == Lesson::Type && !help) {
        result.push_back({OpenKeyboard, {678, 449, 969, 490},
            model.keyboard_open() ? L"Close keyboard  /  View" : L"Open keyboard  /  Y", true});
        result.push_back({OneHanded, {678, 501, 969, 542},
            model.keyboard().input_mode() == KeyboardInputMode::OneHanded ? L"Mode: one-handed flicks" : L"Mode: face buttons", true});
        if (model.keyboard_open()) {
            const auto& characters = keyboard_layout(model.keyboard().page(), model.keyboard().input_mode())[std::min(group, 7u)];
            std::wstring label = L"Group " + std::to_wstring(group + 1) + L": ";
            if (group == 7 && model.keyboard().input_mode() == KeyboardInputMode::OneHanded) { label += L"actions "; }
            else { for (const auto character : characters) { label += character; label += L' '; } }
            label += L"(change)";
            result.push_back({Group, {30, 548, 305, 582}, label, true});
            constexpr std::array<std::wstring_view, 4> faces{L"A / bottom", L"B / right", L"X / left", L"Y / top"};
            for (int i = 0; i < 4; ++i) {
                const int x = 316 + i * 84;
                result.push_back({FaceA + i, {x, 548, x + 77, 582}, std::wstring(faces[i]), true});
            }
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const Control& a, const Control& b) {
        return a.bounds.top < b.bounds.top || (a.bounds.top == b.bounds.top && a.bounds.left < b.bounds.left);
    });
    return result;
}
std::optional<int> clicked_control(const Model& model, const LocalClick& click, unsigned group)
{
    const live::Point down{click.down.x + scene_left, click.down.y + scene_top};
    const live::Point up{click.up.x + scene_left, click.up.y + scene_top};
    for (const auto& control : controls(model, model.help(), group)) {
        if (control.button && control.id != Scene
            && live::contains(control.bounds, down) && live::contains(control.bounds, up)) { return control.id; }
    }
    return {};
}

class Visual::Resources {
public:
    ~Resources()
    {
        wheel.reset();
        cursor_bitmap.Reset(); wheel_bitmap.Reset(); brush.Reset(); target.Reset(); bitmap.Reset(); format.Reset();
        wic.Reset(); write.Reset(); factory.Reset();
        if (uninitialize) { CoUninitialize(); }
    }
    HRESULT surface(int w, int h)
    {
        HRESULT hr = S_OK;
        if (!factory) {
            const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) { return apartment; }
            uninitialize = SUCCEEDED(apartment);
            hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
            if (SUCCEEDED(hr)) { hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(write.GetAddressOf())); }
            if (SUCCEEDED(hr)) { hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wic.GetAddressOf())); }
            if (SUCCEEDED(hr)) { hr = write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20, L"en-US", format.GetAddressOf()); }
            if (FAILED(hr)) { return hr; }
            wheel = std::make_unique<KeyboardVisual>(&theme);
        }
        if (width == w && height == h && target) { return S_OK; }
        cursor_bitmap.Reset(); wheel_bitmap.Reset(); brush.Reset(); target.Reset(); bitmap.Reset();
        hr = wic->CreateBitmap(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, bitmap.GetAddressOf());
        if (SUCCEEDED(hr)) { hr = factory->CreateWicBitmapRenderTarget(bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
            target.GetAddressOf()); }
        if (SUCCEEDED(hr)) { hr = target->CreateSolidColorBrush(D2D1::ColorF(0), brush.GetAddressOf()); }
        if (SUCCEEDED(hr)) {
            width = w; height = h; pixels.resize(static_cast<std::size_t>(w) * h); ++allocations;
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        return hr;
    }
    void box(D2D1_RECT_F rect, COLORREF fill, COLORREF edge, float radius = 12)
    {
        brush->SetColor(color(fill));
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
        brush->SetColor(color(edge));
        target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.5f);
    }
    HRESULT text(std::wstring_view value, D2D1_RECT_F bounds, float size, COLORREF ink, bool bold = false)
    {
        ComPtr<IDWriteTextLayout> text_layout;
        HRESULT hr = write->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format.Get(),
            bounds.right - bounds.left, bounds.bottom - bounds.top, text_layout.GetAddressOf());
        const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(value.size())};
        if (SUCCEEDED(hr)) { hr = text_layout->SetFontSize(size, range); }
        if (SUCCEEDED(hr) && bold) { hr = text_layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range); }
        DWRITE_TEXT_METRICS metrics{};
        DWRITE_OVERHANG_METRICS overhang{};
        if (SUCCEEDED(hr)) { hr = text_layout->GetMetrics(&metrics); }
        if (SUCCEEDED(hr)) { hr = text_layout->GetOverhangMetrics(&overhang); }
        if (FAILED(hr)) { return hr; }
        if (metrics.height > bounds.bottom - bounds.top + .1f
            || metrics.width > bounds.right - bounds.left + .1f) { return E_BOUNDS; }
        geometry.push_back({bounds.left, bounds.top, bounds.right, bounds.bottom,
            bounds.left - overhang.left, bounds.top - overhang.top,
            bounds.right + overhang.right, bounds.bottom + overhang.bottom, std::wstring(value)});
        brush->SetColor(color(ink));
        target->DrawTextLayout({bounds.left, bounds.top}, text_layout.Get(), brush.Get());
        return S_OK;
    }
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> wheel_bitmap;
    ComPtr<ID2D1Bitmap> cursor_bitmap;
    RingGeometry cursor_geometry{};
    std::vector<std::uint32_t> cursor_pixels;
    std::uint64_t wheel_rasterizations = 0;
    std::optional<RenderKey> key;
    Theme theme;
    std::unique_ptr<KeyboardVisual> wheel;
    bool uninitialize = false;
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    std::vector<TextGeometry> geometry;
    std::uint64_t allocations = 0, rasterizations = 0;
};
Visual::Visual() : resources_(std::make_unique<Resources>()) {}
Visual::~Visual() = default;
Frame Visual::frame() const
{
    const auto& r = *resources_;
    return {r.width, r.height, r.pixels, r.geometry, r.allocations, r.rasterizations};
}
KeyboardVisualFrame Visual::keyboard_frame() const
{
    return resources_->wheel ? resources_->wheel->frame() : KeyboardVisualFrame{};
}
std::span<const std::uint32_t> Visual::cursor_frame() const { return resources_->cursor_pixels; }
bool Visual::render(int width, int height, const Model& model, const AppPalette& p, bool help, unsigned group)
{
    if (width < 400 || height < 296 || width > 4000 || height > 2960) {
        error_ = L"Practice frame is outside its bounded size."; return false;
    }
    auto& r = *resources_;
    const auto& keyboard = model.keyboard();
    const RenderKey key{width, height, model.scroll_offset(), model.lesson(), model.input(), model.pointer(),
        model.card(), model.targets(), group, model.completed(), model.primary_clicked(), model.context_open(),
        model.dragging(), model.keyboard_open(), help, p.dark, p.high_contrast,
        {p.background, p.surface, p.text, p.secondary, p.border, p.accent, p.accent_text},
        keyboard.text(), std::wstring(model.feedback()), keyboard.page(), keyboard.phase(),
        keyboard.input_mode(), keyboard.flick_stage(), keyboard.highlighted_petal(), keyboard.selected_face()};
    if (r.key == key) { return true; }
    r.key.reset();
    HRESULT hr = r.surface(width, height);
    if (FAILED(hr)) { error_ = L"Practice drawing resources could not be created."; return false; }
    r.geometry.clear();
    r.target->BeginDraw();
    r.target->SetTransform(D2D1::Matrix3x2F::Identity());
    r.target->Clear(color(p.background));
    const auto view = layout(width, height);
    const auto transform = D2D1::Matrix3x2F::Scale(view.scale, view.scale)
        * D2D1::Matrix3x2F::Translation(view.left, view.top);
    r.target->SetTransform(transform);
    bool in_scene = false;
    const auto label = [&](std::wstring_view text, D2D1_RECT_F rect, float size, COLORREF ink, bool bold = false) {
        if (SUCCEEDED(hr)) {
            hr = r.text(text, rect, size, ink, bold);
            if (SUCCEEDED(hr) && in_scene) {
                auto& geometry = r.geometry.back();
                geometry.left += scene_left; geometry.right += scene_left;
                geometry.top += scene_top; geometry.bottom += scene_top;
                geometry.ink_left += scene_left; geometry.ink_right += scene_left;
                geometry.ink_top += scene_top; geometry.ink_bottom += scene_top;
            }
            if (FAILED(hr)) { error_ = L"Practice text did not fit: " + std::wstring(text); }
        }
    };
    r.box({30, 150, 650, 530}, p.surface, p.border, 18);
    r.box({18, 588, 982, 653}, p.surface, model.complete() ? p.accent : p.border);
    for (const auto& control : controls(model, help, group)) {
        if (control.id == Scene) { continue; }
        const auto b = control.bounds;
        D2D1_RECT_F bounds{static_cast<float>(b.left), static_cast<float>(b.top),
            static_cast<float>(b.right + 1), static_cast<float>(b.bottom + 1)};
        if (control.button) {
            const bool selected = control.id == StepFirst + static_cast<int>(model.lesson());
            const bool pointed = live::contains(control.bounds,
                {model.pointer().x + scene_left, model.pointer().y + scene_top});
            r.box(bounds, selected ? p.accent : p.surface, selected || pointed ? p.accent : p.border, 8);
            bounds.left += 10; bounds.right -= 6; bounds.top += 8;
            label(control.id >= FaceA ? std::wstring_view(control.text).substr(0, 1) : control.text, bounds, 16.0f,
                selected ? p.accent_text : p.text, selected);
        } else if (control.id == Guidance) {
            if (help) {
                label(L"Your safe practice space", {678, 150, 970, 226}, 28, p.text, true);
                label(L"No desktop clicks, typing, recording, or settings changes.",
                    {678, 244, 970, 352}, 22, p.text);
                label(L"View closes help. Point and tap A on any button. Leaving this window pauses the controller.",
                    {678, 372, 970, 485}, 19, p.secondary);
            } else {
                const auto& lesson = lesson_text(model.lesson());
                const bool one = model.lesson() == Lesson::Type && model.keyboard().input_mode() == KeyboardInputMode::OneHanded;
                label(one ? L"One-handed flicks" : lesson.title, {678, 150, 970, 226}, 28, p.text, true);
                label(one ? L"Flick UP and center to choose a-d. Then flick DOWN and center to type a." : lesson.instruction,
                    {678, 236, 970, 359}, 22, p.text);
                label(one ? L"Two separate gestures: group, then character. The highlighted choice commits on centering." : lesson.tip,
                    {678, 365, 970, 443}, 16, p.secondary);
            }
        } else {
            label(control.text, bounds, control.id == Title ? 28.0f : control.id == InputStatus ? 17.0f : 19.0f,
                control.id == InputStatus ? p.secondary : p.text, control.id == Title);
        }
    }
    label(std::to_wstring(model.completed_count()) + L" / 5 skills practiced",
        {30, 657, 290, 679}, 14, p.secondary);
    label(L"Menu: next skill (practice only)     View: close keyboard / exit     F1: help",
        {320, 657, 970, 679}, 14, p.secondary);
    r.target->SetTransform(D2D1::Matrix3x2F::Translation(scene_left, scene_top) * transform);
    in_scene = true;
    r.target->PushAxisAlignedClip({0, 0, world_width, world_height}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (help) {
        label(L"Controller shortcuts", {24, 16, 596, 52}, 26, p.text, true);
        label(L"Left stick   Move / choose a character group\nA   Primary click / hold to drag\nX   Right click     B   Escape / dismiss\nRight stick   Scroll     LB   Precision\nY   Open keyboard     View   Close keyboard",
            {24, 66, 596, 221}, 19, p.text);
        label(L"Voice is explained, never recorded here.", {24, 232, 596, 265}, 22, p.accent, true);
        label(L"In the live keyboard, enable voice first. Hold right-stick click, wait for Listening, speak, then release. View cancels.",
            {24, 274, 596, 337}, 18, p.text);
        label(L"Mouse / keyboard: Tab to scene; arrows move; Enter clicks; Space holds; X right-clicks; Page Up / Down scroll.",
            {24, 343, 596, 379}, 13, p.secondary);
    } else {
        switch (model.lesson()) {
        case Lesson::Aim: {
            label(L"Small movements. Confident stops.", {24, 18, 596, 58}, 24, p.text, true);
            if (model.targets() < 3) {
                const auto center = aim_target(model.targets());
                r.brush->SetColor(color(p.accent));
                r.target->FillEllipse(D2D1::Ellipse({static_cast<float>(center.x), static_cast<float>(center.y)}, 32, 32), r.brush.Get());
                label(std::to_wstring(model.targets() + 1), {static_cast<float>(center.x - 8), static_cast<float>(center.y - 20),
                    static_cast<float>(center.x + 26), static_cast<float>(center.y + 20)}, 28, p.accent_text, true);
            } else {
                label(L"All three targets reached", {80, 130, 580, 184}, 32, p.accent, true);
                label(L"Retry for another lap, or press Menu.", {80, 204, 580, 250}, 22, p.text);
            }
            label(L"LEFT STICK   move                  HOLD LB   precision", {24, 328, 596, 362}, 18, p.secondary);
            break;
        }
        case Lesson::Click:
            label(L"Select first. Then explore options.", {24, 18, 596, 58}, 24, p.text, true);
            r.box({205, 116, 415, 246}, model.primary_clicked() ? p.accent : p.background, p.accent);
            label(model.primary_clicked() ? L"Selected" : L"Weekend ideas", {225, 140, 409, 178}, 23,
                model.primary_clicked() ? p.accent_text : p.text, true);
            label(L"A  select    X  options", {225, 198, 405, 231}, 15,
                model.primary_clicked() ? p.accent_text : p.secondary);
            if (model.context_open()) {
                r.box({270, 260, 588, 354}, p.surface, p.accent);
                label(L"Options - local demonstration", {286, 272, 576, 304}, 17, p.text, true);
                label(L"B closes this menu", {286, 312, 576, 344}, 18, p.secondary);
            }
            break;
        case Lesson::Drag: {
            label(L"Carry the card into its new home.", {24, 18, 596, 58}, 24, p.text, true);
            r.box({385, 118, 585, 280}, p.background, p.accent);
            label(L"DROP HERE", {409, 78, 596, 110}, 18, p.secondary, true);
            const auto card = model.card();
            const bool placed = !model.dragging() && live::contains(drop_zone, {card.left, card.top})
                && live::contains(drop_zone, {card.right, card.bottom});
            r.box({static_cast<float>(card.left), static_cast<float>(card.top), static_cast<float>(card.right), static_cast<float>(card.bottom)},
                model.dragging() ? p.accent : p.surface, p.accent);
            label(model.dragging() ? L"Keep holding A" : placed ? L"Placed!" : L"Hold A on me",
                {static_cast<float>(card.left + 12), static_cast<float>(card.top + 29),
                    static_cast<float>(card.right - 8), static_cast<float>(card.bottom - 8)},
                18, model.dragging() ? p.accent_text : p.text, true);
            label(L"Hold A  >  move  >  release A", {24, 328, 596, 362}, 20, p.secondary);
            break;
        }
        case Lesson::Scroll: {
            label(L"Find the last card in this list.", {24, 1, 596, 36}, 23, p.text, true);
            r.box({120, 40, 500, 340}, p.background, p.border);
            r.target->PushAxisAlignedClip({122, 42, 498, 338}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int row = 0; row < 7; ++row) {
                const bool final = row == 6 && model.scroll_offset() == 480;
                const float top = 56.0f + row * 104.0f - model.scroll_offset() * (416.0f / 480.0f);
                if (top > 338 || top + 70 < 42) { continue; }
                r.box({140, top, 480, top + 70}, final ? p.accent : p.surface, p.border);
                if (top + 19 >= 42 && top + 61 <= 338) {
                    label(final ? L"Found it! Tap A" : L"Practice card " + std::to_wstring(row + 1),
                        {156, top + 19, 470, top + 61}, 22, final ? p.accent_text : p.text, final);
                }
            }
            r.target->PopAxisAlignedClip();
            r.box({515, 42, 522, 338}, p.background, p.border, 3);
            const float thumb = 44 + model.scroll_offset() * .48f;
            r.box({514, thumb, 523, thumb + 54}, p.accent, p.accent, 3);
            label(L"RIGHT STICK   scroll down / up", {124, 347, 596, 379}, 17, p.secondary);
            break;
        }
        case Lesson::Type:
            if (!model.keyboard_open()) {
                label(L"A keyboard made for your controller", {28, 28, 592, 104}, 30, p.text, true);
                label(L"Press Y to open a local typing sample.\n\nThe same character groups and gestures as the live keyboard. No external text field is opened.",
                    {28, 124, 582, 283}, 22, p.text);
                label(L"No microphone is opened. See Help for voice controls.", {28, 324, 592, 371}, 17, p.secondary);
            } else if (SUCCEEDED(hr)) {
                const KeyboardTheme theme{p.dark, p.high_contrast, rgb(p.surface), rgb(p.text), rgb(p.accent), rgb(p.accent_text)};
                if (r.theme.value != theme) {
                    r.theme.value = theme; r.wheel->system_theme_changed(WM_THEMECHANGED);
                }
                const int wheel_width = static_cast<int>(std::lround(keyboard_visual_width * view.scale));
                const int wheel_height = static_cast<int>(std::lround(keyboard_visual_height * view.scale));
                if (!r.wheel->render(wheel_width, wheel_height, model.keyboard(),
                    {model.input() == InputState::Ready, true, {}, {}})) { hr = E_FAIL; }
                if (SUCCEEDED(hr)) {
                    const auto frame = r.wheel->frame();
                    if (!r.wheel_bitmap) {
                        hr = r.target->CreateBitmap(D2D1::SizeU(frame.width, frame.height), frame.pixels.data(),
                            static_cast<UINT32>(frame.width * 4),
                            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                            r.wheel_bitmap.GetAddressOf());
                    } else if (r.wheel_rasterizations != frame.rasterizations) {
                        hr = r.wheel_bitmap->CopyFromMemory(nullptr, frame.pixels.data(), static_cast<UINT32>(frame.width * 4));
                    }
                    if (SUCCEEDED(hr)) {
                        r.wheel_rasterizations = frame.rasterizations;
                        r.target->DrawBitmap(r.wheel_bitmap.Get(), D2D1::RectF(114, 8, 506, 358));
                    }
                }
            }
            break;
        }
    }
    r.target->PopAxisAlignedClip();
    if (!model.keyboard_open() || help) {
        const auto point = model.pointer();
        if (!r.cursor_bitmap && SUCCEEDED(hr)) {
            const auto dpi = static_cast<unsigned>(std::max(96l, std::lround(96 * view.scale)));
            const auto geometry = gamepad::cursor_geometry({}, dpi, default_cursor_size, default_cursor_style);
            r.cursor_pixels = gamepad::cursor_pixels(dpi, default_cursor_size, default_cursor_style);
            if (!geometry || r.cursor_pixels.empty()) { hr = E_INVALIDARG; }
            else {
                r.cursor_geometry = *geometry;
                hr = r.target->CreateBitmap(D2D1::SizeU(geometry->side, geometry->side), r.cursor_pixels.data(),
                    static_cast<UINT32>(geometry->side * 4),
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                    r.cursor_bitmap.GetAddressOf());
            }
        }
        if (SUCCEEDED(hr)) {
            const auto& g = r.cursor_geometry;
            const float left = point.x - g.hotspot_x / view.scale, top = point.y - g.hotspot_y / view.scale;
            r.target->DrawBitmap(r.cursor_bitmap.Get(), D2D1::RectF(left, top, left + g.side / view.scale, top + g.side / view.scale));
        }
    }
    const auto draw = r.target->EndDraw();
    if (SUCCEEDED(hr)) { hr = draw; }
    if (SUCCEEDED(hr)) { hr = r.bitmap->CopyPixels(nullptr, static_cast<UINT>(width * 4),
        static_cast<UINT>(r.pixels.size() * 4), reinterpret_cast<BYTE*>(r.pixels.data())); }
    if (FAILED(hr)) {
        if (hr != E_BOUNDS) { error_ = L"Practice frame could not be rendered."; }
        return false;
    }
    ++r.rasterizations;
    r.key = key;
    error_.clear();
    return true;
}
bool Visual::save_png(const wchar_t* path)
{
    auto& r = *resources_;
    if (!r.bitmap) { error_ = L"No practice frame is available to save."; return false; }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = r.wic->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr)) { hr = stream->InitializeFromFilename(path, GENERIC_WRITE); }
    if (SUCCEEDED(hr)) { hr = r.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache); }
    if (SUCCEEDED(hr)) { hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr); }
    if (SUCCEEDED(hr)) { hr = frame->Initialize(nullptr); }
    if (SUCCEEDED(hr)) { hr = frame->SetSize(r.width, r.height); }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) { hr = frame->SetPixelFormat(&format); }
    if (SUCCEEDED(hr)) { hr = frame->WriteSource(r.bitmap.Get(), nullptr); }
    if (SUCCEEDED(hr)) { hr = frame->Commit(); }
    if (SUCCEEDED(hr)) { hr = encoder->Commit(); }
    if (FAILED(hr)) { error_ = L"Could not save the practice PNG."; return false; }
    return true;
}
} // namespace gamepad::practice
