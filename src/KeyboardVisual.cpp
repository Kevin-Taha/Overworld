#include "KeyboardVisual.h"
#include "KeyboardAppearance.h"
#include "Keyboard.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace overworld {
namespace {
using Microsoft::WRL::ComPtr;
constexpr D2D1_POINT_2F wheel_center{196, 153};
constexpr std::array<D2D1_POINT_2F, 8> centers{{
    {196, 63}, {279, 89}, {314, 153}, {279, 217},
    {196, 243}, {113, 217}, {78, 153}, {113, 89}}};
// A below, B right, X left, Y above: the physical controller, not an alphabet grid.
constexpr std::array<D2D1_POINT_2F, 4> offsets{{{0, 21}, {24, 0}, {-24, 0}, {0, -21}}};
constexpr std::array<D2D1_POINT_2F, 4> character_centers{{{196, 204}, {254, 153}, {138, 153}, {196, 102}}};
constexpr float character_scale = 1.4f;
constexpr float glyph_size = 20;
constexpr D2D1_RECT_F footer{30, 291, 362, 339};
constexpr D2D1_RECT_F voice_center{158, 121, 234, 185};

constexpr bool separated_layout()
{
    for (unsigned i = 0; i < 32; ++i) {
        const D2D1_POINT_2F a{centers[i / 4].x + offsets[i % 4].x,
            centers[i / 4].y + offsets[i % 4].y};
        if (a.x < 36 || a.x > keyboard_visual_width - 36 || a.y < 36 || a.y > 273) {
            return false;
        }
        for (unsigned j = i + 1; j < 32; ++j) {
            const D2D1_POINT_2F b{centers[j / 4].x + offsets[j % 4].x,
                centers[j / 4].y + offsets[j % 4].y};
            if (a.x - b.x < 20 && b.x - a.x < 20 && a.y - b.y < 20 && b.y - a.y < 20) {
                return false;
            }
        }
    }
    return true;
}
static_assert(separated_layout(), "Character cells must not overlap or touch the footer");

D2D1_COLOR_F color(std::uint32_t rgb, float alpha = 1)
{
    return D2D1::ColorF(rgb, alpha);
}

class WindowsThemeSource final : public KeyboardThemeSource {
public:
    HRESULT read(KeyboardTheme& theme) override
    {
        return read_keyboard_theme(theme);
    }
};

struct CachedGlyph {
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    DWRITE_OVERHANG_METRICS overhang{};
};

wchar_t display_character(wchar_t value)
{
    switch (value) {
    case keyboard_action_space: return L'\u2423';
    case keyboard_action_backspace: return L'\u232B';
    case keyboard_action_page_cycle: return L'\u21BB';
    case keyboard_action_enter: return L'\u21B5';
    default: return value;
    }

}

std::wstring_view voice_label(VoicePhase phase)
{
    switch (phase) {
    case VoicePhase::Loading: return L"Loading";
    case VoicePhase::Listening: return L"Listening";
    case VoicePhase::Transcribing: return L"Transcribing";
    case VoicePhase::Error: return L"Voice error";
    default: return {};
    }
}
}

class KeyboardVisual::Resources {
public:
    explicit Resources(KeyboardThemeSource* source) : theme_source(source ? source : &windows_theme) {}
    ~Resources()
    {
        release_surface();
        for (auto& glyph : glyph_cache) { glyph.layout.Reset(); }
        symbol_cache.clear();
        characters.Reset();
        labels.Reset();
        contour.Reset();
        character_contour.Reset();
        write.Reset();
        wic.Reset();
        factory.Reset();
        if (uninitialize) { CoUninitialize(); }
    }

    void release_surface()
    {
        brush.Reset();
        target.Reset();
        bitmap.Reset();
        if (dc) {
            SelectObject(dc, old_bitmap);
            DeleteObject(dib);
            DeleteDC(dc);
        }
        dc = nullptr;
        dib = nullptr;
        old_bitmap = nullptr;
        pixels = nullptr;
        width = height = 0;
        valid = false;
    }

