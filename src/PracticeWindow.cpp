#include "PracticeWindow.h"

#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <new>

namespace gamepad::practice {
namespace {
constexpr wchar_t window_class[] = L"GamepadDesktop.Practice";
constexpr wchar_t title[] = L"Practice - no desktop input";
constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
Tick clock_now()
{
    return static_cast<Tick>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool blit(HDC dc, const Frame& frame, int x, int y)
{
    if (frame.pixels.empty()) { return false; }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = frame.width;
    info.bmiHeader.biHeight = -frame.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto result = SetDIBitsToDevice(dc, x, y, frame.width, frame.height,
        0, 0, 0, frame.height, frame.pixels.data(), &info, DIB_RGB_COLORS);
    return result != 0;
}
}
class Window::Impl {
public:
    ~Impl()
    {
        if (window) { DestroyWindow(window); }
        if (registered) { UnregisterClassW(window_class, instance); }
    }
    bool create(HINSTANCE value, ControllerSource* input)
    {
        instance = value; source = input;
        WNDCLASSEXW type{sizeof(type)};
        type.lpfnWndProc = proc; type.hInstance = value;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.lpszClassName = window_class;
        if (!RegisterClassExW(&type)) { return fail(L"Could not register the practice window."); }
        registered = true;
        RECT bounds{0, 0, canvas_width, canvas_height};
        if (!AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_APPWINDOW, 96)) {
            return fail(L"Could not size the practice window.");
        }
        if (!CreateWindowExW(WS_EX_APPWINDOW, window_class, title, style,
            -12000, -12000, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, value, this)) { return fail(L"Could not create the practice window."); }
        if (!appearance.refresh(window)) { return fail(appearance.error()); }
        if (!update()) { return false; }
        if (source && !SetTimer(window, 1, 16, nullptr)) { return fail(L"Could not start the practice refresh timer."); }
        return true;
    }
    bool focused() const
    {
        return source && window && GetForegroundWindow() == window && GetActiveWindow() == window
            && !IsIconic(window);
    }
    bool fail(std::wstring_view message)
    {
        if (error.empty()) { error = message; }
        if (window) { KillTimer(window, 1); PostMessageW(window, WM_CLOSE, 0, 0); }
        return false;
    }
    bool update(std::optional<AppPalette> palette = {})
    {
        if (!window || !error.empty()) { return false; }
        if (palette && !appearance.refresh(window, palette)) { return fail(appearance.error()); }
        RECT client{};
        if (!GetClientRect(window, &client)) { return fail(L"Could not read practice window dimensions."); }
        if (client.right < 400 || client.bottom < 296 || IsIconic(window)) { return true; }
        const auto entries = controls(model, model.help(), group);
        const auto view = layout(client.right, client.bottom);
        if (!model.display_scale(view.scale)) { return fail(L"Practice scale is outside its supported range."); }
        const auto before = visual.frame().rasterizations;
        if (!visual.render(client.right, client.bottom, model, appearance.colors(), model.help(), group)) { return fail(visual.error()); }
        for (HWND child = GetWindow(window, GW_CHILD); child;) {
            const HWND next = GetWindow(child, GW_HWNDNEXT);
            const auto id = GetDlgCtrlID(child);
            if (std::none_of(entries.begin(), entries.end(), [id](const Control& control) { return id == control.id; })) {
                DestroyWindow(child);
            }
            child = next;
        }
        HWND previous_child = HWND_TOP;
        for (const auto& entry : entries) {
            auto child = GetDlgItem(window, entry.id);
            if (!child) {
                child = CreateWindowExW(0, entry.button ? L"BUTTON" : L"STATIC", entry.text.c_str(),
                    WS_CHILD | WS_VISIBLE | (entry.button ? WS_TABSTOP | BS_OWNERDRAW : SS_OWNERDRAW),
                    0, 0, 1, 1, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(entry.id)), instance, nullptr);
                if (!child) { return fail(L"Could not create an accessible practice control."); }
                if (entry.id == Scene && !SetWindowSubclass(child, scene_proc, 1, reinterpret_cast<DWORD_PTR>(this))) {
                    return fail(L"Could not attach the local scene controls.");
                }
            }
            const auto bounds = view.rect(entry.bounds);
            RECT current{};
            GetWindowRect(child, &current);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&current), 2);
            const auto preceding = GetWindow(child, GW_HWNDPREV);
            const bool order_changed = previous_child == HWND_TOP ? preceding != nullptr : preceding != previous_child;
            if ((!EqualRect(&current, &bounds) || order_changed) && !SetWindowPos(child, previous_child, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE)) {
                return fail(L"Could not lay out the practice controls.");
            }
            previous_child = child;
            const int length = GetWindowTextLengthW(child);
            std::wstring previous(static_cast<std::size_t>(length) + 1, L'\0');
            GetWindowTextW(child, previous.data(), length + 1);
            previous.resize(length);
            if (previous != entry.text && !SetWindowTextW(child, entry.text.c_str())) {
                return fail(L"Could not update the accessible practice status.");
            }
        }
        if (before != visual.frame().rasterizations) {
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        return true;
    }
    void command(int id)
    {
        if (id >= StepFirst && id < StepFirst + static_cast<int>(lesson_count)) {
            model.select(static_cast<Lesson>(id - StepFirst));
        } else {
            switch (id) {
            case Retry: model.retry(); break;
            case Next: model.next(); break;
            case Help: model.show_help(!model.help()); break;
            case Reset: model.reset(); break;
            case Exit: DestroyWindow(window); return;
            case OpenKeyboard:
                if (model.keyboard_open()) { model.close_keyboard(); } else { model.open_keyboard(); }
                break;
            case OneHanded: model.one_handed(model.keyboard().input_mode() != KeyboardInputMode::OneHanded); break;
            case Group: group = (group + 1) % 8; break;
            case FaceA: case FaceB: case FaceX: case FaceY:
                model.keyboard_local(group, static_cast<unsigned>(id - FaceA), clock_now()); break;
            default: break;
            }
        }
        model.focus(focused());
        update();
    }
    void physical()
    {
        if (source) { source->authorize(nullptr); }
        if (model.input() != InputState::Physical) { model.physical_takeover(); }
    }
    void local_command()
    {
        if (const auto click = model.take_click()) {
            if (const auto action = clicked_control(model, *click, group)) { command(*action); }
        }
    }
    bool key(UINT message, WPARAM value, LPARAM data, HWND target)
    {
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (!keys.observe(model, static_cast<unsigned>(value), down)) { return true; }
        if (source) { source->authorize(nullptr); }
        if (down && (data & (LPARAM{1} << 30)) == 0) {
            if (value == VK_F1) { command(Help); return true; }
            if (value == VK_ESCAPE) {
                if (model.help()) { command(Help); }
                else if (model.keyboard_open()) { model.close_keyboard(); update(); }
                else if (model.context_open()) { model.dismiss_options(); update(); }
                else { command(Exit); }
                return true;
            }
        }
        if (GetDlgCtrlID(target) != Scene || model.help()) { return false; }
        if (model.keyboard_open()) {
            if (down && value >= '1' && value <= '4' && (data & (LPARAM{1} << 30)) == 0) {
                model.keyboard_local(group, static_cast<unsigned>(value - '1'), clock_now());
            }
        } else {
            auto point = model.pointer();
            if (down) {
                switch (value) {
                case VK_LEFT: point.x -= 12; break;
                case VK_RIGHT: point.x += 12; break;
                case VK_UP: point.y -= 12; break;
                case VK_DOWN: point.y += 12; break;
                case VK_RETURN: model.primary_local(true); model.primary_local(false); break;
                case VK_PRIOR: model.scroll_local(120); break;
                case VK_NEXT: model.scroll_local(-120); break;
                case 'X': model.secondary_local(); break;
                case 'Y': model.open_keyboard(); break;
                default: break;
                }
            }
            if (value == VK_SPACE) { model.primary_local(down); }
            model.move_local(point);
        }
        local_command();
        update();
        return value == VK_LEFT || value == VK_RIGHT || value == VK_UP || value == VK_DOWN
            || value == VK_SPACE || value == VK_RETURN || value == VK_PRIOR || value == VK_NEXT
            || value == 'X' || value == 'Y' || (value >= '1' && value <= '4');
    }
    static LRESULT CALLBACK scene_proc(HWND child, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data)
    {
        auto& self = *reinterpret_cast<Impl*>(data);
        try { return scene_dispatch(child, message, wparam, lparam, self); }
        catch (const std::bad_alloc&) { self.fail(L"Practice could not update its local scene."); return 0; }
    }
    static LRESULT scene_dispatch(HWND child, UINT message, WPARAM wparam, LPARAM lparam, Impl& self)
    {
        if (message == WM_NCDESTROY) { RemoveWindowSubclass(child, scene_proc, 1); }
        if (!self.source || self.model.help() || !self.focused()) { return DefSubclassProc(child, message, wparam, lparam); }
        switch (message) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
            self.physical();
            self.mouse_held = (wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) != 0;
            RECT client{};
            GetClientRect(self.window, &client);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            MapWindowPoints(child, self.window, &point, 1);
            if (!self.model.keyboard_open()) {
                self.model.move_local(layout(client.right, client.bottom).scene_point(point.x, point.y));
                if (message == WM_LBUTTONDOWN) { self.model.primary_local(true); }
                if (message == WM_LBUTTONUP) { self.model.primary_local(false); }
                if (message == WM_RBUTTONDOWN) { self.model.secondary_local(); }
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, child, 0};
            if (!TrackMouseEvent(&track)) { self.fail(L"Could not track departure from the local scene."); }
            self.local_command();
            self.update();
            return 0;
        }
        case WM_MOUSEWHEEL:
            self.physical(); self.model.scroll_local(GET_WHEEL_DELTA_WPARAM(wparam)); self.update(); return 0;
        case WM_MOUSELEAVE:
            self.mouse_held = false; self.model.cancel_local(); self.update(); return 0;
        default: break;
        }
        return DefSubclassProc(child, message, wparam, lparam);
    }
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) { return DefWindowProcW(window, message, wparam, lparam); }
        try { return self->dispatch(message, wparam, lparam); }
        catch (const std::bad_alloc&) { self->fail(L"Practice ran out of drawing or scene memory."); return 0; }
    }
    LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message) {
        case WM_TIMER:
            if (source && error.empty()) {
                model.focus(focused());
                const bool allowed = focused() && !tracking && !mouse_held && !keys.held() && GetCapture() == nullptr;
                source->authorize(allowed ? window : nullptr);
                if (allowed) {
                    const auto sample = source->read();
                    if (focused()) { model.step(sample, source->now()); }
                    else { model.focus(false); }
                    local_command();
                    if (!window) { return 0; }
                }
                if (model.exit_requested()) { DestroyWindow(window); return 0; }
                update();
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                if (source) { source->authorize(nullptr); }
                keys.reset(); mouse_held = false; model.focus(false);
            }
            return 0;
        case WM_ENTERSIZEMOVE: case WM_ENTERMENULOOP:
            if (source) { source->authorize(nullptr); }
            tracking = true; model.cancel_local(); return 0;
        case WM_EXITSIZEMOVE: case WM_EXITMENULOOP:
            tracking = false; model.cancel_local(); return 0;
        case WM_COMMAND: if (HIWORD(wparam) == BN_CLICKED) { command(LOWORD(wparam)); } return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PRINTCLIENT:
            if (!blit(reinterpret_cast<HDC>(wparam), visual.frame(), 0, 0)) { fail(L"Could not export the owned practice surface."); }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            const bool painted = blit(dc, visual.frame(), 0, 0);
            EndPaint(window, &paint);
            if (!painted && !visual.frame().pixels.empty()) { fail(L"Could not present the practice frame."); }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            RECT rect{};
            GetWindowRect(item.hwndItem, &rect);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
            if (!blit(item.hDC, visual.frame(), -rect.left, -rect.top)) { fail(L"Could not present a practice control."); }
            if (item.itemState & (ODS_FOCUS | ODS_SELECTED)) {
                RECT focus = item.rcItem; InflateRect(&focus, -3, -3); DrawFocusRect(item.hDC, &focus);
            }
            return TRUE;
        }
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) { model.focus(false); }
            if (!visual.frame().pixels.empty()) { update(); }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            RECT size{0, 0, 800, 592};
            AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_APPWINDOW, GetDpiForWindow(window));
            limits->ptMinTrackSize = {size.right - size.left, size.bottom - size.top};
            RECT maximum{0, 0, 4000, 2960};
            if (!AdjustWindowRectExForDpi(&maximum, style, FALSE, WS_EX_APPWINDOW, GetDpiForWindow(window))) {
                fail(L"Could not calculate the maximum practice size."); return 0;
            }
            limits->ptMaxTrackSize = {maximum.right - maximum.left, maximum.bottom - maximum.top};
            limits->ptMaxSize.x = std::min(limits->ptMaxSize.x, limits->ptMaxTrackSize.x);
            limits->ptMaxSize.y = std::min(limits->ptMaxSize.y, limits->ptMaxTrackSize.y);
            return 0;
        }
        case WM_DPICHANGED: {
            model.cancel_local();
            const auto& rect = *reinterpret_cast<const RECT*>(lparam);
            if (!SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE)) { fail(L"Could not apply practice DPI layout."); }
            return 0;
        }
        case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:
            if (!appearance.refresh(window)) { fail(appearance.error()); } else { update(); }
            return 0;
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY:
            if (source) { source->authorize(nullptr); }
            KillTimer(window, 1);
            if (running) { PostQuitMessage(error.empty() ? 0 : 1); }
            return 0;
        case WM_NCDESTROY:
        {
            const auto old = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            window = nullptr;
            return DefWindowProcW(old, message, wparam, lparam);
        }
        default: break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    int run(int)
    {
        if (!source) { fail(L"A render-only practice window cannot start an interactive session."); return 1; }
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &info)) {
            fail(L"Could not position practice in the monitor work area."); return 1;
        }
        const UINT dpi = GetDpiForWindow(window);
        const int width = std::min<int>(MulDiv(canvas_width, static_cast<int>(dpi), 96), info.rcWork.right - info.rcWork.left - 48);
        const int height = std::min<int>(MulDiv(canvas_height + 40, static_cast<int>(dpi), 96), info.rcWork.bottom - info.rcWork.top - 48);
        if (!SetWindowPos(window, nullptr, info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2,
            info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE)) { fail(L"Could not position the practice window."); return 1; }
        ShowWindow(window, SW_SHOWNOACTIVATE);
        running = true;
        MSG message{};
        int status = 0;
        while ((status = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
            const bool owned = message.hwnd == window || IsChild(window, message.hwnd);
            if (owned && (message.message == WM_KEYDOWN || message.message == WM_KEYUP
                || message.message == WM_SYSKEYDOWN || message.message == WM_SYSKEYUP)) {
                if (key(message.message, message.wParam, message.lParam, message.hwnd)) { continue; }
            } else if (owned && message.message >= WM_MOUSEFIRST && message.message <= WM_MOUSELAST) {
                physical();
                mouse_held = (GET_KEYSTATE_WPARAM(message.wParam)
                    & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) != 0;
            }
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (status < 0) { fail(L"The practice message loop failed."); }
        return error.empty() ? static_cast<int>(message.wParam) : 1;
    }
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    ControllerSource* source = nullptr;
    bool registered = false, mouse_held = false, tracking = false, running = false;
    unsigned group = 0;
    LocalKeys keys;
    Model model;
    Visual visual;
    AppUi appearance;
    std::wstring error;
};
Window::Window() : impl_(std::make_unique<Impl>()) {}
Window::~Window() = default;
bool Window::create(HINSTANCE instance, ControllerSource* source) { return impl_->create(instance, source); }
int Window::run(int show) { return impl_->run(show); }
bool Window::update(std::optional<AppPalette> palette) { return impl_->update(palette); }
HWND Window::handle() const { return impl_->window; }
Model& Window::model() { return impl_->model; }
Visual& Window::visual() { return impl_->visual; }
const std::wstring& Window::error() const { return impl_->error; }
int run(HINSTANCE instance, int show, ControllerSource& source)
{
    try {
        Window window;
        const int result = window.create(instance, &source) ? window.run(show) : 1;
        if (!window.error().empty()) { MessageBoxW(nullptr, window.error().c_str(), title, MB_OK | MB_ICONERROR); }
        return result;
    } catch (const std::bad_alloc&) {
        MessageBoxW(nullptr, L"Could not allocate the practice window.", title, MB_OK | MB_ICONERROR);
        return 1;
    }
}
} // namespace gamepad::practice
