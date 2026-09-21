#include "ControllerViewVisual.h"

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

namespace overworld {
namespace {
using Microsoft::WRL::ComPtr;
struct RenderKey {
    int width, height;
    ControllerViewState state;
    KeyboardTheme theme;
    bool operator==(const RenderKey&) const = default;
};
constexpr D2D1_POINT_2F point(float x, float y) { return {x, y}; }
D2D1_COLOR_F color(std::uint32_t rgb, float opacity = 1) { return D2D1::ColorF(rgb, opacity); }
}

class ControllerViewVisual::Resources {
public:
    ~Resources()
    {
        body.Reset(); brush.Reset(); target.Reset(); bitmap.Reset(); wic.Reset(); factory.Reset();
        if (uninitialize) { CoUninitialize(); }
    }
    HRESULT initialize()
    {
        if (initialized) { return S_OK; }
        if (!apartment_checked) {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) { return apartment; }
            uninitialize = SUCCEEDED(apartment);
            apartment_checked = true;
        }
        body.Reset(); wic.Reset(); factory.Reset();
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wic.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) { hr = factory->CreatePathGeometry(body.GetAddressOf()); }
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(hr)) { hr = body->Open(sink.GetAddressOf()); }
        if (SUCCEEDED(hr)) {
            // The body is designed at its own compact proportions; circles are
            // never stretched to fit a wider silhouette.
            sink->BeginFigure(point(103, 54), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddBezier({point(126, 52), point(139, 57), point(160, 57)});
            sink->AddBezier({point(181, 57), point(194, 52), point(217, 54)});
            sink->AddBezier({point(244, 51), point(257, 62), point(268, 91)});
            sink->AddBezier({point(279, 120), point(287, 153), point(289, 178)});
            sink->AddBezier({point(292, 201), point(285, 214), point(271, 214)});
            sink->AddBezier({point(254, 215), point(242, 188), point(227, 177)});
            sink->AddBezier({point(218, 170), point(203, 174), point(187, 179)});
            sink->AddBezier({point(178, 182), point(142, 182), point(133, 179)});
            sink->AddBezier({point(117, 174), point(102, 170), point(93, 177)});
            sink->AddBezier({point(78, 188), point(66, 215), point(49, 214)});
            sink->AddBezier({point(35, 214), point(28, 201), point(31, 178)});
            sink->AddBezier({point(33, 153), point(41, 120), point(52, 91)});
            sink->AddBezier({point(63, 62), point(76, 51), point(103, 54)});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            hr = sink->Close();
        }
        initialized = SUCCEEDED(hr);
        return hr;
    }
    HRESULT surface(int w, int h)
    {
        HRESULT hr = initialize();
        if (FAILED(hr)) { return hr; }
        if (width == w && height == h && target) { return S_OK; }
        brush.Reset(); target.Reset(); bitmap.Reset();
        valid = false;
        hr = wic->CreateBitmap(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, bitmap.GetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = factory->CreateWicBitmapRenderTarget(bitmap.Get(),
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                target.GetAddressOf());
        }
        if (SUCCEEDED(hr)) { hr = target->CreateSolidColorBrush(color(0), brush.GetAddressOf()); }
        if (SUCCEEDED(hr)) {
            width = w; height = h;
            pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            ++surface_allocations;
        }
        return hr;
    }
    ControllerViewRect pixels_rect(D2D1_RECT_F rect) const
    {
        return {left + rect.left * scale, top + rect.top * scale,
            left + rect.right * scale, top + rect.bottom * scale};
    }
    void ink(std::uint32_t value, float opacity = 1) { brush->SetColor(color(value, opacity)); }
    void box(D2D1_RECT_F bounds, float radius, bool pressed, const KeyboardPalette& p, bool contrast)
    {
        const auto shape = D2D1::RoundedRect(bounds, radius, radius);
        ink(pressed ? p.selected : p.surface); target->FillRoundedRectangle(shape, brush.Get());
        ink(pressed ? p.selected_ink : p.outline, contrast ? 1.0f : pressed ? .9f : .42f);
        target->DrawRoundedRectangle(shape, brush.Get(), pressed ? 1.25f : 1.0f);
    }
    void disc(float x, float y, float radius, bool pressed, const KeyboardPalette& p, bool contrast)
    {
        const auto ellipse = D2D1::Ellipse(point(x, y), radius, radius);
        ink(pressed ? p.selected : p.surface); target->FillEllipse(ellipse, brush.Get());
        ink(pressed ? p.selected_ink : p.outline, contrast ? 1.0f : pressed ? .9f : .48f);
        target->DrawEllipse(ellipse, brush.Get(), pressed ? 1.5f : 1.0f);
    }
    void control(ControllerViewControl id, D2D1_RECT_F bounds) { controls.push_back({id, pixels_rect(bounds)}); }
    void stick(unsigned index, ControllerViewControl id, float x, float y,
        std::int16_t raw_x, std::int16_t raw_y, bool pressed, const KeyboardPalette& p, bool contrast)
    {
        constexpr float radius = 23, travel = 6, cap = 13;
        const float cap_x = x + controller_view_axis(raw_x) * travel;
        const float cap_y = y - controller_view_axis(raw_y) * travel;
        control(id, {x - radius - 2, y - radius - 2, x + radius + 2, y + radius + 2});
        sticks[index] = {left + x * scale, top + y * scale,
            left + cap_x * scale, top + cap_y * scale, travel * scale, radius * scale, cap * scale};
        const auto well = D2D1::Ellipse(point(x, y), radius, radius);
        ink(p.surface); target->FillEllipse(well, brush.Get());
        ink(p.secondary, contrast ? 0.0f : .065f); target->FillEllipse(well, brush.Get());
        ink(p.outline, contrast ? 1.0f : .34f); target->DrawEllipse(well, brush.Get(), 1);
        disc(cap_x, cap_y, cap, pressed, p, contrast);
    }
    bool initialized = false, apartment_checked = false, uninitialize = false, valid = false;
    int width = 0, height = 0;
    float scale = 1, left = 0, top = 0;
    ComPtr<ID2D1Factory> factory;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1PathGeometry> body;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::vector<ControllerViewControlBounds> controls;
    std::vector<std::uint32_t> pixels;
    std::array<ControllerViewStick, 2> sticks{};
    ControllerViewRect body_bounds{};
    std::optional<RenderKey> key;
    std::uint64_t rasterizations = 0, surface_allocations = 0;
};

