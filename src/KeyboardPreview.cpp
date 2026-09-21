#include "KeyboardPreview.h"

#include "Keyboard.h"
#include "KeyboardVisual.h"
#include "XInputReader.h"

#include <algorithm>
#include <new>
#include <string_view>
#include <system_error>

namespace overworld {
namespace {
constexpr wchar_t window_class[] = L"Overworld.KeyboardPreview";
constexpr wchar_t window_title[] = L"Keyboard preview - Overworld";
constexpr UINT_PTR refresh_timer = 1;
constexpr DWORD window_style = WS_POPUP;
constexpr DWORD window_ex_style = WS_EX_APPWINDOW | WS_EX_LAYERED;
constexpr int minimum_width = keyboard_visual_width;
constexpr int minimum_height = keyboard_visual_height;

bool work_area(HMONITOR monitor, RECT& result) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    result = info.rcWork;
    return true;
}

RECT fit_to_work_area(RECT bounds, const RECT& work) {
    const LONG width = std::min(bounds.right - bounds.left, work.right - work.left);
    const LONG height = std::min(bounds.bottom - bounds.top, work.bottom - work.top);
    bounds.left = std::clamp(bounds.left, work.left, work.right - width);
    bounds.top = std::clamp(bounds.top, work.top, work.bottom - height);
    bounds.right = bounds.left + width;
    bounds.bottom = bounds.top + height;
    return bounds;
}

bool window_bounds(UINT dpi, int logical_width, int logical_height, RECT& bounds) {
    bounds = {0, 0, MulDiv(logical_width, static_cast<int>(dpi), 96),
        MulDiv(logical_height, static_cast<int>(dpi), 96)};
    return AdjustWindowRectExForDpi(&bounds, window_style, FALSE, window_ex_style, dpi) != FALSE;
}

class Preview {
public:
    ~Preview() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
        poller_.stop();
    }

    bool create(HINSTANCE instance, int show, KeyboardInputMode mode) {
        RECT work{};
        if (!work_area(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), work)) {
            fail(L"Could not read the monitor work area.");
            return false;
        }
        RECT bounds{};
        if (!window_bounds(GetDpiForSystem(), keyboard_visual_width, keyboard_visual_height, bounds)) {
            fail(L"Could not calculate the keyboard preview window size.");
            return false;
        }
        bounds = centered(bounds, work);
        if (CreateWindowExW(window_ex_style, window_class, window_title, window_style,
                bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                nullptr, nullptr, instance, this) == nullptr) {
            fail(L"Could not create the keyboard preview window.");
            return false;
        }
        if (!window_bounds(GetDpiForWindow(window_), keyboard_visual_width, keyboard_visual_height, bounds)) {
            fail(L"Could not calculate the keyboard preview DPI layout.");
            return false;
        }
        bounds = centered(bounds, work);
        if (!SetWindowPos(window_, nullptr, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE)) {
            fail(L"Could not position the keyboard preview inside the monitor work area.");
            return false;
        }
        model_.reset();
        model_.set_input_mode(mode);
        poller_.start();
        if (SetTimer(window_, refresh_timer, 16, nullptr) == 0) {
            fail(L"Could not start the keyboard preview refresh timer.");
            return false;
        }
        ShowWindow(window_, show == SW_HIDE ? SW_SHOWNORMAL : show);
        UpdateWindow(window_);
        return error_.empty();
    }

    const std::wstring& error() const {
        return error_;
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* preview = reinterpret_cast<Preview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            preview = static_cast<Preview*>(create->lpCreateParams);
            preview->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(preview));
        }
        if (preview == nullptr) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        try {
            return preview->dispatch(window, message, wparam, lparam);
        } catch (const std::bad_alloc&) {
            preview->fail(L"The keyboard preview could not update its composition or drawing resources.");
            return 0;
        }
    }

