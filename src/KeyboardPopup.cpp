#include "KeyboardPopup.h"
#include "KeyboardVisual.h"

#include <algorithm>

namespace gamepad {
namespace {
constexpr wchar_t popup_class[] = L"GamepadDesktopKeyboard";
constexpr DWORD popup_style = WS_POPUP;
constexpr DWORD popup_extended = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
}

KeyboardPopup::KeyboardPopup() = default;
KeyboardPopup::~KeyboardPopup() { close(); }

bool KeyboardPopup::create(HWND host)
{
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = window_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = popup_class;
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error_ = L"Keyboard window class registration failed";
        return false;
    }
    host_ = host;
    visual_ = std::make_unique<KeyboardVisual>();
    window_ = CreateWindowExW(popup_extended, popup_class, L"Gamepad Desktop - keyboard",
        popup_style, 0, 0, 1, 1, nullptr, nullptr, type.hInstance, this);
    if (!window_) {
        error_ = L"Keyboard window creation failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    return true;
}

bool KeyboardPopup::update(HWND host, KeyboardDelivery& keyboard, const VoicePresentation& voice)
{
    keyboard_ = &keyboard;
    voice_phase_ = voice.phase;
    voice_level_ = voice.level;
    const auto hint = voice.phase == VoicePhase::Idle ? std::wstring_view{} : voice.hint.substr(0, 120);
    if (voice_hint_ != hint) { voice_hint_ = hint; }
    if (!keyboard.visible()) {
        if (window_ && shown_) { ShowWindow(window_, SW_HIDE); }
        shown_ = false;
        return error_.empty();
    }
    if (!window_ && !create(host)) { return false; }
    if (!shown_) {
        const auto foreground = GetForegroundWindow();
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST), &monitor)) {
            error_ = L"Keyboard monitor work area unavailable";
            return false;
        }
        const auto dpi = GetDpiForWindow(foreground);
        RECT rect{0, 0, MulDiv(keyboard_visual_width, static_cast<int>(dpi ? dpi : 96), 96),
            MulDiv(keyboard.prediction_enabled() ? keyboard_prediction_height : keyboard_visual_height,
                static_cast<int>(dpi ? dpi : 96), 96)};
        const auto& work = monitor.rcWork;
        const int width = std::min<int>(rect.right - rect.left, work.right - work.left);
        const int height = std::min<int>(rect.bottom - rect.top, work.bottom - work.top);
        if (!SetWindowPos(window_, HWND_TOPMOST, work.left + (work.right - work.left - width) / 2,
            std::max<int>(work.top, work.bottom - height - MulDiv(16, static_cast<int>(dpi ? dpi : 96), 96)),
            width, height, SWP_NOACTIVATE)) {
            error_ = L"Keyboard popup presentation failed";
            return false;
        }
    }
    // Presentation is synchronous with the fresh model; unchanged state reuses its frame.
    paint();
    if (error_.empty() && !shown_) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        shown_ = true;
    }
    return error_.empty();
}

void KeyboardPopup::paint()
{
    if (!keyboard_) {
        ValidateRect(window_, nullptr);
        return;
    }
    if (error_.empty()) {
        const bool active = keyboard_->accepts_typing();
        const auto message = active ? std::wstring{} : keyboard_->status();
        KeyboardVisualState state{active, false, message, {}, {voice_phase_, voice_level_, voice_hint_}};
        state.word_suggestions = keyboard_->prediction_enabled();
        state.prediction = keyboard_->prediction_view();
        state.prediction_hint = keyboard_->prediction_hint();
        if (visual_->paint(window_, keyboard_->model(), state)) { return; }
        error_ = L"Keyboard drawing failed: " + visual_->error();
        keyboard_->block(L"Keyboard unavailable. View closes.");
        ShowWindow(window_, SW_HIDE);
        shown_ = false;
        PostMessageW(host_, keyboard_popup_message, 0, 0);
        return;
    }
    ValidateRect(window_, nullptr);
}

void KeyboardPopup::close()
{
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
    visual_.reset();
    keyboard_ = nullptr;
    voice_phase_ = VoicePhase::Idle;
    voice_level_ = 0;
    voice_hint_.clear();
    shown_ = false;
}

LRESULT CALLBACK KeyboardPopup::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* popup = reinterpret_cast<KeyboardPopup*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        popup = static_cast<KeyboardPopup*>(create->lpCreateParams);
        popup->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(popup));
    }
    if (!popup) { return DefWindowProcW(window, message, wparam, lparam); }
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: popup->paint(); return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        if (popup->visual_) { popup->visual_->system_theme_changed(message); }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        if (popup->keyboard_) { popup->keyboard_->dismiss(); }
        ShowWindow(window, SW_HIDE);
        popup->shown_ = false;
        PostMessageW(popup->host_, keyboard_popup_message, 0, 0);
        return 0;
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        if (!SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
            SWP_NOACTIVATE | SWP_NOZORDER)) {
            popup->error_ = L"Keyboard display scaling failed";
            if (popup->keyboard_) { popup->keyboard_->block(L"Keyboard unavailable. View closes."); }
            ShowWindow(window, SW_HIDE);
            popup->shown_ = false;
            PostMessageW(popup->host_, keyboard_popup_message, 0, 0);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_NCDESTROY: popup->window_ = nullptr; break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace gamepad