    HRESULT initialize()
    {
        if (factory) { return S_OK; }
        if (!com_attempted) {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) { return apartment; }
            uninitialize = SUCCEEDED(apartment);
            com_attempted = true;
        }
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(write.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wic.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            hr = write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, glyph_size, L"en-US",
                characters.GetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            hr = write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"en-US",
                labels.GetAddressOf());
        }
        if (SUCCEEDED(hr)) { hr = labels->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
        if (SUCCEEDED(hr)) { hr = create_contour(); }
        if (SUCCEEDED(hr)) { hr = create_character_contour(); }
        for (unsigned index = 0; SUCCEEDED(hr) && index < glyph_cache.size(); ++index) {
            const wchar_t value = static_cast<wchar_t>(index + 32);
            auto& glyph = glyph_cache[index];
            hr = write->CreateTextLayout(&value, 1, characters.Get(), 36, 32, glyph.layout.GetAddressOf());
            if (SUCCEEDED(hr)) { hr = glyph.layout->GetMetrics(&glyph.metrics); }
            if (SUCCEEDED(hr)) { hr = glyph.layout->GetOverhangMetrics(&glyph.overhang); }
        }
        if (FAILED(hr)) {
            factory.Reset();
            write.Reset();
            wic.Reset();
            characters.Reset();
            labels.Reset();
            contour.Reset();
            character_contour.Reset();
            for (auto& glyph : glyph_cache) { glyph.layout.Reset(); }
        }
        return hr;
    }

    HRESULT create_contour()
    {
        ComPtr<ID2D1PathGeometry> centerline, ribbon;
        ComPtr<ID2D1GeometrySink> sink;
        HRESULT hr = factory->CreatePathGeometry(centerline.GetAddressOf());
        if (SUCCEEDED(hr)) { hr = centerline->Open(sink.GetAddressOf()); }
        if (FAILED(hr)) { return hr; }
        sink->BeginFigure(centers[0], D2D1_FIGURE_BEGIN_HOLLOW);
        for (std::size_t i = 0; i < centers.size(); ++i) {
            const auto& a = centers[(i + 7) % 8];
            const auto& b = centers[i];
            const auto& c = centers[(i + 1) % 8];
            const auto& d = centers[(i + 2) % 8];
            sink->AddBezier(D2D1::BezierSegment(
                D2D1::Point2F(b.x + (c.x - a.x) / 6, b.y + (c.y - a.y) / 6),
                D2D1::Point2F(c.x - (d.x - b.x) / 6, c.y - (d.y - b.y) / 6), c));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        hr = sink->Close();
        sink.Reset();
        if (SUCCEEDED(hr)) { hr = factory->CreatePathGeometry(ribbon.GetAddressOf()); }
        if (SUCCEEDED(hr)) { hr = ribbon->Open(sink.GetAddressOf()); }
        if (SUCCEEDED(hr)) { hr = centerline->Widen(30, nullptr, nullptr, 0.1f, sink.Get()); }
        if (SUCCEEDED(hr)) { hr = sink->Close(); }
        sink.Reset();
        if (FAILED(hr)) { return hr; }
        contour = ribbon;
        // Union once: translucent overlapping lobes must not accumulate opacity.
        for (const auto point : centers) {
            ComPtr<ID2D1EllipseGeometry> lobe;
            ComPtr<ID2D1PathGeometry> joined;
            hr = factory->CreateEllipseGeometry(D2D1::Ellipse(point, 42, 38), lobe.GetAddressOf());
            if (SUCCEEDED(hr)) { hr = factory->CreatePathGeometry(joined.GetAddressOf()); }
            if (SUCCEEDED(hr)) { hr = joined->Open(sink.GetAddressOf()); }
            if (SUCCEEDED(hr)) {
                hr = contour->CombineWithGeometry(lobe.Get(), D2D1_COMBINE_MODE_UNION, nullptr, 0.1f, sink.Get());
            }
            if (SUCCEEDED(hr)) { hr = sink->Close(); }
            sink.Reset();
            if (FAILED(hr)) { return hr; }
            contour = joined;
        }
        return S_OK;
    }

    HRESULT create_character_contour()
    {
        ComPtr<ID2D1EllipseGeometry> center;
        HRESULT hr = factory->CreateEllipseGeometry(D2D1::Ellipse(wheel_center, 36, 36), center.GetAddressOf());
        if (FAILED(hr)) { return hr; }
        character_contour = center;
        for (const auto point : character_centers) {
            ComPtr<ID2D1EllipseGeometry> lobe;
            ComPtr<ID2D1PathGeometry> joined;
            ComPtr<ID2D1GeometrySink> sink;
            hr = factory->CreateEllipseGeometry(D2D1::Ellipse(point, 30, 30), lobe.GetAddressOf());
            if (SUCCEEDED(hr)) { hr = factory->CreatePathGeometry(joined.GetAddressOf()); }
            if (SUCCEEDED(hr)) { hr = joined->Open(sink.GetAddressOf()); }
            if (SUCCEEDED(hr)) {
                hr = character_contour->CombineWithGeometry(lobe.Get(), D2D1_COMBINE_MODE_UNION, nullptr, 0.1f, sink.Get());
            }
            if (SUCCEEDED(hr)) { hr = sink->Close(); }
            if (FAILED(hr)) { return hr; }
            character_contour = joined;
        }
        return S_OK;
    }

    HRESULT resize(int requested_width, int requested_height)
    {
        if (width == requested_width && height == requested_height && target) { return S_OK; }
        release_surface();
        HRESULT hr = wic->CreateBitmap(static_cast<UINT>(requested_width), static_cast<UINT>(requested_height),
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.GetAddressOf());
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (SUCCEEDED(hr)) { hr = factory->CreateWicBitmapRenderTarget(bitmap.Get(), properties, target.GetAddressOf()); }
        if (SUCCEEDED(hr)) { hr = target->CreateSolidColorBrush(color(0), brush.GetAddressOf()); }
        if (FAILED(hr)) { release_surface(); return hr; }
        dc = CreateCompatibleDC(nullptr);
        if (!dc) { hr = HRESULT_FROM_WIN32(GetLastError()); release_surface(); return FAILED(hr) ? hr : E_FAIL; }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = requested_width;
        info.bmiHeader.biHeight = -requested_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib) { hr = HRESULT_FROM_WIN32(GetLastError()); release_surface(); return FAILED(hr) ? hr : E_FAIL; }
        old_bitmap = SelectObject(dc, dib);
        if (!old_bitmap || old_bitmap == HGDI_ERROR) { release_surface(); return E_FAIL; }
        pixels = static_cast<std::uint32_t*>(bits);
        width = requested_width;
        height = requested_height;
        ++allocations;
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        return S_OK;
    }

    HRESULT glyph(wchar_t value, const CachedGlyph*& result)
    {
        if (value >= 32 && value <= 126) {
            result = &glyph_cache[static_cast<std::size_t>(value - 32)];
            return S_OK;
        }
        if (value < 32 || (value >= 0xD800 && value <= 0xDFFF)) { return E_INVALIDARG; }
        const auto found = symbol_cache.find(value);
        if (found != symbol_cache.end()) { result = &found->second; return S_OK; }
        CachedGlyph symbol;
        HRESULT hr = write->CreateTextLayout(&value, 1, characters.Get(), 36, 32, symbol.layout.GetAddressOf());
        if (SUCCEEDED(hr)) { hr = symbol.layout->SetFontFamilyName(L"Segoe UI Symbol", {0, 1}); }
        if (SUCCEEDED(hr)) { hr = symbol.layout->GetMetrics(&symbol.metrics); }
        if (SUCCEEDED(hr)) { hr = symbol.layout->GetOverhangMetrics(&symbol.overhang); }
        if (FAILED(hr)) { return hr; }
        result = &symbol_cache.emplace(value, std::move(symbol)).first->second;
        return S_OK;
    }

    void fill(D2D1_RECT_F rect, float radius, D2D1_COLOR_F value)
    {
        brush->SetColor(value);
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    }

    HRESULT label(std::wstring_view value, D2D1_RECT_F rect, D2D1_COLOR_F ink,
        float size = 12, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, bool require_fit = false)
    {
        if (value.empty()) { return S_OK; }
        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = write->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), labels.Get(),
            rect.right - rect.left, rect.bottom - rect.top, layout.GetAddressOf());
        const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(value.size())};
        if (SUCCEEDED(hr)) { hr = layout->SetFontSize(size, all); }
        if (SUCCEEDED(hr)) { hr = layout->SetFontWeight(weight, all); }
        if (SUCCEEDED(hr)) { hr = layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); }
        if (SUCCEEDED(hr)) { hr = layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
        if (SUCCEEDED(hr) && require_fit) {
            DWRITE_TEXT_METRICS metrics{};
            DWRITE_OVERHANG_METRICS overhang{};
            hr = layout->GetMetrics(&metrics);
            if (SUCCEEDED(hr)) { hr = layout->GetOverhangMetrics(&overhang); }
            if (SUCCEEDED(hr) && (metrics.widthIncludingTrailingWhitespace > rect.right - rect.left
                || metrics.height > rect.bottom - rect.top || overhang.left > 0 || overhang.right > 0
                || overhang.top > 0 || overhang.bottom > 0)) { return E_INVALIDARG; }
        }
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(hr)) { hr = write->CreateEllipsisTrimmingSign(labels.Get(), ellipsis.GetAddressOf()); }
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        if (SUCCEEDED(hr)) { hr = layout->SetTrimming(&trimming, ellipsis.Get()); }
        if (FAILED(hr)) { return hr; }
        brush->SetColor(ink);
        target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return S_OK;
    }

    bool com_attempted = false;
    bool uninitialize = false;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1Geometry> contour;
    ComPtr<ID2D1Geometry> character_contour;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> characters, labels;
    std::array<CachedGlyph, 95> glyph_cache;
    std::map<wchar_t, CachedGlyph> symbol_cache;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    std::array<KeyboardGlyphBounds, 32> bounds{};
    std::size_t glyph_count = 0;
    std::uint64_t rasterizations = 0, allocations = 0, presented = 0;
    HWND presented_window = nullptr;
    bool valid = false, active = false, preview = false;
    KeyboardPage page = KeyboardPage::Letters;
    KeyboardInputMode mode = KeyboardInputMode::FaceButtons;
    KeyboardFlickStage stage = KeyboardFlickStage::Group;
    std::optional<unsigned> face;
    std::optional<unsigned> selected;
    std::optional<unsigned> highlighted;
    std::wstring message, sample;
    VoicePhase voice_phase = VoicePhase::Idle;
    unsigned voice_level = 0;
    std::wstring voice_hint;
    bool prediction_preview = false;
    PredictionView prediction;
    bool word_suggestions = false;
    std::wstring prediction_hint;
    WindowsThemeSource windows_theme;
    KeyboardThemeSource* theme_source;
    KeyboardTheme theme;
    bool theme_dirty = true;
};