private:
    static RECT centered(const RECT& bounds, const RECT& work) {
        const LONG width = std::min(bounds.right - bounds.left, work.right - work.left);
        const LONG height = std::min(bounds.bottom - bounds.top, work.bottom - work.top);
        const LONG left = work.left + (work.right - work.left - width) / 2;
        const LONG top = work.top + (work.bottom - work.top - height) / 2;
        return {left, top, left + width, top + height};
    }

    bool accepts_input() const {
        return GetActiveWindow() == window_ && GetForegroundWindow() == window_ && !IsIconic(window_);
    }

    void fail(const wchar_t* reason) {
        if (error_.empty()) {
            error_ = reason;
        }
        if (window_ != nullptr) {
            KillTimer(window_, refresh_timer);
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    }

    void suspend() {
        model_.suspend();
        active_ = false;
        InvalidateRect(window_, nullptr, FALSE);
    }

    LRESULT dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_TIMER:
            if (wparam == refresh_timer && error_.empty()) {
                const bool focused = accepts_input();
                if (!focused) {
                    if (active_) {
                        suspend();
                    }
                    return 0;
                }
                const bool activated = !active_;
                active_ = true;
                const KeyboardAction action = model_.step(poller_.latest(), monotonic_now());
                if (action == KeyboardAction::Cancel) {
                    DestroyWindow(window);
                } else if (activated || action != KeyboardAction::None) {
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE || HIWORD(wparam) != 0) {
                suspend();
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && accepts_input()) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            visual_.system_theme_changed(message);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint(window);
            return 0;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                suspend();
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            RECT minimum{};
            RECT work{};
            if (window_bounds(GetDpiForWindow(window), minimum_width, minimum_height, minimum)
                && work_area(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), work)) {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                limits->ptMinTrackSize.x = std::min(minimum.right - minimum.left, work.right - work.left);
                limits->ptMinTrackSize.y = std::min(minimum.bottom - minimum.top, work.bottom - work.top);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            RECT bounds = *reinterpret_cast<const RECT*>(lparam);
            RECT work{};
            if (work_area(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), work)) {
                bounds = fit_to_work_area(bounds, work);
            }
            if (!SetWindowPos(window, nullptr, bounds.left, bounds.top,
                    bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE)) {
                fail(L"Could not resize the keyboard preview for the new display DPI.");
            }
            return 0;
        }
        case WM_DISPLAYCHANGE: {
            RECT bounds{};
            RECT work{};
            if (!IsIconic(window) && GetWindowRect(window, &bounds)
                && work_area(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), work)) {
                bounds = fit_to_work_area(bounds, work);
                SetWindowPos(window, nullptr, bounds.left, bounds.top,
                    bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, refresh_timer);
            poller_.stop();
            PostQuitMessage(error_.empty() ? 0 : 1);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            window_ = nullptr;
            break;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void paint(HWND window) {
        const KeyboardVisualState state{active_, true,
            model_.phase() == KeyboardPhase::Complete ? L"Preview complete - Menu restarts" : L"", {}};
        if (!visual_.paint(window, model_, state)) {
            fail(visual_.error().c_str());
        }
    }

    HWND window_ = nullptr;
    Daisywheel model_;
    ControllerPoller poller_;
    KeyboardVisual visual_;
    std::wstring error_;
    bool active_ = false;
};
} // namespace

int run_keyboard_preview(HINSTANCE instance, int show, KeyboardInputMode mode) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.style = CS_HREDRAW | CS_VREDRAW;
    type.lpfnWndProc = Preview::window_proc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = window_class;
    if (RegisterClassExW(&type) == 0) {
        MessageBoxW(nullptr, L"Could not register the keyboard preview window class.",
            window_title, MB_OK | MB_ICONERROR);
        return 1;
    }

    int result = 1;
    std::wstring error;
    try {
        Preview preview;
        if (preview.create(instance, show, mode)) {
            MSG message{};
            int status = 0;
            while ((status = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (status == -1) {
                error = L"The keyboard preview message loop failed.";
            } else {
                result = static_cast<int>(message.wParam);
            }
        }
        if (!preview.error().empty()) {
            error = preview.error();
            result = 1;
        }
    } catch (const std::bad_alloc&) {
        error = L"Could not allocate the keyboard preview or its controller reader.";
        result = 1;
    } catch (const std::system_error&) {
        error = L"Could not start the keyboard preview controller reader.";
        result = 1;
    }
    UnregisterClassW(window_class, instance);
    if (!error.empty()) {
        MessageBoxW(nullptr, error.c_str(), window_title, MB_OK | MB_ICONERROR);
    }
    return result;
}
} // namespace overworld