ControllerViewVisual::ControllerViewVisual() : resources_(std::make_unique<Resources>()) {}
ControllerViewVisual::~ControllerViewVisual() = default;

bool ControllerViewVisual::fail(std::wstring_view message, HRESULT result)
{
    resources_->valid = false;
    resources_->key.reset();
    error_ = std::wstring(message) + L" (HRESULT " + std::to_wstring(static_cast<long>(result)) + L").";
    return false;
}
ControllerViewVisualFrame ControllerViewVisual::frame() const
{
    const auto& r = *resources_;
    if (!r.valid) { return {}; }
    return {r.width, r.height, r.pixels, r.controls, r.sticks, r.body_bounds,
        r.rasterizations, r.surface_allocations};
}

bool ControllerViewVisual::render(int width, int height, const ControllerViewState& input, const KeyboardTheme& theme)
{
    if (width < controller_view_min_width || height < controller_view_min_height
        || width > controller_view_max_width || height > controller_view_max_height) {
        return fail(L"Controller view dimensions are outside the bounded surface", E_INVALIDARG);
    }
    auto state = input.signal == ControllerViewSignal::Ready
        ? input : controller_view_clear(input, input.signal);
    state.buttons &= standard_buttons;
    // Route and slot belong to the native accessible title, not the canvas.
    state.route = ControllerViewRoute::Observing;
    state.slot.reset();
    auto& r = *resources_;
    const RenderKey wanted{width, height, state, theme};
    if (r.valid && r.key == wanted) { return true; }
    r.valid = false; r.key.reset();
    HRESULT hr = r.surface(width, height);
    if (FAILED(hr)) { return fail(L"Controller view resources could not be created", hr); }
    const auto p = keyboard_palette(theme);
    const bool contrast = theme.high_contrast;
    r.scale = std::min(static_cast<float>(width) / controller_view_width, static_cast<float>(height) / controller_view_height);
    r.left = (static_cast<float>(width) - controller_view_width * r.scale) / 2;
    r.top = (static_cast<float>(height) - controller_view_height * r.scale) / 2;
    const auto transform = D2D1::Matrix3x2F::Scale(r.scale, r.scale)
        * D2D1::Matrix3x2F::Translation(r.left, r.top);
    D2D1_RECT_F body_bounds{};
    hr = r.body->GetBounds(&transform, &body_bounds);
    if (FAILED(hr)) { return fail(L"Controller view geometry could not be measured", hr); }
    r.body_bounds = {body_bounds.left, body_bounds.top, body_bounds.right, body_bounds.bottom};
    r.controls.clear();
    r.target->BeginDraw();
    r.target->SetTransform(D2D1::Matrix3x2F::Identity());
    r.target->Clear(color(0, 0));
    r.target->SetTransform(transform);
    const auto held = [&](std::uint16_t bit) { return (state.buttons & bit) != 0; };
    r.ink(p.surface, contrast ? 1.0f : 230.0f / 255.0f);
    r.target->FillGeometry(r.body.Get(), r.brush.Get());
    r.ink(p.outline, contrast ? 1.0f : .42f);
    r.target->DrawGeometry(r.body.Get(), r.brush.Get(), 1.2f);

    for (unsigned side = 0; side < 2; ++side) {
        const bool right = side != 0;
        const float x = right ? 206.0f : 68.0f;
        const auto value = right ? state.right_trigger : state.left_trigger;
        r.control(right ? ControllerViewControl::RightTrigger : ControllerViewControl::LeftTrigger,
            {x - 1, 29, x + 47, 37});
        r.box({x, 30, x + 46, 36}, 3, false, p, contrast);
        if (value) {
            const auto fill = D2D1::RoundedRect({x + 1, 31, x + 1 + 44 * (static_cast<float>(value) / 255), 35}, 1.5f, 1.5f);
            r.ink(p.selected); r.target->FillRoundedRectangle(fill, r.brush.Get());
            r.ink(p.selected_ink, contrast ? 1.0f : .8f); r.target->DrawRoundedRectangle(fill, r.brush.Get(), .65f);
        }
        r.control(right ? ControllerViewControl::RightShoulder : ControllerViewControl::LeftShoulder,
            {x - 1, 43, x + 47, 53});
        r.box({x, 44, x + 46, 52}, 4, held(right ? 0x0200 : 0x0100), p, contrast);
    }
    r.stick(0, ControllerViewControl::LeftStick, 88, 97, state.left_x, state.left_y, held(0x0040), p, contrast);
    r.stick(1, ControllerViewControl::RightStick, 193, 145, state.right_x, state.right_y, held(0x0080), p, contrast);
    struct Face { float x, y; std::uint16_t bit; ControllerViewControl id; };
    constexpr std::array<Face, 4> faces{{
        {231, 116, 0x1000, ControllerViewControl::A}, {249, 98, 0x2000, ControllerViewControl::B},
        {213, 98, 0x4000, ControllerViewControl::X}, {231, 80, 0x8000, ControllerViewControl::Y}
    }};
    for (const auto& face : faces) {
        r.control(face.id, {face.x - 10, face.y - 10, face.x + 10, face.y + 10});
        r.disc(face.x, face.y, 8.5f, held(face.bit), p, contrast);
    }
    struct Direction { D2D1_RECT_F rect; std::uint16_t bit; ControllerViewControl id; };
    constexpr std::array<Direction, 4> directions{{
        {{118, 127, 130, 138}, 0x0001, ControllerViewControl::Up},
        {{118, 152, 130, 163}, 0x0002, ControllerViewControl::Down},
        {{106, 139, 117, 151}, 0x0004, ControllerViewControl::Left},
        {{131, 139, 142, 151}, 0x0008, ControllerViewControl::Right}
    }};
    r.box({118, 139, 130, 151}, 1, false, p, contrast);
    for (const auto& direction : directions) {
        const auto rect = direction.rect;
        r.control(direction.id, {rect.left - 1, rect.top - 1, rect.right + 1, rect.bottom + 1});
        r.box(rect, 1.5f, held(direction.bit), p, contrast);
    }
    for (unsigned index = 0; index < 2; ++index) {
        const bool menu = index != 0;
        const float x = menu ? 179.0f : 146.0f;
        r.control(menu ? ControllerViewControl::Menu : ControllerViewControl::View, {x - 6, 102, x + 6, 110});
        r.box({x - 5, 103, x + 5, 109}, 3, held(menu ? 0x0010 : 0x0020), p, contrast);
    }
    if (state.signal != ControllerViewSignal::Ready && state.signal != ControllerViewSignal::Waiting) {
        const bool fault = state.signal == ControllerViewSignal::ReadError || state.signal == ControllerViewSignal::Stale;
        r.ink(fault ? p.error : p.secondary);
        r.target->FillEllipse(D2D1::Ellipse(point(160, 225), 2.5f, 2.5f), r.brush.Get());
    }
    const HRESULT draw = r.target->EndDraw();
    if (FAILED(draw)) { return fail(L"Controller view drawing failed", draw); }
    const auto stride = static_cast<UINT>(width) * 4;
    hr = r.bitmap->CopyPixels(nullptr, stride, stride * static_cast<UINT>(height),
        reinterpret_cast<BYTE*>(r.pixels.data()));
    if (FAILED(hr)) { return fail(L"Controller view pixels could not be read", hr); }
    ++r.rasterizations; r.key = wanted; r.valid = true; error_.clear();
    return true;
}