KeyboardVisual::KeyboardVisual(KeyboardThemeSource* source) : resources_(std::make_unique<Resources>(source)) {}
KeyboardVisual::~KeyboardVisual() = default;

bool KeyboardVisual::system_theme_changed(UINT message)
{
    if (message != WM_SETTINGCHANGE && message != WM_THEMECHANGED && message != WM_SYSCOLORCHANGE) { return false; }
    resources_->theme_dirty = true;
    return true;
}

bool KeyboardVisual::fail(std::wstring_view reason, HRESULT result)
{
    wchar_t number[16]{};
    swprintf_s(number, L"%08lX", static_cast<unsigned long>(result));
    error_ = std::wstring(reason) + L" (0x" + number + L")";
    return false;
}

bool KeyboardVisual::render(int width, int height, const Daisywheel& model, const KeyboardVisualState& state)
{
    auto& r = *resources_;
    // Bound native allocations, including the stride and CopyPixels byte-count conversions.
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return fail(L"Unsupported keyboard surface dimensions", E_INVALIDARG);
    }
    if (state.voice.phase < VoicePhase::Idle || state.voice.phase > VoicePhase::Error) {
        return fail(L"Invalid voice presentation phase", E_INVALIDARG);
    }
    const bool voice_active = state.voice.phase != VoicePhase::Idle;
    const unsigned voice_level = state.voice.phase == VoicePhase::Listening && std::isfinite(state.voice.level)
        ? static_cast<unsigned>(std::lround(std::clamp(state.voice.level, 0.0f, 1.0f) * 16)) : 0;
    const auto voice_hint = voice_active ? state.voice.hint.substr(0, 120) : std::wstring_view{};
    PredictionView prediction;
    const bool prediction_surface = keyboard_prediction_surface(state);
    const auto prediction_hint = prediction_surface && state.word_suggestions
        ? state.prediction_hint.substr(0, 120) : std::wstring_view{};
    if (prediction_surface && state.active && !voice_active && model.phase() == KeyboardPhase::Typing) {
        if (state.prediction.count > prediction_choice_limit
            || (state.prediction.count && state.prediction.selected >= state.prediction.count)) {
            return fail(L"Invalid suggestion selection", E_INVALIDARG);
        }
        for (std::size_t i = 0; i < state.prediction.count; ++i) {
            const auto& word = state.prediction.words[i];
            if (word.empty() || word.size() > prediction_word_limit
                || !std::all_of(word.begin(), word.end(), [](wchar_t c) {
                    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
                })) { return fail(L"Invalid suggestion text", E_INVALIDARG); }
            prediction.words[i] = word;
        }
        prediction.count = state.prediction.count;
        prediction.selected = state.prediction.count ? state.prediction.selected : 0;
    }
    prediction.generation = 0;
    for (std::size_t i = prediction.count; i < prediction.words.size(); ++i) { prediction.words[i].clear(); }
    HRESULT hr = r.initialize();
    if (FAILED(hr)) { return fail(L"Keyboard graphics initialization failed", hr); }
    hr = r.resize(width, height);
    if (FAILED(hr)) { return fail(L"Keyboard surface creation failed", hr); }
    if (r.theme_dirty) {
        KeyboardTheme theme;
        hr = r.theme_source->read(theme);
        if (FAILED(hr)) { return fail(L"Keyboard theme unavailable", hr); }
        if (theme != r.theme) { r.valid = false; }
        r.theme = theme;
        r.theme_dirty = false;
    }
    auto sample = state.prediction_preview ? (state.active ? state.practice_sample : std::wstring_view{})
        : state.preview ? std::wstring_view(model.text()) : std::wstring_view{};
    sample = sample.substr(sample.size() > 44 ? sample.size() - 44 : 0);
    if (r.valid && r.page == model.page() && r.selected == model.selected_petal()
        && r.highlighted == model.highlighted_petal()
        && r.mode == model.input_mode() && r.stage == model.flick_stage() && r.face == model.selected_face()
        && r.voice_phase == state.voice.phase && r.voice_level == voice_level && r.voice_hint == voice_hint
        && r.prediction_preview == state.prediction_preview && r.prediction == prediction
        && r.word_suggestions == state.word_suggestions && r.prediction_hint == prediction_hint
        && r.active == state.active && r.preview == state.preview && r.message == state.message && r.sample == sample) {
        error_.clear();
        return true;
    }
    r.valid = false;
    const bool one_handed = model.input_mode() == KeyboardInputMode::OneHanded;
    const bool character_stage = one_handed && model.flick_stage() == KeyboardFlickStage::Character;
    if ((model.selected_petal() && *model.selected_petal() >= centers.size())
        || (model.highlighted_petal() && *model.highlighted_petal() >= centers.size())
        || (model.selected_face() && *model.selected_face() >= offsets.size())
        || (character_stage && !model.selected_petal())) {
        return fail(L"Invalid keyboard selection", E_INVALIDARG);
    }
    const int logical_height = prediction_surface ? keyboard_prediction_height : keyboard_visual_height;
    const float scale = std::min(static_cast<float>(width) / keyboard_visual_width,
        static_cast<float>(height) / logical_height);
    const float left = (width - keyboard_visual_width * scale) / 2;
    const float top = (height - logical_height * scale) / 2;
    const auto colors = keyboard_palette(r.theme);
    const auto contour = character_stage ? r.character_contour.Get() : r.contour.Get();
    const auto transform = D2D1::Matrix3x2F::Scale(scale, scale)
        * D2D1::Matrix3x2F::Translation(left, top);
    r.target->BeginDraw();
    r.target->SetTransform(D2D1::Matrix3x2F::Identity());
    r.target->Clear(D2D1::ColorF(0, 0.0f));
    // All material/highlight passes precede every letter; no later card can hide ink.
    r.target->SetTransform(D2D1::Matrix3x2F::Translation(0, 3) * transform);
    for (int spread = 10; spread >= 1 && colors.shadow_alpha > 0; --spread) {
        r.brush->SetColor(color(0x0C121C, colors.shadow_alpha));
        r.target->DrawGeometry(contour, r.brush.Get(), static_cast<float>(spread * 2));
        const float s = static_cast<float>(spread) * 0.6f;
        r.fill({footer.left - s, footer.top - s, footer.right + s, footer.bottom + s},
            22 + s, color(0x0C121C, colors.shadow_alpha));
    }
    r.target->SetTransform(transform);
    r.brush->SetColor(color(colors.surface, colors.alpha));
    r.target->FillGeometry(contour, r.brush.Get());
    r.fill(footer, 22, color(colors.surface, colors.alpha));
    r.brush->SetColor(color(colors.outline, colors.edge_alpha));
    r.target->DrawGeometry(contour, r.brush.Get(), r.theme.high_contrast ? 2.0f : 1.0f);
    r.target->DrawRoundedRectangle(D2D1::RoundedRect(footer, 22, 22), r.brush.Get(),
        r.theme.high_contrast ? 2.0f : 1.0f);
    const auto selection = character_stage ? model.selected_face() : model.highlighted_petal();
    if (state.active && selection) {
        const auto p = character_stage ? character_centers[*selection] : centers[*selection];
        const float rx = character_stage ? 28.0f : 40.0f;
        const float ry = character_stage ? 28.0f : 36.0f;
        r.brush->SetColor(color(colors.selected));
        r.target->FillEllipse(D2D1::Ellipse(p, rx, ry), r.brush.Get());
        r.brush->SetColor(color(colors.selected_ink, r.theme.high_contrast ? 1.0f : 0.28f));
        r.target->DrawEllipse(D2D1::Ellipse(p, rx, ry), r.brush.Get(), 1);
    }
    if (voice_active) {
        r.fill(voice_center, 16, color(colors.surface, colors.alpha));
        r.brush->SetColor(color(colors.outline, colors.edge_alpha));
        r.target->DrawRoundedRectangle(D2D1::RoundedRect(voice_center, 16, 16), r.brush.Get(),
            r.theme.high_contrast ? 2.0f : 1.0f);
    }

    r.glyph_count = 0;
    const auto& layout = keyboard_layout(model.page(), model.input_mode());
    for (unsigned petal = 0; SUCCEEDED(hr) && petal < centers.size(); ++petal) {
        if (character_stage && model.selected_petal() != petal) { continue; }
        for (unsigned face = 0; face < offsets.size(); ++face) {
            const bool selected = state.active && (character_stage ? model.selected_face() == face
                : model.highlighted_petal() == petal);
            const wchar_t value = display_character(layout[petal][face]);
            if (value == 0 || value == L' ') { continue; }
            const CachedGlyph* cached = nullptr;
            hr = r.glyph(value, cached);
            if (FAILED(hr)) { break; }
            const auto& glyph = *cached;
            const float magnification = character_stage ? character_scale : 1.0f;
            const auto center = character_stage ? character_centers[face]
                : D2D1::Point2F(centers[petal].x + offsets[face].x, centers[petal].y + offsets[face].y);
            const float x = center.x - glyph.metrics.width * magnification / 2;
            const float y = center.y - glyph.metrics.height * magnification / 2;
            r.brush->SetColor(color(selected ? colors.selected_ink : colors.ink));
            r.target->SetTransform(D2D1::Matrix3x2F::Scale(magnification, magnification)
                * D2D1::Matrix3x2F::Translation(x, y) * transform);
            r.target->DrawTextLayout(D2D1::Point2F(), glyph.layout.Get(), r.brush.Get());
            // Overhang metrics describe ink relative to the full layout rectangle,
            // not the text advance. Negative overhang is meaningful whitespace.
            r.bounds[r.glyph_count++] = {(x - glyph.overhang.left * magnification) * scale + left,
                (y - glyph.overhang.top * magnification) * scale + top,
                (x + (36 + glyph.overhang.right) * magnification) * scale + left,
                (y + (32 + glyph.overhang.bottom) * magnification) * scale + top, petal, face, value};
        }
    }
    r.target->SetTransform(transform);
    if (SUCCEEDED(hr) && voice_active) {
        const auto accent = color(state.voice.phase == VoicePhase::Error ? colors.error : colors.ink);
        r.brush->SetColor(accent);
        if (state.voice.phase == VoicePhase::Listening) {
            r.target->FillRoundedRectangle(D2D1::RoundedRect({191, 128, 201, 142}, 5, 5), r.brush.Get());
            r.target->PushAxisAlignedClip({184, 136, 208, 150}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            r.target->DrawEllipse(D2D1::Ellipse({196, 136}, 10, 10), r.brush.Get(), 1.5f);
            r.target->PopAxisAlignedClip();
            r.target->DrawLine({196, 146}, {196, 151}, r.brush.Get(), 1.5f);
            r.target->DrawLine({191, 151}, {201, 151}, r.brush.Get(), 1.5f);
        } else if (state.voice.phase == VoicePhase::Loading) {
            r.target->DrawEllipse(D2D1::Ellipse({196, 139}, 9, 9), r.brush.Get(), 1.5f);
            r.target->FillEllipse(D2D1::Ellipse({202, 132}, 2.5f, 2.5f), r.brush.Get());
        } else if (state.voice.phase == VoicePhase::Transcribing) {
            for (float x : {186.0f, 196.0f, 206.0f}) {
                r.target->FillEllipse(D2D1::Ellipse({x, 139}, 2.5f, 2.5f), r.brush.Get());
            }
        } else {
            hr = r.label(L"!", {184, 126, 208, 151}, accent, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
        if (SUCCEEDED(hr)) { hr = r.label(voice_label(state.voice.phase), {161, 153, 231, 172}, accent, 11); }
        if (state.voice.phase == VoicePhase::Listening) {
            r.fill({172, 175, 220, 178}, 1.5f, color(colors.outline, r.theme.high_contrast ? 1.0f : 0.4f));
            if (voice_level) {
                r.fill({172, 175, 172 + static_cast<float>(voice_level) * 3, 178}, 1.5f, accent);
            }
        }
    }
    if (SUCCEEDED(hr)) {
        if (!state.message.empty()) {
            hr = r.label(state.message.substr(0, 240), {43, 295, 349, 316},
                color(state.active ? colors.secondary : colors.error), 12);
        } else if (voice_active) {
            const auto hint = !voice_hint.empty() ? voice_hint
                : state.voice.phase == VoicePhase::Loading ? L"Preparing local voice"
                : state.voice.phase == VoicePhase::Listening ? L"Release right-stick click to transcribe"
                : state.voice.phase == VoicePhase::Transcribing ? L"Processing on this device"
                : L"Voice unavailable. Release the stick to dismiss.";
            hr = r.label(hint, {43, 295, 349, 318}, color(colors.secondary), 12);
        } else if (state.preview) {
            hr = r.label(sample.empty() ? (state.prediction_preview ? L"RB Space, then type two letters"
                    : L"Preview only - no text is sent") : sample,
                {43, 295, 349, 318}, color(colors.ink), sample.empty() ? 12.0f : 17.0f);
        } else if (one_handed) {
            hr = r.label(character_stage ? L"Flick a character, then center" : L"Flick a group, then center",
                {43, 295, 349, 318}, color(colors.secondary), 12);
        } else {
            hr = r.label(L"LB  Backspace       RB  Space       Menu  Enter",
                {43, 295, 349, 318}, color(colors.secondary), 12);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = r.label(state.prediction_preview ? L"LOCAL PRACTICE - no external output"
            : prediction_surface && state.word_suggestions ? prediction_hint
            : L"LT  Shift       RT  Symbols       View  Close",
            {43, 318, 349, 337}, color(colors.secondary), 11, DWRITE_FONT_WEIGHT_NORMAL, prediction_surface && state.word_suggestions);
    }
    if (SUCCEEDED(hr) && prediction_surface) {
        if (prediction.count) {
            for (std::size_t i = 0; SUCCEEDED(hr) && i < prediction.count; ++i) {
                const float x = 30 + static_cast<float>(i) * 112;
                const bool selected = i == prediction.selected;
                const D2D1_RECT_F chip{x, 352, x + 108, 381};
                r.fill(chip, 10, color(selected ? colors.selected : colors.surface, colors.alpha));
                r.brush->SetColor(color(selected ? colors.selected_ink : colors.outline,
                    r.theme.high_contrast ? 1.0f : 0.65f));
                r.target->DrawRoundedRectangle(D2D1::RoundedRect(chip, 10, 10), r.brush.Get(), 1);
                hr = r.label(prediction.words[i], {x + 5, 353, x + 103, 380},
                    color(selected ? colors.selected_ink : colors.ink), prediction_font_size(prediction.words[i].size()),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
            }
        }
        if (SUCCEEDED(hr) && prediction.count) {
            r.fill({30, 388, 362, 410}, 10, color(colors.surface, colors.alpha));
            hr = r.label(L"D-pad Left / Right: choose     Up: append suffix",
                {34, 389, 358, 409}, color(colors.secondary), 11);
        }
    }
    const HRESULT draw = r.target->EndDraw();
    if (FAILED(draw)) {
        r.release_surface();
        return fail(L"Keyboard vector rendering failed", draw);
    }
    if (FAILED(hr)) { return fail(L"Keyboard text layout failed", hr); }
    hr = r.bitmap->CopyPixels(nullptr, static_cast<UINT>(width * 4), static_cast<UINT>(width * height * 4),
        reinterpret_cast<BYTE*>(r.pixels));
    if (FAILED(hr)) { return fail(L"Keyboard frame transfer failed", hr); }
    r.page = model.page();
    r.selected = model.selected_petal();
    r.highlighted = model.highlighted_petal();
    r.mode = model.input_mode();
    r.stage = model.flick_stage();
    r.face = model.selected_face();
    r.active = state.active;
    r.preview = state.preview;
    r.message = state.message;
    r.sample = sample;
    r.voice_phase = state.voice.phase;
    r.voice_level = voice_level;
    r.voice_hint = voice_hint;
    r.prediction_preview = state.prediction_preview;
    r.prediction = std::move(prediction);
    r.word_suggestions = state.word_suggestions;
    r.prediction_hint = prediction_hint;
    r.valid = true;
    ++r.rasterizations;
    error_.clear();
    return true;
}

bool KeyboardVisual::paint(HWND window, const Daisywheel& model, const KeyboardVisualState& state)
{
    RECT client{};
    if (!GetClientRect(window, &client)) { return fail(L"Keyboard client rectangle unavailable", HRESULT_FROM_WIN32(GetLastError())); }
    ValidateRect(window, nullptr);
    if (!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED)) {
        return fail(L"Keyboard presentation requires a layered window", E_INVALIDARG);
    }
    if (!render(client.right, client.bottom, model, state)) { return false; }
    auto& r = *resources_;
    if (r.presented_window == window && r.presented == r.rasterizations) { return true; }
    SIZE size{r.width, r.height};
    POINT origin{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(window, nullptr, nullptr, &size, r.dc, &origin, 0, &blend, ULW_ALPHA)) {
        return fail(L"Keyboard alpha presentation failed", HRESULT_FROM_WIN32(GetLastError()));
    }
    r.presented_window = window;
    r.presented = r.rasterizations;
    return true;
}

KeyboardVisualFrame KeyboardVisual::frame() const
{
    const auto& r = *resources_;
    if (!r.valid) { return {}; }
    return {r.width, r.height, {r.pixels, static_cast<std::size_t>(r.width) * r.height},
        {r.bounds.data(), r.glyph_count}, r.rasterizations, r.allocations};
}

bool KeyboardVisual::save_png(const wchar_t* path, std::optional<std::uint32_t> background)
{
    auto& r = *resources_;
    if (!r.valid) { return fail(L"No keyboard frame available to export", E_UNEXPECTED); }
    ComPtr<IWICBitmap> composite;
    ComPtr<IWICBitmapSource> source = r.bitmap;
    std::vector<std::uint32_t> opaque;
    HRESULT hr = S_OK;
    if (background) {
        opaque.assign(r.pixels, r.pixels + static_cast<std::size_t>(r.width) * r.height);
        for (auto& pixel : opaque) {
            const auto inverse = 255 - (pixel >> 24);
            std::uint32_t result = 0xFF000000;
            for (unsigned shift = 0; shift < 24; shift += 8) {
                result |= (((pixel >> shift) & 255) + (((*background >> shift) & 255) * inverse + 127) / 255) << shift;
            }
            pixel = result;
        }
        hr = r.wic->CreateBitmapFromMemory(static_cast<UINT>(r.width), static_cast<UINT>(r.height),
            GUID_WICPixelFormat32bppPBGRA, static_cast<UINT>(r.width * 4), static_cast<UINT>(opaque.size() * 4),
            reinterpret_cast<BYTE*>(opaque.data()), composite.GetAddressOf());
        source = composite;
    }
    ComPtr<IWICFormatConverter> converted;
    if (SUCCEEDED(hr)) { hr = r.wic->CreateFormatConverter(converted.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = converted->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom); }
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) { hr = r.wic->CreateStream(stream.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = stream->InitializeFromFilename(path, GENERIC_WRITE); }
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(hr)) { hr = r.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache); }
    ComPtr<IWICBitmapFrameEncode> encoded;
    if (SUCCEEDED(hr)) { hr = encoder->CreateNewFrame(encoded.GetAddressOf(), nullptr); }
    if (SUCCEEDED(hr)) { hr = encoded->Initialize(nullptr); }
    if (SUCCEEDED(hr)) { hr = encoded->SetSize(static_cast<UINT>(r.width), static_cast<UINT>(r.height)); }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) { hr = encoded->SetPixelFormat(&format); }
    if (SUCCEEDED(hr)) { hr = encoded->WriteSource(converted.Get(), nullptr); }
    if (SUCCEEDED(hr)) { hr = encoded->Commit(); }
    if (SUCCEEDED(hr)) { hr = encoder->Commit(); }
    if (FAILED(hr)) { return fail(L"Keyboard PNG export failed", hr); }
    error_.clear();
    return true;
}
} // namespace overworld
