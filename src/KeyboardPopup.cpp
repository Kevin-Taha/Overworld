#include "KeyboardPopup.h"
#include "KeyboardVisual.h"

#include <algorithm>

namespace overworld {
namespace {
constexpr wchar_t popup_class[] = L"OverworldKeyboard";
constexpr DWORD popup_style = WS_POPUP;
constexpr DWORD popup_extended = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
}

KeyboardPopup::KeyboardPopup(KeyboardThemeSource* source, KeyboardShellSource* shell)
    : theme_source_(source), shell_source_(shell) {}
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
    visual_ = std::make_unique<KeyboardVisual>(theme_source_);
    window_ = CreateWindowExW(popup_extended, popup_class, L"Overworld - keyboard",
        popup_style, 0, 0, 1, 1, nullptr, nullptr, type.hInstance, this);
    if (!window_) {
        error_ = L"Keyboard window creation failed (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    return true;
}

bool KeyboardPopup::update(HWND host, KeyboardDelivery& keyboard, const VoicePresentation& voice)
{
    return update_window(host, keyboard, voice, keyboard.visible() ? GetForegroundWindow() : nullptr, true);
}
bool KeyboardPopup::prepare(HWND host, KeyboardDelivery& keyboard, const VoicePresentation& voice)
{
    if (!keyboard.visible()) { return true; }
    return prepare_window(host, keyboard, voice, GetForegroundWindow(), true);
}

bool KeyboardPopup::update_window(HWND host, KeyboardDelivery& keyboard, const VoicePresentation& voice,
    HWND placement_target, bool reveal)
{
    keyboard_ = &keyboard;
    voice_phase_ = voice.phase;
    voice_level_ = voice.level;
    const auto hint = voice.phase == VoicePhase::Idle ? std::wstring_view{} : voice.hint.substr(0, 120);
    if (voice_hint_ != hint) { voice_hint_ = hint; }
    if (!keyboard.visible()) {
        if (window_ && shown_) { ShowWindow(window_, SW_HIDE); }
        shown_ = false;
        // Only an observed close permits a new presentation attempt; never retry the failed session.
        if (failed_) { close(); }
        // There is no draw to retry while closed. Retain the diagnostic without
        // making Host block every neutral sample needed for a deliberate new open.
        return true;
    }
    if (!prepare_window(host, keyboard, voice, placement_target, reveal || shell_source_)) { return false; }
    // Presentation is synchronous with the fresh model; unchanged state reuses its frame.
    paint();
    if (error_.empty() && !shown_) {
        if (reveal) { ShowWindow(window_, SW_SHOWNOACTIVATE); }
        shown_ = true;
    }
    return error_.empty();
}

bool KeyboardPopup::prepare_window(HWND host, KeyboardDelivery& keyboard, const VoicePresentation& voice,
    HWND placement_target, bool inspect_shell)
{
    keyboard_ = &keyboard;
    if (failed_) { return false; }
    if (!window_ && !create(host)) { fail(error_); return false; }
    KeyboardShellObservation shell;
    if (inspect_shell) { shell = (shell_source_ ? *shell_source_ : native_shell_).observe(placement_target); }
    if (!shell.available) { fail(L"Keyboard shell placement unavailable"); return false; }
    KeyboardVisualState state{keyboard.accepts_typing(), false, {}, {}, voice};
    state.word_suggestions = keyboard.prediction_enabled();
    state.prediction = keyboard.prediction_view();
    const auto logical_height = keyboard_frame_height(state);
    if (!shown_ || logical_height_ != logical_height || shell.obstruction || obstruction_) {
        RECT current{};
        MONITORINFO monitor{sizeof(monitor)};
        if ((shown_ && !GetWindowRect(window_, &current))
            || !GetMonitorInfoW(MonitorFromWindow(shown_ ? window_ : placement_target, MONITOR_DEFAULTTONEAREST), &monitor)) {
            fail(L"Keyboard monitor geometry unavailable");
            return false;
        }
        if (!place(monitor.rcWork, shown_ ? dpi_ : GetDpiForWindow(placement_target), logical_height,
                shown_ ? std::optional<RECT>{current} : std::nullopt, shell.obstruction)) { return false; }
    }
    obstruction_ = shell.obstruction;
    return true;
}

bool KeyboardPopup::place(RECT work, UINT dpi, int logical_height, std::optional<RECT> anchor, std::optional<RECT> obstruction)
{
    const auto placement = keyboard_popup_placement(work, dpi, logical_height, anchor, obstruction);
    if (!placement) {
        fail(obstruction ? L"No keyboard space beside Start or Search within the 20% compact limit"
            : L"Keyboard popup geometry invalid");
        return false;
    }
    const auto& bounds = placement->bounds;
    RECT current{};
    if (shown_ && GetWindowRect(window_, &current) && EqualRect(&current, &bounds) && dpi_ == dpi && logical_height_ == logical_height) {
        return true;
    }
    positioning_ = true;
    const bool positioned = SetWindowPos(window_, HWND_TOPMOST, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE) != FALSE;
    positioning_ = false;
    if (!positioned) { fail(L"Keyboard popup presentation failed"); return false; }
    dpi_ = dpi;
    logical_height_ = logical_height;
    return true;
}

void KeyboardPopup::fail(std::wstring reason)
{
    error_ = std::move(reason);
    failed_ = true;
    if (keyboard_) { keyboard_->block(L"Keyboard unavailable. View closes."); }
    if (window_) { ShowWindow(window_, SW_HIDE); }
    shown_ = false;
    PostMessageW(host_, keyboard_popup_message, 0, 0);
}

void KeyboardPopup::paint()
{
    if (!keyboard_ || positioning_) {
        ValidateRect(window_, nullptr);
        return;
    }
    if (!failed_) {
        const bool active = keyboard_->accepts_typing();
        const auto message = active ? std::wstring{} : keyboard_->status();
        KeyboardVisualState state{active, false, message, {}, {voice_phase_, voice_level_, voice_hint_}};
        state.word_suggestions = keyboard_->prediction_enabled();
        state.prediction = keyboard_->prediction_view();
        state.prediction_hint = keyboard_->prediction_hint();
        if (visual_->paint(window_, keyboard_->model(), state)) { error_.clear(); return; }
        fail(L"Keyboard drawing failed: " + visual_->error());
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
    failed_ = positioning_ = false;
    logical_height_ = 0;
    obstruction_.reset();
    native_shell_.reset();
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
        if (popup->positioning_ || popup->failed_) { return 0; }
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        MONITORINFO monitor{sizeof(monitor)};
        if (!rect || rect->right <= rect->left || rect->bottom <= rect->top
            || !GetMonitorInfoW(MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST), &monitor)) {
            popup->fail(L"Keyboard display scaling geometry unavailable");
        } else {
            popup->place(monitor.rcWork, HIWORD(wparam), popup->logical_height_, *rect, popup->obstruction_);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_NCDESTROY: popup->window_ = nullptr; break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace overworld
