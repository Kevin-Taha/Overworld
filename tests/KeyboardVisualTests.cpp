#include "KeyboardVisual.h"
#include "Keyboard.h"
#include "PredictionPractice.h"
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

using namespace gamepad;
namespace {
void check(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

void render(KeyboardVisual& visual, int width, int height, const Daisywheel& model, KeyboardVisualState state)
{
    if (!visual.render(width, height, model, state)) {
        std::wcerr << visual.error() << L'\n';
        throw std::runtime_error("render failed");
    }
}

struct Fixture {
    Daisywheel model;
    Snapshot snapshot{};
    Tick now = 1000;
    explicit Fixture(KeyboardInputMode mode = KeyboardInputMode::FaceButtons)
    {
        model.set_input_mode(mode);
        snapshot.slots[0].connection = Connection::Connected;
        snapshot.slots[0].connection_epoch = 1;
        tick();
    }
    void tick()
    {
        now += 16;
        snapshot.sampled_at = now;
        ++snapshot.sequence;
        model.step(snapshot, now);
    }
    void select(unsigned group, unsigned page)
    {
        const double angle = group * std::numbers::pi / 4;
        auto& pad = snapshot.slots[0];
        pad.left_x = static_cast<std::int16_t>(std::sin(angle) * 32767);
        pad.left_y = static_cast<std::int16_t>(std::cos(angle) * 32767);
        pad.left_trigger = (page & 1) ? 255 : 0;
        pad.right_trigger = (page & 2) ? 255 : 0;
        tick();
        if (model.input_mode() == KeyboardInputMode::FaceButtons) {
            check(model.selected_petal() == group, "fixture must select requested clockwise group");
        }
    }
    void center()
    {
        snapshot.slots[0].left_x = snapshot.slots[0].left_y = 0;
        tick();
    }
    void face(unsigned index)
    {
        constexpr std::array<std::array<std::int16_t, 2>, 4> direction{{
            {0, -32767}, {32767, 0}, {-32767, 0}, {0, 32767}}};
        snapshot.slots[0].left_x = direction[index][0];
        snapshot.slots[0].left_y = direction[index][1];
        tick();
        check(model.selected_face() == index, "fixture must lock requested cardinal character");
    }
};

bool overlaps(const KeyboardGlyphBounds& a, const KeyboardGlyphBounds& b)
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

float widest_span = 0;
float smallest_gap = 1000;

class FakeTheme final : public KeyboardThemeSource {
public:
    HRESULT read(KeyboardTheme& value) override { ++reads; value = theme; return result; }
    KeyboardTheme theme;
    unsigned reads = 0;
    HRESULT result = S_OK;
};

double luminance(std::uint32_t pixel, std::uint32_t background)
{
    const double alpha = static_cast<double>(pixel >> 24) / 255;
    double result = 0;
    for (const auto [shift, weight] : std::array<std::pair<unsigned, double>, 3>{{{16, 0.2126}, {8, 0.7152}, {0, 0.0722}}}) {
        const double channel = (((pixel >> shift) & 255) + ((background >> shift) & 255) * (1 - alpha)) / 255;
        result += weight * (channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4));
    }
    return result;
}

double contrast(std::uint32_t a, std::uint32_t b, std::uint32_t background)
{
    const auto x = luminance(a, background), y = luminance(b, background);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

void inspect(const KeyboardVisualFrame& frame, bool high_contrast = false, bool character_stage = false, bool voice_active = false)
{
    check(frame.glyphs.size() == (character_stage ? 4 : 32), "visible glyph count must match group/character stage");
    const float scale = static_cast<float>(frame.width) / keyboard_visual_width;
    const auto pixel_at = [&](float x, float y) {
        return frame.pixels[static_cast<std::size_t>(y * scale) * frame.width + static_cast<std::size_t>(x * scale)];
    };
    float leftmost = static_cast<float>(frame.width), rightmost = 0;
    for (std::size_t i = 0; i < frame.glyphs.size(); ++i) {
        const auto& a = frame.glyphs[i];
        leftmost = std::min(leftmost, a.left);
        rightmost = std::max(rightmost, a.right);
        check(a.left < a.right && a.top < a.bottom, "glyph ink bounds must be nonempty");
        if (voice_active) {
            check(a.right <= 158 * scale || a.left >= 234 * scale || a.bottom <= 121 * scale || a.top >= 185 * scale,
                "voice center must not overlap any actual character ink");
        }
        check(a.left >= 36 * scale && a.right <= (keyboard_visual_width - 36) * scale
            && a.top >= 22 * scale && a.bottom < 277 * scale, "glyph must stay inside body and above footer");
        for (std::size_t j = i + 1; j < frame.glyphs.size(); ++j) {
            const auto& b = frame.glyphs[j];
            check(!overlaps(a, b), "actual glyph ink must never overlap");
            const float dx = std::max({a.left - b.right, b.left - a.right, 0.0f});
            const float dy = std::max({a.top - b.bottom, b.top - a.bottom, 0.0f});
            smallest_gap = std::min(smallest_gap, std::sqrt(dx * dx + dy * dy) / scale);
        }
        constexpr std::array<std::pair<float, float>, 8> centers{{
            {196.0f, 63.0f}, {279.0f, 89.0f}, {314.0f, 153.0f}, {279.0f, 217.0f},
            {196.0f, 243.0f}, {113.0f, 217.0f}, {78.0f, 153.0f}, {113.0f, 89.0f}}};
        constexpr std::array<std::pair<float, float>, 4> character_centers{{
            {196.0f, 204.0f}, {254.0f, 153.0f}, {138.0f, 153.0f}, {196.0f, 102.0f}}};
        const auto backing = character_stage
            ? pixel_at(character_centers[a.face].first + 22, character_centers[a.face].second)
            : pixel_at(centers[a.petal].first, centers[a.petal].second);
        check((backing >> 24) >= 240, "glyph group must have sufficiently opaque backing");
        std::array<double, 2> strongest{};
        for (int y = static_cast<int>(std::floor(a.top)); y < static_cast<int>(std::ceil(a.bottom)); ++y) {
            for (int x = static_cast<int>(std::floor(a.left)); x < static_cast<int>(std::ceil(a.right)); ++x) {
                const auto pixel = frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
                check((pixel >> 24) >= 240, "all actual glyph ink must be contained by the form-fitting material");
                strongest[0] = std::max(strongest[0], contrast(pixel, backing, 0xFFFFFF));
                strongest[1] = std::max(strongest[1], contrast(pixel, backing, 0));
            }
        }
        check(strongest[0] >= 4.5 && strongest[1] >= 4.5,
            "every glyph needs visible 4.5:1 ink over both light and dark desktops");
    }
    widest_span = std::max(widest_span, (rightmost - leftmost) / scale);
    check((rightmost - leftmost) / scale <= 300, "visible character ink span must stay at or below 300 DIP");
    for (unsigned petal = 0; petal < frame.glyphs.size() / 4; ++petal) {
        const auto& a = frame.glyphs[petal * 4];
        const auto& b = frame.glyphs[petal * 4 + 1];
        const auto& x = frame.glyphs[petal * 4 + 2];
        const auto& y = frame.glyphs[petal * 4 + 3];
        check(a.top > b.top && a.top > x.top && y.bottom < b.bottom && y.bottom < x.bottom
            && b.left > a.left && b.left > y.left && x.right < a.right && x.right < y.right,
            "actual glyphs must preserve A-bottom/B-right/X-left/Y-top physical diamond");
    }
    for (const auto pixel : frame.pixels) {
        const auto alpha = pixel >> 24;
        check((pixel & 255) <= alpha && ((pixel >> 8) & 255) <= alpha && ((pixel >> 16) & 255) <= alpha,
            "every BGRA color channel must be premultiplied by alpha");
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            check(frame.pixels[static_cast<std::size_t>(y) * frame.width + x] == 0,
                "top-left corner must be genuinely transparent");
            check(frame.pixels[static_cast<std::size_t>(frame.height - 1 - y) * frame.width + frame.width - 1 - x] == 0,
                "bottom-right corner must be genuinely transparent");
        }
    }
    if (!character_stage && !voice_active) {
        for (int y = 125; y <= 181; ++y) {
            for (int x = 170; x <= 222; ++x) {
                check(pixel_at(static_cast<float>(x), static_cast<float>(y)) == 0,
                    "unused center must be genuinely transparent with no page/face legend");
            }
        }
    } else if (character_stage) {
        check(pixel_at(78, 153) == 0 && pixel_at(314, 153) == 0,
            "character stage must remove the unused outer groups and material");
    }
    check(pixel_at(40, 40) == 0 && pixel_at(352, 260) == 0, "unused corners of old backing must be transparent");
    const auto material = character_stage ? pixel_at(196, 153) : pixel_at(196, 63);
    check((material >> 24) >= 240, "material must back the north group");
    if (!high_contrast) {
        check(std::count_if(frame.pixels.begin(), frame.pixels.end(), [](auto value) {
            return (value >> 24) >= 240 && (value >> 24) < 255;
        }) > 1000, "normal contour must contain genuinely translucent material");
    }
    const auto shadow = pixel_at(196, 344);
    check(high_contrast ? shadow == 0 : (shadow >> 24) > 0 && (shadow >> 24) < 64,
        "normal contour has a subtle shadow; high contrast must remove it");
}

void export_frame(KeyboardVisual& visual, const std::filesystem::path& directory, const std::wstring& name);

void prediction_frames(const std::filesystem::path& directory)
{
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf()))), "vocabulary text metrics factory");
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    check(SUCCEEDED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14, L"en-US", format.GetAddressOf())), "vocabulary text format");
    check(SUCCEEDED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)), "vocabulary labels do not wrap");
    check(SUCCEEDED(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER))
        && SUCCEEDED(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)), "vocabulary label alignment");
    for (auto word : prediction_vocabulary()) {
        for (unsigned casing = 0; casing < 3; ++casing) {
            std::wstring value(word.begin(), word.end());
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (casing == 2 || (casing == 1 && !i)) { value[i] = static_cast<wchar_t>(value[i] - (L'a' - L'A')); }
            }
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            check(SUCCEEDED(factory->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format.Get(),
                98, 27, layout.GetAddressOf())), "vocabulary text layout");
            check(SUCCEEDED(layout->SetFontSize(prediction_font_size(value.size()), {0, static_cast<UINT32>(value.size())})),
                "vocabulary bounded font size");
            DWRITE_TEXT_METRICS metrics{}; DWRITE_OVERHANG_METRICS ink{};
            check(SUCCEEDED(layout->GetMetrics(&metrics)) && SUCCEEDED(layout->GetOverhangMetrics(&ink)), "vocabulary ink metrics");
            check(metrics.widthIncludingTrailingWhitespace <= 98 && metrics.height <= 27
                && ink.left <= 0 && ink.right <= 0 && ink.top <= 0 && ink.bottom <= 0,
                "every lower/title/upper vocabulary word must fit without clipping or ellipsis");
        }
    }
    FakeTheme source;
    KeyboardVisual visual(&source);
    unsigned count = 0;
    for (unsigned theme = 0; theme < 4; ++theme) {
        source.theme = {};
        source.theme.dark = theme == 1;
        source.theme.high_contrast = theme >= 2;
        if (theme == 3) {
            source.theme.window = 0; source.theme.window_text = 0xFFFFFF;
            source.theme.highlight = 0xFFFF00; source.theme.highlight_text = 0;
        }
        visual.system_theme_changed(WM_THEMECHANGED);
        for (auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
            PredictionPractice practice; seed_prediction_demo(practice, mode);
            check(practice.sample() == L" he" && practice.keyboard().prediction_view().count == 3,
                "visual fixture must use actual keyboard-completed edits");
            KeyboardVisualState state{true, true, {}, {}};
            state.prediction_preview = true;
            state.prediction = practice.keyboard().prediction_view();
            state.practice_sample = practice.sample();
            for (int percent : {100, 150, 200}) {
                const int width = keyboard_visual_width * percent / 100;
                const int height = keyboard_prediction_height * percent / 100;
                const float scale = percent / 100.0f;
                render(visual, width, height, practice.keyboard().model(), state);
                inspect(visual.frame(), source.theme.high_contrast);
                const auto pixel = [&](float x, float y) {
                    return visual.frame().pixels[static_cast<std::size_t>(y * scale) * width
                        + static_cast<std::size_t>(x * scale)];
                };
                for (unsigned i = 0; i < 3; ++i) {
                    const float x = 30 + i * 112.0f;
                    const auto backing = pixel(x + 10, 365);
                    check((backing >> 24) >= 240, "suggestion must have opaque enough theme backing");
                    double strongest = 0;
                    for (int y = 355; y < 378; ++y) {
                        for (int ink_x = static_cast<int>(x + 18); ink_x < x + 90; ++ink_x) {
                            strongest = std::max(strongest, std::min(
                                contrast(pixel(static_cast<float>(ink_x), static_cast<float>(y)), backing, 0),
                                contrast(pixel(static_cast<float>(ink_x), static_cast<float>(y)), backing, 0xFFFFFF)));
                        }
                    }
                    check(strongest >= 4.5, "each actual suggestion label has 4.5:1 visible ink");
                }
                const auto before = visual.frame();
                const std::vector<std::uint32_t> original(before.pixels.begin(), before.pixels.end());
                for (unsigned i = 0; i < 30; ++i) { render(visual, width, height, practice.keyboard().model(), state); }
                check(before.rasterizations == visual.frame().rasterizations
                    && before.surface_allocations == visual.frame().surface_allocations, "idle suggestion frames reuse surfaces and pixels");
                auto selected = state; selected.prediction.selected = 1;
                render(visual, width, height, practice.keyboard().model(), selected);
                check(visual.frame().rasterizations == before.rasterizations + 1, "selected word visibly updates");
                for (int y = 0; y < static_cast<int>(350 * scale); ++y) {
                    for (int x = 0; x < width; ++x) {
                        const auto index = static_cast<std::size_t>(y) * width + x;
                        check(original[index] == visual.frame().pixels[index], "selection never changes wheel, voice center or footer pixels");
                    }
                }
                render(visual, width, height, practice.keyboard().model(), state);
                if (!directory.empty() && percent != 200) {
                    export_frame(visual, directory, L"prediction-" + std::to_wstring(theme) + L"-"
                        + std::to_wstring(static_cast<unsigned>(mode)) + L"-" + std::to_wstring(percent));
                }
                for (unsigned hidden = 0; hidden < 3; ++hidden) {
                    auto muted = state;
                    if (hidden == 0) { muted.active = false; }
                    else if (hidden == 1) { muted.voice = {VoicePhase::Listening, 0.5f, {}}; }
                    else { muted.prediction = {}; }
                    render(visual, width, height, practice.keyboard().model(), muted);
                    check(pixel(80, 365) == 0 && pixel(196, 365) == 0 && pixel(306, 365) == 0 && pixel(196, 399) == 0,
                        "suggestion chips truly disappear when inactive, voice-active or without prefix");
                }
                ++count;
            }
        }
    }
    PredictionPractice practice; seed_prediction_demo(practice, KeyboardInputMode::FaceButtons);
    KeyboardVisualState state{true, true, {}, {}};
    state.prediction_preview = true;
    state.prediction = practice.keyboard().prediction_view();
    render(visual, keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state);
    auto raster = visual.frame().rasterizations;
    state.prediction.words[0] = L"help";
    render(visual, keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state);
    check(visual.frame().rasterizations == raster + 1, "renderer owns cached suggestion text, not an alias");
    state.prediction.count = 4;
    check(!visual.render(keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state),
        "oversized suggestion list explicitly rejected");
    state.prediction.count = 3; state.prediction.words[0] = std::wstring(prediction_word_limit + 1, L'a');
    check(!visual.render(keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state),
        "oversized suggestion text explicitly rejected");
    state.prediction.words[0] = L"bad\ntext";
    check(!visual.render(keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state),
        "multiline/control suggestion text explicitly rejected");
    state.prediction.words[0] = L"WWWWWWWWWWWWWWWW";
    check(!visual.render(keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state),
        "actual ink that cannot fit must not silently truncate a completion");
    state.prediction = practice.keyboard().prediction_view();
    state.preview = state.prediction_preview = false;
    state.word_suggestions = true;
    state.prediction_hint = L"Suggestions ON - privacy unverified. Down hides";
    state.practice_sample = L"NEVER DRAW PRIVATE APP TEXT";
    render(visual, keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state);
    const auto native_raster = visual.frame().rasterizations;
    state.practice_sample = {};
    render(visual, keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state);
    check(visual.frame().rasterizations == native_raster, "native suggestion view cannot echo a practice sample or app text");
    inspect(visual.frame(), source.theme.high_contrast);
    if (!directory.empty()) { export_frame(visual, directory, L"prediction-native-on"); }
    state.prediction = {};
    for (auto hint : {L"Privacy unverified. Hold Down to enable",
            L"Word suggestions off: protected field", L"Word suggestions: privacy metadata unavailable"}) {
        state.prediction_hint = hint;
        render(visual, keyboard_visual_width, keyboard_prediction_height, practice.keyboard().model(), state);
        check(visual.frame().pixels[365 * keyboard_visual_width + 196] == 0, "native consent/protection/query-failure state never exposes candidate chips");
    }
    if (!directory.empty()) { export_frame(visual, directory, L"prediction-native-unavailable"); }
    std::cout << "PASS " << count << " prediction mode/theme/DPI frames; label fit/contrast, hidden chips, ownership and idle reuse\n";
}

