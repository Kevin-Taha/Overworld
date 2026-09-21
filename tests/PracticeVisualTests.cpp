#include "PracticeWindow.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace overworld;
using namespace overworld::practice;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
void inspect(const Frame& frame)
{
    check(!frame.pixels.empty() && frame.text.size() >= 12, "frame must have actual pixels and measured text");
    for (const auto pixel : frame.pixels) { check((pixel >> 24) == 255, "owned scene is fully opaque premultiplied BGRA"); }
    for (std::size_t i = 0; i < frame.text.size(); ++i) {
        const auto& a = frame.text[i];
        if (!(a.ink_left >= a.left - .1f && a.ink_top >= a.top - .1f
            && a.ink_right <= a.right + .1f && a.ink_bottom <= a.bottom + .1f)) {
            std::wcerr << L"Clipped text: " << a.text << L'\n';
            throw std::runtime_error("actual DirectWrite ink must fit its reserved box");
        }
        check(a.ink_left >= 0 && a.ink_top >= 0 && a.ink_right <= canvas_width
            && a.ink_bottom <= canvas_height, "ink must remain in canvas");
        check(a.ink_right > a.ink_left && a.ink_bottom > a.ink_top, "text has nonempty actual glyph ink");
        for (std::size_t j = i + 1; j < frame.text.size(); ++j) {
            const auto& b = frame.text[j];
            if (a.ink_left < b.ink_right && b.ink_left < a.ink_right && a.ink_top < b.ink_bottom && b.ink_top < a.ink_bottom) {
                std::wcerr << L"Overlap: " << a.text << L" / " << b.text << L'\n';
                throw std::runtime_error("separate labels cannot overlap");
            }
        }
        const auto view = layout(frame.width, frame.height);
        const auto x = static_cast<int>(view.left + a.ink_left * view.scale);
        const auto y = static_cast<int>(view.top + a.ink_top * view.scale);
        const auto right = static_cast<int>(std::ceil(view.left + a.ink_right * view.scale));
        const auto bottom = static_cast<int>(std::ceil(view.top + a.ink_bottom * view.scale));
        bool varied = false;
        const auto first = frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
        for (int row = y; row < bottom && !varied; ++row) {
            for (int col = x; col < right; ++col) {
                if (frame.pixels[static_cast<std::size_t>(row) * frame.width + col] != first) { varied = true; break; }
            }
        }
        check(varied, "glyph rectangle must contain visible pixels, not just successful paint calls");
    }
}
class SharedTheme final : public KeyboardThemeSource {
public:
    KeyboardTheme theme;
    HRESULT read(KeyboardTheme& result) override { result = theme; return S_OK; }
};
void shared_pixels(Visual& visual, int width, int height, const Model& model, const AppPalette& palette, bool help)
{
    const float scale = layout(width, height).scale;
    if (model.keyboard_open() && !help) {
        const auto rgb = [](COLORREF value) {
            return (static_cast<std::uint32_t>(GetRValue(value)) << 16) | (GetGValue(value) << 8) | GetBValue(value);
        };
        SharedTheme theme;
        theme.theme = {palette.dark, palette.high_contrast, rgb(palette.surface), rgb(palette.text),
            rgb(palette.accent), rgb(palette.accent_text)};
        KeyboardVisual production(&theme);
        check(production.render(static_cast<int>(std::lround(keyboard_visual_width * scale)),
            static_cast<int>(std::lround(keyboard_visual_height * scale)), model.keyboard(),
            {model.input() == InputState::Ready, true, {}, {}}), "production keyboard reference renders");
        const auto actual = visual.keyboard_frame(), expected = production.frame();
        check(actual.width == expected.width && actual.height == expected.height
            && std::equal(actual.pixels.begin(), actual.pixels.end(), expected.pixels.begin(), expected.pixels.end()),
            "harness keyboard backing pixels must exactly match the shared production renderer");
        check(actual.glyphs.size() == expected.glyphs.size(), "harness uses the production glyph layout");
        for (std::size_t i = 0; i < actual.glyphs.size(); ++i) {
            const auto& a = actual.glyphs[i]; const auto& b = expected.glyphs[i];
            check(a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom
                && a.character == b.character && a.face == b.face && a.petal == b.petal,
                "harness must preserve production character identity and exact ink geometry");
        }
    } else {
        const auto dpi = static_cast<unsigned>(std::max(96l, std::lround(96 * scale)));
        const auto expected = cursor_pixels(dpi, default_cursor_size, default_cursor_style);
        const auto actual = visual.cursor_frame();
        check(!actual.empty() && std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()),
            "harness cursor uses actual production cursor pixels/defaults, not independent artwork");
    }
}
void render(Visual& visual, int width, int height, const Model& model, const AppPalette& palette, bool help = false)
{
    if (!visual.render(width, height, model, palette, help, 0)) {
        std::wcerr << visual.error() << L'\n'; throw std::runtime_error("render failed");
    }
    inspect(visual.frame());
    shared_pixels(visual, width, height, model, palette, help);
}
void progress(Model& model, Lesson lesson)
{
    switch (lesson) {
    case Lesson::Aim: for (unsigned i = 0; i < 3; ++i) { model.move_local(aim_target(i)); } break;
    case Lesson::Click:
        model.move_local({300, 175}); model.primary_local(true); model.primary_local(false); model.secondary_local(); break;
    case Lesson::Drag:
        model.move_local({140, 190}); model.primary_local(true); model.move_local({480, 210}); model.primary_local(false); break;
    case Lesson::Scroll:
        model.move_local({300, 300}); model.scroll_local(-480); model.primary_local(true); model.primary_local(false); break;
    case Lesson::Type:
        model.open_keyboard(); model.keyboard_local(0, 0, 1000); break;
    }
}
void owned_window(const std::filesystem::path& capture = {})
{
    Window window;
    if (!window.create(GetModuleHandleW(nullptr))) {
        std::wcerr << window.error() << L'\n';
        throw std::runtime_error("hidden source-free practice window creates");
    }
    check(!IsWindowVisible(window.handle()), "render window never becomes visible or activates");
    check(window.update(app_palette(true)), "native control frame update");
    RECT client{};
    GetClientRect(window.handle(), &client);
    const auto view = layout(client.right, client.bottom);
    const auto entries = controls(window.model(), false, 0);
    HWND tab = nullptr;
    for (const auto& entry : entries) {
        const auto child = GetDlgItem(window.handle(), entry.id);
        check(child != nullptr, "every visual action/status has a native accessibility control");
        const auto expected = view.rect(entry.bounds);
        RECT actual{};
        GetWindowRect(child, &actual);
        MapWindowPoints(nullptr, window.handle(), reinterpret_cast<POINT*>(&actual), 2);
        check(EqualRect(&expected, &actual), "native control geometry matches renderer exactly");
        if (entry.button) {
            check((GetWindowLongPtrW(child, GWL_STYLE) & WS_TABSTOP) != 0, "native actions are keyboard reachable");
            tab = GetNextDlgTabItem(window.handle(), tab, FALSE);
            check(tab == child, "tab order follows the visible rows");
            if (entry.id != Scene) {
                const live::Point point{entry.bounds.left + 20 - scene_left, entry.bounds.top + 20 - scene_top};
                check(clicked_control(window.model(), {point, point}, 0) == entry.id,
                    "controller A can reach the same native practice action");
                check(!clicked_control(window.model(), {point, {300, 180}}, 0),
                    "release on another target cannot activate a local command");
            }
        }
        check(GetWindowTextLengthW(child) > 0, "native controls expose accessible names");
    }
    window.model().select(Lesson::Type); window.model().open_keyboard();
    if (!window.update()) {
        std::wcerr << window.error() << L'\n';
        throw std::runtime_error("native keyboard fallback controls update");
    }
    for (int id = Group; id <= FaceY; ++id) {
        check(GetDlgItem(window.handle(), id) != nullptr, "group and face selectors exist without external fields");
    }
    const auto before = window.visual().frame().rasterizations;
    for (unsigned i = 0; i < 50; ++i) { check(window.update(), "idle frame updates"); }
    check(window.visual().frame().rasterizations == before, "idle window never rerasterizes unchanged state");
    check(window.model().input() == InputState::Background, "render-only mode cannot poll a controller");

    const auto frame = window.visual().frame();
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = frame.width;
    info.bmiHeader.biHeight = -frame.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    check(dc && bitmap && pixels, "owned capture surface allocated");
    const auto old = SelectObject(dc, bitmap);
    // PrintWindow may return a blank success for a hidden HWND. Exercise the actual
    // owned paint handlers against a bounded DC instead, including child offsets.
    SendMessageW(window.handle(), WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    for (const auto& entry : controls(window.model(), false, 0)) {
        const auto bounds = view.rect(entry.bounds);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
        SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
        DRAWITEMSTRUCT item{};
        item.CtlType = entry.button ? ODT_BUTTON : ODT_STATIC;
        item.CtlID = entry.id;
        item.itemAction = ODA_DRAWENTIRE;
        item.hwndItem = GetDlgItem(window.handle(), entry.id);
        item.hDC = dc;
        item.rcItem = {0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
        SendMessageW(window.handle(), WM_DRAWITEM, entry.id, reinterpret_cast<LPARAM>(&item));
        RestoreDC(dc, saved);
    }
    std::size_t matches = 0;
    const auto* actual = static_cast<const std::uint32_t*>(pixels);
    for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
        if ((actual[i] & 0xFFFFFF) == (frame.pixels[i] & 0xFFFFFF)) { ++matches; }
    }
    if (!capture.empty()) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + static_cast<DWORD>(frame.pixels.size() * 4);
        std::ofstream output(capture, std::ios::binary);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        output.write(reinterpret_cast<const char*>(pixels), static_cast<std::streamsize>(frame.pixels.size() * 4));
        check(output.good(), "owned HWND capture saved");
    }
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
    if (matches != frame.pixels.size()) {
        std::cerr << "Owned paint RGB match=" << matches << '/' << frame.pixels.size() << '\n';
    }
    check(matches == frame.pixels.size(),
        "actual owned HWND/child painting must exactly reproduce renderer RGB pixels, not only return success");
}
void owned_hold_messages()
{
    Window window;
    check(window.create(GetModuleHandleW(nullptr)), "owned hold-message window creates");
    window.model().focus(true);
    window.model().select(Lesson::Drag);
    window.model().move_local({140, 190});
    check(window.update(), "drag frame ready");
    const auto scene = GetDlgItem(window.handle(), Scene);
    const auto next = GetDlgItem(window.handle(), Next);
    MSG message{};
    message.hwnd = scene; message.message = WM_KEYDOWN; message.wParam = VK_SPACE;
    check(window.pretranslate(message) && window.model().local_hold() && window.model().dragging(), "Space begins scene drag");
    message.wParam = VK_TAB;
    window.pretranslate(message);
    SendMessageW(scene, WM_KILLFOCUS, reinterpret_cast<WPARAM>(next), 0);
    check(!window.model().local_hold() && !window.model().dragging(), "child focus loss cancels owned drag without a drop");
    message.hwnd = next; message.message = WM_KEYUP;
    window.pretranslate(message);
    message.wParam = VK_SPACE;
    check(window.pretranslate(message) && !window.model().local_hold(), "matching Space release consumed before non-scene guard");
    window.model().retry(); window.model().move_local({140, 190});
    message.hwnd = scene; message.message = WM_KEYDOWN; message.wParam = VK_SPACE;
    window.pretranslate(message);
    message.wParam = VK_F1; window.pretranslate(message);
    check(window.model().help() && !window.model().local_hold(), "help while Space held cancels gesture");
    message.message = WM_KEYUP; window.pretranslate(message);
    message.wParam = VK_SPACE;
    check(window.pretranslate(message) && !window.model().local_hold(), "matching release survives help-mode guard");
}
void frames(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path);
    const AppPalette high_black{RGB(0,0,0), RGB(0,0,0), RGB(255,255,255), RGB(255,255,255),
        RGB(255,255,255), RGB(255,255,0), RGB(0,0,0), false, true};
    const AppPalette high_white{RGB(255,255,255), RGB(255,255,255), RGB(0,0,0), RGB(0,0,0),
        RGB(0,0,0), RGB(0,0,0), RGB(255,255,255), false, true};
    const std::array<AppPalette, 4> palettes{app_palette(false), app_palette(true), high_black, high_white};
    const std::array<std::wstring_view, 4> names{L"light", L"dark", L"contrast-black", L"contrast-white"};
    unsigned count = 0;
    for (unsigned theme = 0; theme < palettes.size(); ++theme) {
        for (const int dpi : {96, 144, 192}) {
            Visual visual;
            const int width = canvas_width * dpi / 96, height = canvas_height * dpi / 96;
            Model model;
            for (unsigned index = 0; index < lesson_count; ++index) {
                const auto lesson = static_cast<Lesson>(index);
                model.select(lesson);
                render(visual, width, height, model, palettes[theme]);
                progress(model, lesson);
                render(visual, width, height, model, palettes[theme]);
                const auto stem = std::wstring(names[theme]) + L"-" + std::to_wstring(dpi) + L"-skill-" + std::to_wstring(index + 1);
                check(visual.save_png((path / (stem + L".png")).c_str()), "PNG export succeeds");
                ++count;
            }
            model.one_handed(true);
            model.keyboard_local(0, 0, 2000);
            render(visual, width, height, model, palettes[theme]);
            check(visual.save_png((path / (std::wstring(names[theme]) + L"-" + std::to_wstring(dpi) + L"-one-handed.png")).c_str()),
                "one-handed PNG");
            render(visual, width, height, model, palettes[theme], true);
            check(visual.save_png((path / (std::wstring(names[theme]) + L"-" + std::to_wstring(dpi) + L"-help.png")).c_str()), "help PNG");
            const auto frame = visual.frame();
            for (unsigned i = 0; i < 40; ++i) {
                check(visual.render(width, height, model, palettes[theme], true, 0), "idle render");
            }
            check(visual.frame().allocations == frame.allocations && visual.frame().rasterizations == frame.rasterizations,
                "unchanged state retains surface and complete raster");
        }
    }
    Model narrow;
    Visual small;
    render(small, 800, 592, narrow, palettes[1]);
    check(small.save_png((path / L"compact-800x592.png").c_str()), "compact handheld frame");
    render(small, 1200, 740, narrow, palettes[0]);
    check(small.save_png((path / L"wide-letterbox.png").c_str()), "wide letterboxed frame");
    std::cout << count << " skill PNGs + 24 keyboard/help PNGs + 2 compact/wide PNGs; actual ink, alpha, geometry, idle reuse passed.\n";
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 3 || std::wstring_view(argv[1]) != L"--render-only") {
        std::cerr << "Explicit invocation required: overworld_practice_visual_tests --render-only <owned-output-directory>\n";
        return 2;
    }
    std::jthread watchdog([](std::stop_token stop) {
        for (int i = 0; i < 1200 && !stop.stop_requested(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        if (!stop.stop_requested()) { TerminateProcess(GetCurrentProcess(), 3); }
    });
    try {
        std::filesystem::create_directories(argv[2]);
        owned_hold_messages();
        owned_window(std::filesystem::path(argv[2]) / L"owned-window.bmp");
        const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        frames(argv[2]);
        for (int i = 0; i < 3; ++i) { owned_window(); }
        const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        check(after <= before + 4, "repeated native-window lifetimes must not leak GDI resources");
        std::cout << "Hidden owned-window/native accessibility geometry and bounded lifetimes passed. No controller source linked.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