bool ControllerViewVisual::save_png(const wchar_t* path)
{
    auto& r = *resources_;
    if (!path || !*path || !r.valid) { return fail(L"Controller view has no valid PNG destination or frame", E_INVALIDARG); }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> output;
    ComPtr<IPropertyBag2> properties;
    ComPtr<IWICFormatConverter> straight;
    HRESULT hr = r.wic->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr)) { hr = stream->InitializeFromFilename(path, GENERIC_WRITE); }
    if (SUCCEEDED(hr)) { hr = r.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache); }
    if (SUCCEEDED(hr)) { hr = encoder->CreateNewFrame(output.GetAddressOf(), properties.GetAddressOf()); }
    if (SUCCEEDED(hr)) { hr = output->Initialize(properties.Get()); }
    if (SUCCEEDED(hr)) { hr = output->SetSize(static_cast<UINT>(r.width), static_cast<UINT>(r.height)); }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) { hr = output->SetPixelFormat(&format); }
    if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) { hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; }
    if (SUCCEEDED(hr)) { hr = r.wic->CreateFormatConverter(straight.GetAddressOf()); }
    if (SUCCEEDED(hr)) {
        hr = straight->Initialize(r.bitmap.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0, WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) { hr = output->WriteSource(straight.Get(), nullptr); }
    if (SUCCEEDED(hr)) { hr = output->Commit(); }
    if (SUCCEEDED(hr)) { hr = encoder->Commit(); }
    if (FAILED(hr)) { return fail(L"Controller view PNG export failed", hr); }
    return true;
}
} // namespace overworld