void voice_frames(const std::filesystem::path& directory)
{
    FakeTheme theme;
    KeyboardVisual visual(&theme);
    unsigned cases = 0;
    for (unsigned palette = 0; palette < 3; ++palette) {
        theme.theme = {};
        theme.theme.dark = palette == 1;
        theme.theme.high_contrast = palette == 2;
        if (palette == 2) {
            theme.theme.window = 0; theme.theme.window_text = 0xFFFFFF;
            theme.theme.highlight = 0xFFFF00; theme.theme.highlight_text = 0;
        }
        visual.system_theme_changed(WM_SETTINGCHANGE);
        for (int percent : {100, 150, 200}) {
            const int width = keyboard_visual_width * percent / 100;
            const int height = keyboard_visual_height * percent / 100;
            const float scale = percent / 100.0f;
            for (unsigned mode = 0; mode < 3; ++mode) {
                Fixture fixture(mode ? KeyboardInputMode::OneHanded : KeyboardInputMode::FaceButtons);
                fixture.select(mode == 2 ? 7 : 1, 0);
                if (mode == 2) { fixture.center(); }
                KeyboardVisualState state{true, false, {}, {}};
                render(visual, width, height, fixture.model, state);
                const std::vector<std::uint32_t> idle(visual.frame().pixels.begin(), visual.frame().pixels.end());
                const auto idle_rasters = visual.frame().rasterizations;
                state.voice = {VoicePhase::Idle, 999, L"Ignored idle hint"};
                render(visual, width, height, fixture.model, state);
                check(visual.frame().rasterizations == idle_rasters
                    && std::equal(idle.begin(), idle.end(), visual.frame().pixels.begin()),
                    "explicit idle voice level/hint must be pixel-identical and not dirty the cache");
                for (const auto phase : {VoicePhase::Loading, VoicePhase::Listening, VoicePhase::Transcribing, VoicePhase::Error}) {
                    state.voice = {phase, 0.5f, phase == VoicePhase::Error ? L"Model unavailable" : L""};
                    render(visual, width, height, fixture.model, state);
                    inspect(visual.frame(), theme.theme.high_contrast, mode == 2, true);
                    check(!std::equal(idle.begin(), idle.end(), visual.frame().pixels.begin()), "active voice must visibly change center");
                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            const bool center = x >= 150 * scale && x <= 242 * scale && y >= 119 * scale && y <= 187 * scale;
                            const bool footer = x >= 42 * scale && x <= 350 * scale && y >= 294 * scale && y <= 319 * scale;
                            if (!center && !footer) {
                                const auto index = static_cast<std::size_t>(y) * width + x;
                                check(idle[index] == visual.frame().pixels[index], "voice must not change character layout or pixels outside center/hint");
                            }
                        }
                    }
                    if (!directory.empty() && percent != 200 && mode != 1) {
                        export_frame(visual, directory, L"voice-" + std::to_wstring(palette) + L"-"
                            + std::to_wstring(static_cast<int>(phase)) + L"-" + std::to_wstring(mode) + L"-" + std::to_wstring(percent));
                    }
                    const auto before = visual.frame();
                    render(visual, width, height, fixture.model, state);
                    check(visual.frame().rasterizations == before.rasterizations
                        && visual.frame().surface_allocations == before.surface_allocations, "unchanged voice view must not rerasterize/allocate");
                    ++cases;
                }
                state.voice = {};
                render(visual, width, height, fixture.model, state);
                check(std::equal(idle.begin(), idle.end(), visual.frame().pixels.begin()), "ending voice restores exact idle pixels");
            }
        }
    }
    Daisywheel model;
    KeyboardVisualState state{true, false, {}, {}, {VoicePhase::Listening, 0, {}}};
    render(visual, keyboard_visual_width, keyboard_visual_height, model, state);
    const auto zero = visual.frame().rasterizations;
    for (const float value : {-1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        state.voice.level = value;
        render(visual, keyboard_visual_width, keyboard_visual_height, model, state);
        check(visual.frame().rasterizations == zero, "nonfinite/negative levels normalize to zero before caching");
    }
    state.voice.level = 0.501f;
    render(visual, keyboard_visual_width, keyboard_visual_height, model, state);
    const auto middle = visual.frame().rasterizations;
    state.voice.level = 0.504f;
    render(visual, keyboard_visual_width, keyboard_visual_height, model, state);
    check(visual.frame().rasterizations == middle, "small level jitter stays in the same quantized bucket");
    state.voice.level = 1;
    render(visual, keyboard_visual_width, keyboard_visual_height, model, state);
    const auto maximum = visual.frame().rasterizations;
    state.voice.level = 1e30f;
    render(visual, keyboard_visual_width, keyboard_visual_height, model, state);
    check(visual.frame().rasterizations == maximum, "large finite level clamps to one");
    std::wstring hint = L"First hint";
    state.voice.hint = hint;
    render(visual, keyboard_visual_width, keyboard_visual_height, model, state);
    const auto first_hint = visual.frame().rasterizations;
    hint[0] = L'N';
    render(visual, keyboard_visual_width, keyboard_visual_height, model, state);
    check(visual.frame().rasterizations == first_hint + 1, "cached hint must own its old text, not alias caller storage");
    std::cout << "PASS " << cases << " voice phase/theme/mode/DPI frames; idle equivalence, ink exclusion, bounded levels and owning hint cache\n";
}

