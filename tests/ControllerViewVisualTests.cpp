#include "ControllerViewVisual.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace overworld;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
bool overlaps(ControllerViewRect a, ControllerViewRect b)
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
bool contains(ControllerViewRect outer, ControllerViewRect inner)
{
    constexpr float tolerance = .1f;
    return inner.left >= outer.left - tolerance && inner.top >= outer.top - tolerance
        && inner.right <= outer.right + tolerance && inner.bottom <= outer.bottom + tolerance;
}
struct PixelRect { int left, top, right, bottom; };
PixelRect pixel_rect(ControllerViewRect rect, int width, int height)
{
    return {std::clamp(static_cast<int>(std::floor(rect.left)), 0, width),
        std::clamp(static_cast<int>(std::floor(rect.top)), 0, height),
        std::clamp(static_cast<int>(std::ceil(rect.right)), 0, width),
        std::clamp(static_cast<int>(std::ceil(rect.bottom)), 0, height)};
}
ControllerViewRect bounds(const ControllerViewVisualFrame& frame, ControllerViewControl control)
{
    const auto found = std::find_if(frame.controls.begin(), frame.controls.end(),
        [&](const ControllerViewControlBounds& entry) { return entry.control == control; });
    check(found != frame.controls.end(), "every supported control has testable geometry");
    return found->bounds;
}
std::size_t changes(const ControllerViewVisualFrame& frame, std::span<const std::uint32_t> before,
    ControllerViewRect region)
{
    check(before.size() == frame.pixels.size(), "pixel comparisons use equal dimensions");
    const auto rect = pixel_rect(region, frame.width, frame.height);
    std::size_t changed = 0;
    for (int y = rect.top; y < rect.bottom; ++y) {
        for (int x = rect.left; x < rect.right; ++x) {
            const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x);
            if (frame.pixels[index] != before[index]) { ++changed; }
        }
    }
    return changed;
}
void inspect(const ControllerViewVisualFrame& frame)
{
    check(frame.width > 0 && frame.height > 0 && !frame.pixels.empty(), "actual offscreen pixels exist");
    check(frame.controls.size() == 16, "all 14 reported buttons and two analog triggers are drawn; no Guide input");
    for (const auto pixel : frame.pixels) {
        const auto alpha = pixel >> 24;
        check(((pixel >> 16) & 255) <= alpha && ((pixel >> 8) & 255) <= alpha && (pixel & 255) <= alpha,
            "every pixel must be premultiplied BGRA");
    }
    const ControllerViewRect canvas{0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height)};
    for (const auto& control : frame.controls) { check(contains(canvas, control.bounds), "all controls remain visible"); }
    const float scale = std::min(static_cast<float>(frame.width) / controller_view_width,
        static_cast<float>(frame.height) / controller_view_height);
    const float left = (static_cast<float>(frame.width) - controller_view_width * scale) / 2;
    const float top = (static_cast<float>(frame.height) - controller_view_height * scale) / 2;
    struct Position { ControllerViewControl id; float x, y; };
    constexpr std::array<Position, 16> expected{{
        {ControllerViewControl::A, 231, 116}, {ControllerViewControl::B, 249, 98},
        {ControllerViewControl::X, 213, 98}, {ControllerViewControl::Y, 231, 80},
        {ControllerViewControl::LeftStick, 88, 97}, {ControllerViewControl::RightStick, 193, 145},
        {ControllerViewControl::LeftShoulder, 91, 48}, {ControllerViewControl::RightShoulder, 229, 48},
        {ControllerViewControl::LeftTrigger, 91, 33}, {ControllerViewControl::RightTrigger, 229, 33},
        {ControllerViewControl::View, 146, 106}, {ControllerViewControl::Menu, 179, 106},
        {ControllerViewControl::Up, 124, 132.5f}, {ControllerViewControl::Down, 124, 157.5f},
        {ControllerViewControl::Left, 111.5f, 145}, {ControllerViewControl::Right, 136.5f, 145}
    }};
    for (const auto& position : expected) {
        const auto actual = bounds(frame, position.id);
        check(std::abs((actual.left + actual.right) / 2 - (left + position.x * scale)) < .01f
            && std::abs((actual.top + actual.bottom) / 2 - (top + position.y * scale)) < .01f,
            "physical control positions remain Xbox-like: offset sticks, A bottom, B right, X left, Y top");
    }
    const auto body = frame.body_bounds;
    const float ratio = (body.right - body.left) / (body.bottom - body.top);
    check(ratio >= 1.55f && ratio <= 1.7f, "actual D2D silhouette has compact body proportions, not a stretched outline");
    check(contains(canvas, {body.left - 12 * scale, body.top - 12 * scale,
        body.right + 12 * scale, body.bottom + 12 * scale}), "body retains quiet clearance from the canvas edges");
    const auto pixel = [&](int x, int y) {
        return frame.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)];
    };
    const auto background = frame.pixels.front();
    const auto area = pixel_rect({body.left - 2 * scale, body.top - 2 * scale,
        body.right + 2 * scale, body.bottom + 2 * scale}, frame.width, frame.height);
    PixelRect ink{frame.width, frame.height, 0, 0};
    for (int y = area.top; y < area.bottom; ++y) {
        for (int x = area.left; x < area.right; ++x) {
            if (pixel(x, y) != background) {
                ink.left = std::min(ink.left, x); ink.right = std::max(ink.right, x + 1);
                ink.top = std::min(ink.top, y); ink.bottom = std::max(ink.bottom, y + 1);
            }
        }
    }
    const float raster_ratio = static_cast<float>(ink.right - ink.left) / static_cast<float>(ink.bottom - ink.top);
    check(raster_ratio >= 1.55f && raster_ratio <= 1.7f, "actual visible silhouette pixels retain compact proportions");
    check(std::abs(static_cast<float>(ink.left) - body.left) <= 3 * scale
        && std::abs(static_cast<float>(ink.right) - body.right) <= 3 * scale
        && std::abs(static_cast<float>(ink.bottom) - body.bottom) <= 3 * scale,
        "visible pixels reach measured geometry rather than reporting an invisible outline");
    const auto dpad = [](ControllerViewControl id) {
        return id == ControllerViewControl::Up || id == ControllerViewControl::Down
            || id == ControllerViewControl::Left || id == ControllerViewControl::Right;
    };
    const auto face = [](ControllerViewControl id) {
        return id == ControllerViewControl::A || id == ControllerViewControl::B
            || id == ControllerViewControl::X || id == ControllerViewControl::Y;
    };
    for (std::size_t i = 0; i < frame.controls.size(); ++i) {
        const auto& control = frame.controls[i];
        for (std::size_t j = i + 1; j < frame.controls.size(); ++j) {
            const auto& other = frame.controls[j];
            if (dpad(control.control) && dpad(other.control)) { continue; }
            if (face(control.control) && face(other.control)) {
                const float dx = (control.bounds.left + control.bounds.right - other.bounds.left - other.bounds.right) / 2;
                const float dy = (control.bounds.top + control.bounds.bottom - other.bounds.top - other.bounds.bottom) / 2;
                check(std::sqrt(dx * dx + dy * dy) > 22 * scale, "tight face diamond preserves visible circle clearance");
                continue;
            }
            check(!overlaps(control.bounds, other.bounds), "independent controls retain clearance even in the compact layout");
        }
    }
    for (const auto& stick : frame.sticks) {
        const float dx = stick.cap_x - stick.center_x, dy = stick.cap_y - stick.center_y;
        check(std::sqrt(dx * dx + dy * dy) + stick.cap_radius + scale <= stick.well_radius,
            "circular thumb caps remain inside their wells with clearance at exact diagonal extrema");
        check(stick.travel >= 4.5f && stick.cap_radius >= 10, "minimum-size cap displacement and pressed area remain visible");
    }
    for (const auto region : {ControllerViewRect{0, 0, 320, 26}, ControllerViewRect{0, 231, 320, 240},
        ControllerViewRect{0, 0, 25, 240}, ControllerViewRect{295, 0, 320, 240}}) {
        const auto rect = pixel_rect({left + region.left * scale, top + region.top * scale,
            left + region.right * scale, top + region.bottom * scale}, frame.width, frame.height);
        for (int y = rect.top; y < rect.bottom; ++y) {
            for (int x = rect.left; x < rect.right; ++x) {
                check(pixel(x, y) == background, "quiet margins have no diagnostic header, footer, labels or decoration");
            }
        }
    }
}
void alpha_contract(const ControllerViewVisualFrame& frame, const KeyboardTheme& theme)
{
    const float scale = std::min(static_cast<float>(frame.width) / controller_view_width,
        static_cast<float>(frame.height) / controller_view_height);
    const float left = (static_cast<float>(frame.width) - controller_view_width * scale) / 2;
    const float top = (static_cast<float>(frame.height) - controller_view_height * scale) / 2;
    const auto pixel = [&](float x, float y) {
        const auto column = static_cast<std::size_t>(left + x * scale);
        const auto row = static_cast<std::size_t>(top + y * scale);
        return frame.pixels[row * static_cast<std::size_t>(frame.width) + column];
    };
    for (const auto point : {std::pair{5.0f, 5.0f}, std::pair{160.0f, 40.0f}, std::pair{91.0f, 40.0f},
        std::pair{229.0f, 40.0f}, std::pair{120.0f, 48.0f}, std::pair{160.0f, 205.0f}}) {
        check(pixel(point.first, point.second) == 0, "outside body, between tracks, and below the saddle are truly transparent");
    }
    const auto body = pixel(160, 85);
    const auto alpha = body >> 24;
    check(alpha == (theme.high_contrast ? 255u : 230u), "body is 230/255 opaque normally and fully opaque in high contrast");
    const auto palette = keyboard_palette(theme);
    for (const unsigned shift : {0u, 8u, 16u}) {
        const int expected = static_cast<int>((((palette.surface >> shift) & 255) * alpha + 127) / 255);
        const int actual = static_cast<int>((body >> shift) & 255);
        check(std::abs(actual - expected) <= 1, "translucent body uses correctly premultiplied shared keyboard surface colors");
    }
    for (const auto point : {std::pair{88.0f, 97.0f}, std::pair{193.0f, 145.0f},
        std::pair{231.0f, 116.0f}, std::pair{231.0f, 80.0f}, std::pair{91.0f, 48.0f}}) {
        check((pixel(point.first, point.second) >> 24) == 255, "thumb caps and physical controls remain fully opaque and readable");
    }
}
void render(ControllerViewVisual& visual, int width, int height, const ControllerViewState& state, const KeyboardTheme& theme)
{
    if (!visual.render(width, height, state, theme)) {
        std::wcerr << visual.error() << L'\n';
        throw std::runtime_error("synthetic controller render failed");
    }
    alpha_contract(visual.frame(), theme);
}
void stable(ControllerViewVisual& visual, int width, int height, const ControllerViewState& state, const KeyboardTheme& theme)
{
    const auto before = visual.frame();
    for (unsigned repeat = 0; repeat < 50; ++repeat) { render(visual, width, height, state, theme); }
    const auto after = visual.frame();
    check(before.rasterizations == after.rasterizations && before.surface_allocations == after.surface_allocations
        && before.pixels.data() == after.pixels.data(),
        "idle frames reuse the actual bitmap and resources without rasterization");
}
ControllerViewState neutral()
{
    ControllerViewState state;
    state.signal = ControllerViewSignal::Ready;
    state.route = ControllerViewRoute::Desktop;
    state.slot = 0;
    return state;
}
struct Button { std::uint16_t bit; ControllerViewControl control; };
constexpr std::array<Button, 14> buttons{{
    {0x0001, ControllerViewControl::Up}, {0x0002, ControllerViewControl::Down},
    {0x0004, ControllerViewControl::Left}, {0x0008, ControllerViewControl::Right},
    {0x0010, ControllerViewControl::Menu}, {0x0020, ControllerViewControl::View},
    {0x0040, ControllerViewControl::LeftStick}, {0x0080, ControllerViewControl::RightStick},
    {0x0100, ControllerViewControl::LeftShoulder}, {0x0200, ControllerViewControl::RightShoulder},
    {0x1000, ControllerViewControl::A}, {0x2000, ControllerViewControl::B},
    {0x4000, ControllerViewControl::X}, {0x8000, ControllerViewControl::Y}
}};
void digital(ControllerViewVisual& visual, int width, int height, const KeyboardTheme& theme)
{
    const auto idle = neutral();
    render(visual, width, height, idle, theme);
    const auto baseline = visual.frame();
    const std::vector<std::uint32_t> pixels(baseline.pixels.begin(), baseline.pixels.end());
    const auto allocations = baseline.surface_allocations;
    for (const auto& button : buttons) {
        auto state = idle;
        state.buttons = button.bit;
        render(visual, width, height, state, theme);
        const auto frame = visual.frame();
        const auto region = bounds(frame, button.control);
        const auto changed = changes(frame, pixels, region);
        check(changed > 8, "each reported button changes pixels at its expected control");
        const auto total = changes(frame, pixels, {0, 0, static_cast<float>(width), static_cast<float>(height)});
        check(total == changed, "a single digital input must not highlight another control or invent Guide input");
        check(frame.surface_allocations == allocations, "button changes reuse retained surfaces");
        const int center_x = static_cast<int>((region.left + region.right) / 2);
        const int center_y = static_cast<int>((region.top + region.bottom) / 2);
        check((frame.pixels[static_cast<std::size_t>(center_y) * static_cast<std::size_t>(width)
            + static_cast<std::size_t>(center_x)] & 0xffffff) == keyboard_palette(theme).selected,
            "pressed controls show the shared theme accent without button labels");
        render(visual, width, height, idle, theme);
        check(std::equal(pixels.begin(), pixels.end(), visual.frame().pixels.begin()),
            "release immediately restores exact neutral pixels without held animations");
    }
    auto reserved = idle;
    reserved.buttons = 0x0c00;
    const auto before = visual.frame().rasterizations;
    render(visual, width, height, reserved, theme);
    check(visual.frame().rasterizations == before
        && std::equal(pixels.begin(), pixels.end(), visual.frame().pixels.begin()),
        "reserved Guide/unreported bits never create a highlight");
}
void sticks(ControllerViewVisual& visual, int width, int height, const KeyboardTheme& theme)
{
    struct Position { std::int16_t x, y; float expected_x, expected_y; };
    constexpr std::array<Position, 9> positions{{
        {0, 32767, 0, 1}, {32767, 0, 1, 0}, {0, -32768, 0, -1}, {-32768, 0, -1, 0},
        {32767, 32767, 1, 1}, {32767, -32768, 1, -1},
        {-32768, 32767, -1, 1}, {-32768, -32768, -1, -1},
        {16384, -16384, 16384.0f / 32767.0f, -.5f}
    }};
    const auto idle = neutral();
    render(visual, width, height, idle, theme);
    const auto baseline = visual.frame();
    const std::vector<std::uint32_t> pixels(baseline.pixels.begin(), baseline.pixels.end());
    for (unsigned index = 0; index < 2; ++index) {
        for (const auto& position : positions) {
            auto state = idle;
            if (index == 0) { state.left_x = position.x; state.left_y = position.y; }
            else { state.right_x = position.x; state.right_y = position.y; }
            render(visual, width, height, state, theme);
            const auto frame = visual.frame();
            const auto& stick = frame.sticks[index];
            check(std::abs(stick.cap_x - stick.center_x - position.expected_x * stick.travel) < .001f
                && std::abs(stick.cap_y - stick.center_y + position.expected_y * stick.travel) < .001f,
                "stick caps follow exact asymmetric raw endpoints, with upward Y pointing up and no smoothing");
            const auto region = bounds(frame, index == 0 ? ControllerViewControl::LeftStick : ControllerViewControl::RightStick);
            const auto changed = changes(frame, pixels, region);
            check(changed > 20, "every cardinal, diagonal and partial stick changes actual pixels");
            check(changes(frame, pixels, {0, 0, static_cast<float>(width), static_cast<float>(height)}) == changed,
                "axis changes remain confined to the corresponding stick");
            const auto other = frame.sticks[1 - index];
            check(other.cap_x == other.center_x && other.cap_y == other.center_y, "one stick cannot move the other");
        }
    }
    render(visual, width, height, idle, theme);
    check(std::equal(pixels.begin(), pixels.end(), visual.frame().pixels.begin()), "stick release resets immediately");
}
void triggers(ControllerViewVisual& visual, int width, int height, const KeyboardTheme& theme)
{
    const auto idle = neutral();
    render(visual, width, height, idle, theme);
    const auto baseline = visual.frame();
    const std::vector<std::uint32_t> pixels(baseline.pixels.begin(), baseline.pixels.end());
    for (unsigned side = 0; side < 2; ++side) {
        std::size_t previous = 0;
        for (const auto level : {std::uint8_t{64}, std::uint8_t{128}, std::uint8_t{255}}) {
            auto state = idle;
            if (side == 0) { state.left_trigger = level; }
            else { state.right_trigger = level; }
            render(visual, width, height, state, theme);
            const auto frame = visual.frame();
            const auto region = bounds(frame, side == 0 ? ControllerViewControl::LeftTrigger : ControllerViewControl::RightTrigger);
            const auto changed = changes(frame, pixels, region);
            check(changed > previous, "raw partial/full trigger values produce monotonically increasing visible fill");
            check(changes(frame, pixels, {0, 0, static_cast<float>(width), static_cast<float>(height)}) == changed,
                "analog triggers do not highlight shoulders or the other trigger");
            previous = changed;
        }
    }
    render(visual, width, height, idle, theme);
    check(std::equal(pixels.begin(), pixels.end(), visual.frame().pixels.begin()), "trigger release resets immediately");
}
ControllerViewState all_held()
{
    auto state = neutral();
    state.buttons = standard_buttons;
    state.left_trigger = 128; state.right_trigger = 255;
    state.left_x = -32768; state.left_y = 32767;
    state.right_x = 32767; state.right_y = -32768;
    return state;
}
void signals(ControllerViewVisual& visual, int width, int height, const KeyboardTheme& theme)
{
    for (const auto signal : {ControllerViewSignal::Waiting, ControllerViewSignal::Stale,
        ControllerViewSignal::Disconnected, ControllerViewSignal::ReadError, ControllerViewSignal::Ambiguous}) {
        auto held = all_held();
        held.signal = signal;
        render(visual, width, height, held, theme);
        inspect(visual.frame());
        const std::vector<std::uint32_t> actual(visual.frame().pixels.begin(), visual.frame().pixels.end());
        render(visual, width, height, controller_view_clear(held, signal), theme);
        check(std::equal(actual.begin(), actual.end(), visual.frame().pixels.begin()),
            "every non-ready signal explicitly clears buttons, axes, triggers and route even for malformed held fixtures");
        stable(visual, width, height, held, theme);
    }
    const ControllerViewFrame sample{all_held(), 1000, 9};
    auto stale = controller_view_present(sample, 1101);
    check(stale.signal == ControllerViewSignal::Stale, "old samples age independently of new publications");
    render(visual, width, height, stale, theme);
    const std::vector<std::uint32_t> stale_pixels(visual.frame().pixels.begin(), visual.frame().pixels.end());
    render(visual, width, height, controller_view_present(sample, 999), theme);
    check(std::equal(stale_pixels.begin(), stale_pixels.end(), visual.frame().pixels.begin()), "future timestamps render cleared, not held");
    render(visual, width, height, controller_view_present(sample, 1100), theme);
    check(!std::equal(stale_pixels.begin(), stale_pixels.end(), visual.frame().pixels.begin()), "100ms is the last fresh sample boundary");
    render(visual, width, height, neutral(), theme);
    const std::vector<std::uint32_t> route_pixels(visual.frame().pixels.begin(), visual.frame().pixels.end());
    const auto route_rasterizations = visual.frame().rasterizations;
    for (const auto route : {ControllerViewRoute::Observing, ControllerViewRoute::Desktop,
        ControllerViewRoute::Keyboard, ControllerViewRoute::Paused}) {
        auto state = neutral(); state.route = route;
        render(visual, width, height, state, theme);
        inspect(visual.frame());
        check(visual.frame().rasterizations == route_rasterizations
            && std::equal(route_pixels.begin(), route_pixels.end(), visual.frame().pixels.begin()),
            "route-only metadata never adds canvas labels or changes the controller artwork");
        const auto before = visual.frame().rasterizations;
        state.slot = 3;
        render(visual, width, height, state, theme);
        check(visual.frame().rasterizations == before, "accessible slot-only changes do not redraw a text-free canvas");
    }
}
void save(ControllerViewVisual& visual, const std::filesystem::path& path)
{
    if (!visual.save_png(path.c_str())) {
        std::wcerr << visual.error() << L'\n'; throw std::runtime_error("synthetic PNG export failed");
    }
    check(std::filesystem::file_size(path) > 100, "PNG artifacts are persistent and nonempty");
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> decoded;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (SUCCEEDED(hr)) { hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = decoder->GetFrame(0, decoded.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = factory->CreateFormatConverter(converter.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = converter->Initialize(decoded.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom); }
    const auto frame = visual.frame();
    std::vector<std::uint32_t> roundtrip(frame.pixels.size());
    if (SUCCEEDED(hr)) { hr = converter->CopyPixels(nullptr, static_cast<UINT>(frame.width) * 4,
        static_cast<UINT>(roundtrip.size() * 4), reinterpret_cast<BYTE*>(roundtrip.data())); }
    check(SUCCEEDED(hr), "exported PNG decodes through WIC into the actual premultiplied format");
    for (std::size_t index = 0; index < roundtrip.size(); ++index) {
        check((roundtrip[index] >> 24) == (frame.pixels[index] >> 24), "PNG preserves every alpha value, including the fully clear background");
        for (const unsigned shift : {0u, 8u, 16u}) {
            const int actual = static_cast<int>((roundtrip[index] >> shift) & 255);
            const int expected = static_cast<int>((frame.pixels[index] >> shift) & 255);
            check(std::abs(actual - expected) <= 1, "straight-alpha PNG export must not double-premultiply and darken transparent edges");
        }
    }
}
void run(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    KeyboardTheme light, dark, black, white;
    dark.dark = true;
    black.high_contrast = true;
    black.window = 0; black.window_text = 0xffffff; black.highlight = 0xffff00; black.highlight_text = 0;
    white.high_contrast = true;
    white.window = 0xffffff; white.window_text = 0; white.highlight = 0; white.highlight_text = 0xffffff;
    const std::array<KeyboardTheme, 4> themes{light, dark, black, white};
    const std::array<std::wstring_view, 4> names{L"light", L"dark", L"contrast-black", L"contrast-white"};
    unsigned cases = 0;
    for (unsigned theme_index = 0; theme_index < themes.size(); ++theme_index) {
        const auto& theme = themes[theme_index];
        for (const int dpi : {96, 144, 192}) {
            ControllerViewVisual visual;
            const int width = controller_view_width * dpi / 96, height = controller_view_height * dpi / 96;
            const auto idle = neutral();
            render(visual, width, height, idle, theme);
            inspect(visual.frame());
            check(visual.frame().pixels.front() == 0, "no rectangular background remains around the controller");
            stable(visual, width, height, idle, theme);
            digital(visual, width, height, theme);
            sticks(visual, width, height, theme);
            triggers(visual, width, height, theme);
            signals(visual, width, height, theme);
            render(visual, width, height, all_held(), theme);
            inspect(visual.frame());
            if (theme_index >= 2 && dpi == 144) { save(visual, directory / (std::wstring(names[theme_index]) + L"-active-144.png")); }
            if (theme_index < 2 && dpi == 96) {
                save(visual, directory / (std::wstring(names[theme_index]) + L"-active-96.png"));
                render(visual, width, height, idle, theme);
                save(visual, directory / (std::wstring(names[theme_index]) + L"-neutral-96.png"));
            }
            if (theme_index == 1 && dpi == 192) { save(visual, directory / L"dark-active-192.png"); }
            if (theme_index == 3 && dpi == 192) {
                render(visual, width, height, controller_view_clear(all_held(), ControllerViewSignal::Stale), theme);
                save(visual, directory / L"contrast-white-stale-192.png");
            }
            ++cases;
        }
    }
    ControllerViewVisual resized;
    for (const auto& theme : themes) {
        render(resized, controller_view_width, controller_view_height, neutral(), theme);
        inspect(resized.frame());
        check(resized.frame().surface_allocations == 1, "theme changes reuse the same native bitmap and brushes");
        check(resized.frame().pixels.front() == 0, "theme changes preserve transparent space around the controller");
    }
    for (const auto size : {std::pair{256, 192}, std::pair{700, 300}, std::pair{320, 700}, std::pair{320, 240}}) {
        render(resized, size.first, size.second, all_held(), light);
        inspect(resized.frame());
        stable(resized, size.first, size.second, all_held(), light);
    }
    for (unsigned theme_index = 0; theme_index < themes.size(); ++theme_index) {
        const auto& theme = themes[theme_index];
        digital(resized, controller_view_min_width, controller_view_min_height, theme);
        sticks(resized, controller_view_min_width, controller_view_min_height, theme);
        triggers(resized, controller_view_min_width, controller_view_min_height, theme);
        signals(resized, controller_view_min_width, controller_view_min_height, theme);
        render(resized, controller_view_min_width, controller_view_min_height, all_held(), theme);
        inspect(resized.frame());
        if (theme_index < 2) { save(resized, directory / (L"minimum-size-" + std::wstring(names[theme_index]) + L".png")); }
    }
    for (const auto size : {std::pair{0, 0}, std::pair{-1, 240}, std::pair{255, 192},
        std::pair{320, 191}, std::pair{2049, 240}, std::pair{320, 1537}}) {
        check(!resized.render(size.first, size.second, neutral(), light)
            && !resized.error().empty() && resized.frame().pixels.empty(), "invalid sizes fail explicitly without exposing old pixels");
    }
    render(resized, controller_view_width, controller_view_height, neutral(), light);
    check(resized.error().empty(), "a successful explicit rerender clears a prior size error");
    check(!resized.save_png(nullptr) && !resized.error().empty(), "invalid PNG destinations fail explicitly");
    std::cout << "Transparent controller view: " << cases << " theme/DPI combinations plus all four minimum-size themes; "
        "no text, compact silhouette, all inputs, staleness, clear outside alpha, translucent body, opaque controls, "
        "premultiplication, PNG alpha roundtrip and idle reuse passed. "
        "No window, reader or input boundary was created.\n";
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 3 || std::wstring_view(argv[1]) != L"--render-only" || !*argv[2]) {
        std::cerr << "Usage: overworld_controller_view_visual_tests --render-only <artifact-directory>\n";
        return 2;
    }
    try {
        run(std::filesystem::path(argv[2]));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Controller view synthetic rendering failed: " << error.what() << '\n';
        return 1;
    }
}