void export_frame(KeyboardVisual& visual, const std::filesystem::path& directory, const std::wstring& name)
{
    for (const auto& variant : std::array<std::pair<std::wstring, std::optional<std::uint32_t>>, 3>{{
            {L"-alpha", {}}, {L"-light", 0xE8EAED}, {L"-dark", 0x202733}}}) {
        const auto path = directory / (name + variant.first + L".png");
        if (!visual.save_png(path.c_str(), variant.second)) {
            std::wcerr << visual.error() << L'\n';
            throw std::runtime_error("PNG export failed");
        }
    }
    // BMPV5 contains the exact top-down premultiplied bytes used by UpdateLayeredWindow.
    const auto frame = visual.frame();
    BITMAPV5HEADER info{};
    info.bV5Size = sizeof(info);
    info.bV5Width = frame.width;
    info.bV5Height = -frame.height;
    info.bV5Planes = 1;
    info.bV5BitCount = 32;
    info.bV5Compression = BI_BITFIELDS;
    info.bV5SizeImage = static_cast<DWORD>(frame.pixels.size_bytes());
    info.bV5RedMask = 0x00FF0000;
    info.bV5GreenMask = 0x0000FF00;
    info.bV5BlueMask = 0x000000FF;
    info.bV5AlphaMask = 0xFF000000;
    info.bV5CSType = LCS_sRGB;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.bV5SizeImage;
    std::ofstream output(directory / (name + L"-premultiplied.bmp"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(frame.pixels.data()), static_cast<std::streamsize>(frame.pixels.size_bytes()));
    check(output.good(), "exact premultiplied BMP export failed");
}

void hidden_presentation()
{
    const auto original_foreground = GetForegroundWindow();
    const auto original_focus = GetFocus();
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"GamepadKeyboardVisualHiddenTest";
    check(RegisterClassW(&type) != 0, "test class registration failed");
    const auto window = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        type.lpszClassName, L"No-input hidden renderer test", WS_POPUP, -32000, -32000,
        keyboard_visual_width, keyboard_visual_height, nullptr, nullptr, type.hInstance, nullptr);
    check(window != nullptr, "hidden layered HWND creation failed");
    KeyboardVisual visual;
    Daisywheel model;
    const bool painted = visual.paint(window, model, {true, false, {}, {}});
    const auto first_frame = visual.frame();
    for (int i = 0; i < 40; ++i) {
        InvalidateRect(window, nullptr, FALSE);
        check(visual.paint(window, model, {true, false, {}, {}}), "cached layered presentation failed");
    }
    check(visual.frame().rasterizations == first_frame.rasterizations, "WM_PAINT must reuse unchanged pixels");
    check(GetUpdateRect(window, nullptr, FALSE) == FALSE, "layered paint must validate its update region");
    const bool focus_unchanged = original_foreground == GetForegroundWindow() && original_focus == GetFocus();
    DestroyWindow(window);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    if (!painted) { std::wcerr << visual.error() << L'\n'; }
    check(painted, "UpdateLayeredWindow must accept the actual premultiplied frame");
    check(focus_unchanged, "layered presentation must not change foreground or focus");
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        check(argc == 1 || argc == 2, "usage: gamepad_keyboard_visual_tests [owned-artifact-directory]");
        std::filesystem::path directory;
        if (argc == 2) {
            directory = argv[1];
            std::filesystem::create_directories(directory);
        }
        voice_frames(directory);
        prediction_frames(directory);
        FakeTheme source;
        KeyboardVisual visual(&source);
        unsigned inspected = 0;
        for (unsigned theme = 0; theme < 4; ++theme) {
        source.theme = {};
        source.theme.dark = theme == 1;
        source.theme.high_contrast = theme >= 2;
        if (theme == 3) {
            source.theme.window = 0;
            source.theme.window_text = 0xFFFFFF;
            source.theme.highlight = 0xFFFF00;
            source.theme.highlight_text = 0;
        }
        const bool hc = source.theme.high_contrast;
        const std::wstring theme_name = theme == 0 ? L"light-" : theme == 1 ? L"dark-" : theme == 2 ? L"hc-light-" : L"hc-dark-";
        visual.system_theme_changed(WM_SETTINGCHANGE);
        for (const int percent : {100, 150, 200}) {
            const int width = keyboard_visual_width * percent / 100;
            const int height = keyboard_visual_height * percent / 100;
            Daisywheel neutral;
            render(visual, width, height, neutral, {true, false, {}, {}});
            inspect(visual.frame(), hc);
            if (!directory.empty() && percent != 200) {
                export_frame(visual, directory, theme_name + L"normal-" + std::to_wstring(percent));
            }
            for (unsigned page = 0; page < 4; ++page) {
                for (unsigned group = 0; group < 8; ++group) {
                    Fixture fixture;
                    fixture.select(group, page);
                    render(visual, width, height, fixture.model, {true, false, {}, {}});
                    inspect(visual.frame(), hc);
                    ++inspected;
                    if (!directory.empty() && percent != 200 && group == 1 && (page == 0 || page == 2)) {
                        export_frame(visual, directory, theme_name + (page == 0 ? L"selected-" : L"symbols-") + std::to_wstring(percent));
                    }
                    Fixture single(KeyboardInputMode::OneHanded);
                    render(visual, width, height, single.model, {true, false, {}, {}});
                    const auto neutral_raster = visual.frame().rasterizations;
                    single.select(group, page);
                    check(!single.model.selected_petal() && single.model.highlighted_petal() == group,
                        "outward Group gesture highlights candidate before committing a group");
                    render(visual, width, height, single.model, {true, false, {}, {}});
                    check(visual.frame().rasterizations == neutral_raster + 1,
                        "pending Group highlight must invalidate the renderer cache");
                    inspect(visual.frame(), hc);
                    ++inspected;
                    const std::array<wchar_t, 4> actions{L'\u2423', L'\u232B', L'\u21BB', L'\u21B5'};
                    for (unsigned face = 0; face < 4; ++face) {
                        check(visual.frame().glyphs[28 + face].character == actions[face],
                            "PUA actions must be mapped to standard space/erase/cycle/Enter symbols");
                    }
                    const bool export_onehand = !directory.empty() && percent != 200 && page == 0 && (group == 1 || group == 7);
                    if (export_onehand) {
                        export_frame(visual, directory, theme_name + (group == 7 ? L"actions-group-" : L"onehand-group-") + std::to_wstring(percent));
                    }
                    const auto candidate_raster = visual.frame().rasterizations;
                    const std::vector<std::uint32_t> candidate_pixels(visual.frame().pixels.begin(), visual.frame().pixels.end());
                    single.select((group + 1) % 8, page);
                    check(!single.model.selected_petal() && single.model.highlighted_petal() == (group + 1) % 8,
                        "moving the outward stick updates only the pending Group candidate");
                    render(visual, width, height, single.model, {true, false, {}, {}});
                    check(visual.frame().rasterizations == candidate_raster + 1
                        && !std::equal(candidate_pixels.begin(), candidate_pixels.end(), visual.frame().pixels.begin()),
                        "candidate-only change must immediately update visible highlight pixels");
                    single.select(group, page);
                    render(visual, width, height, single.model, {true, false, {}, {}});
                    single.center();
                    check(single.model.flick_stage() == KeyboardFlickStage::Character, "centering must enter character stage");
                    check(single.model.selected_petal() == group, "centering must commit requested clockwise group");
                    const auto group_raster = visual.frame().rasterizations;
                    render(visual, width, height, single.model, {true, false, {}, {}});
                    check(visual.frame().rasterizations == group_raster + 1, "stage transition must refresh even with same group/page");
                    inspect(visual.frame(), hc, true);
                    ++inspected;
                    if (export_onehand) {
                        export_frame(visual, directory, theme_name + (group == 7 ? L"actions-character-" : L"onehand-character-") + std::to_wstring(percent));
                    }
                    for (unsigned face = 0; face < 4; ++face) {
                        single.face(face);
                        const auto previous_raster = visual.frame().rasterizations;
                        render(visual, width, height, single.model, {true, false, {}, {}});
                        check(visual.frame().rasterizations == previous_raster + 1,
                            "candidate face change must refresh same group/page/stage");
                        inspect(visual.frame(), hc, true);
                        ++inspected;
                        if (export_onehand && face == 1) {
                            export_frame(visual, directory, theme_name + (group == 7 ? L"actions-candidate-" : L"onehand-candidate-") + std::to_wstring(percent));
                        }
                    }
                    const auto idle_frame = visual.frame();
                    render(visual, width, height, single.model, {true, false, {}, {}});
                    check(visual.frame().rasterizations == idle_frame.rasterizations
                        && visual.frame().pixels.data() == idle_frame.pixels.data(),
                        "idle character stage must reuse exact frame");
                    single.center();
                    check(single.model.flick_stage() == KeyboardFlickStage::Group, "character commit must return to group stage");
                    check(!single.model.highlighted_petal(), "return to Group must clear the previous highlight");
                    render(visual, width, height, single.model, {true, false, {}, {}});
                    inspect(visual.frame(), hc);
                }
            }
            render(visual, width, height, neutral, {false, false, L"Typing stopped: focus changed. View closes.", {}});
            inspect(visual.frame(), hc);
            if (!directory.empty() && percent != 200) {
                export_frame(visual, directory, theme_name + L"error-" + std::to_wstring(percent));
            }
            render(visual, width, height, neutral, {true, false, {}, {}});
            const auto before = visual.frame();
            const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            const auto reads = source.reads;
            for (int repeat = 0; repeat < 200; ++repeat) {
                render(visual, width, height, neutral, {true, false, {}, {}});
            }
            const auto after = visual.frame();
            check(before.pixels.data() == after.pixels.data() && before.rasterizations == after.rasterizations
                && before.surface_allocations == after.surface_allocations, "idle state must not allocate or rasterize");
            check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == gdi, "idle state must not leak GDI resources");
            check(source.reads == reads, "idle render must not poll system theme");
        }
        }
        std::cout << "PASS " << inspected << " selected/page/DPI layouts: actual ink nonoverlap and visibility, alpha, corners, shadow\n";
        std::cout << "PASS 2400 idle renders: same pixels, no rasterization or theme reads, stable GDI handles\n";
        source.theme = {};
        visual.system_theme_changed(WM_THEMECHANGED);
        Daisywheel model;
        check(!visual.render(0, 350, model, {}) && !visual.error().empty(), "invalid dimensions must report failure");
        render(visual, keyboard_visual_width, keyboard_visual_height, model, {false, true, std::wstring(500, L'W'), {}});
        inspect(visual.frame());
        const auto before_theme = visual.frame();
        const auto before_reads = source.reads;
        source.theme.dark = true;
        render(visual, keyboard_visual_width, keyboard_visual_height, model, {false, true, std::wstring(500, L'W'), {}});
        check(source.reads == before_reads && visual.frame().rasterizations == before_theme.rasterizations,
            "theme source changes alone must not cause polling");
        visual.system_theme_changed(WM_THEMECHANGED);
        render(visual, keyboard_visual_width, keyboard_visual_height, model, {false, true, std::wstring(500, L'W'), {}});
        check(source.reads == before_reads + 1 && visual.frame().rasterizations == before_theme.rasterizations + 1
            && visual.frame().surface_allocations == before_theme.surface_allocations,
            "theme notification must update palette once without reallocating surfaces");
        inspect(visual.frame());
        visual.system_theme_changed(WM_SYSCOLORCHANGE);
        render(visual, keyboard_visual_width, keyboard_visual_height, model, {false, true, std::wstring(500, L'W'), {}});
        check(visual.frame().rasterizations == before_theme.rasterizations + 1,
            "unchanged palette notification must not rerasterize");
        source.result = E_ACCESSDENIED;
        visual.system_theme_changed(WM_SETTINGCHANGE);
        check(!visual.render(keyboard_visual_width, keyboard_visual_height, model, {}) && !visual.error().empty(),
            "theme read error must be explicit, not stale success");
        source.result = S_OK;
        Fixture private_sample;
        private_sample.select(0, 0);
        render(visual, keyboard_visual_width, keyboard_visual_height, private_sample.model, {true, false, {}, L"Untrusted target title"});
        const auto before_sample = visual.frame().rasterizations;
        private_sample.snapshot.slots[0].buttons = 0x1000;
        private_sample.tick();
        check(!private_sample.model.text().empty(), "preview sample fixture must contain a character");
        render(visual, keyboard_visual_width, keyboard_visual_height, private_sample.model, {true, false, {}, L"A different untrusted title"});
        check(visual.frame().rasterizations == before_sample, "production visual must not render target title or local draft");
        hidden_presentation();
        const DWORD initial_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 10; ++i) {
            KeyboardVisual lifecycle;
            render(lifecycle, keyboard_visual_width, keyboard_visual_height, model, {true, false, {}, {}});
        }
        check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == initial_gdi, "renderer destruction must release all GDI resources");
        std::cout << "PASS physical diamonds, errors, bounded status, no production draft, cached layered presentation, unchanged focus and resource shutdown\n";
        std::cout << "PASS light/dark/high-contrast palettes, transparent center/contour, glyph contrast and fake live theme invalidation\n";
        std::cout << "PASS outward Group candidate highlighting before commit, candidate-only cache invalidation and recenter clearing\n";
        std::cout << "Final measured character ink: widest span " << widest_span << " DIP, minimum pairwise gap "
            << smallest_gap << " DIP; group glyphs 20 DIP, character-stage glyphs 28 DIP\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
