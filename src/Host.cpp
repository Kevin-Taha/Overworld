#include "Core.h"
#include "BuildInfo.h"
#include "Desktop.h"
#include "CursorRing.h"
#include "NativeCursorVisibility.h"
#include "NativeCursorSurface.h"
#include "Resources.h"
#include "Settings.h"
#include "XInputReader.h"
#include "KeyboardPreview.h"
#include "KeyboardPopup.h"
#include "NativeNavigationFilter.h"
#include "NativeVoice.h"
#include "VoiceRecognizer.h"
#include "VoiceViewAdapter.h"
#include "HostShutdown.h"
#include "AppUi.h"
#include "SelfCaption.h"
#include "LaunchMode.h"
#include "InstanceGuard.h"

#include <Windows.h>
#include <Shellapi.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {
using namespace gamepad;
constexpr wchar_t window_class[] = L"GamepadDesktopM0";
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT restore_cursor_message = WM_APP + 3;
constexpr UINT self_caption_message = WM_APP + 20;
constexpr UINT_PTR timer_id = 1;
constexpr int hotkey_id = 1;
constexpr int event_limit = 128;
constexpr DWORD host_style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL;

bool simulation_control(int id)
{
    return id == Enable || id == StatusLine || id == SimulationGroup || id == SimulationNotice
        || id == Context || id == Session || id == Baseline || id == Modifier || id == MouseButton
        || id == Takeover || id == ContextSwitch;
}

std::wstring buttons_text(std::uint16_t buttons)
{
    struct ButtonName { std::uint16_t bit; const wchar_t* name; };
    const ButtonName names[] = {
        {0x0001, L"Up"}, {0x0002, L"Down"}, {0x0004, L"Left"}, {0x0008, L"Right"},
        {0x0010, L"Start"}, {0x0020, L"Back"}, {0x0040, L"LS"}, {0x0080, L"RS"},
        {0x0100, L"LB"}, {0x0200, L"RB"}, {0x1000, L"A"}, {0x2000, L"B"},
        {0x4000, L"X"}, {0x8000, L"Y"}
    };
    std::wstring text;
    for (const auto& [bit, name] : names) {
        if ((buttons & bit) != 0) {
            if (!text.empty()) {
                text += L" ";
            }
            text += name;
        }
    }
    return text.empty() ? L"(none)" : text;
}

std::wstring slot_text(unsigned index, const Controller& controller)
{
    std::wostringstream text;
    text << L"Slot " << index << L"  ";
    switch (controller.connection) {
    case Connection::Unknown: text << L"Waiting for reader"; break;
    case Connection::Disconnected: text << L"Disconnected (1167)"; break;
    case Connection::Error: text << L"READ ERROR " << controller.error; break;
    case Connection::Connected:
        text << L"Connected  Buttons: " << buttons_text(controller.buttons)
             << L"  [0x" << std::hex << std::setw(4) << std::setfill(L'0')
             << controller.buttons << std::dec << L"]"
             << L"\r\n        LT " << unsigned{controller.left_trigger}
             << L"  RT " << unsigned{controller.right_trigger}
             << L"  Left (" << controller.left_x << L", " << controller.left_y << L")"
             << L"  Right (" << controller.right_x << L", " << controller.right_y << L")"
             << L"  Packet " << controller.packet;
        break;
    }
    text << L"  Epoch " << controller.connection_epoch;
    return text.str();
}

struct Child {
    HWND window;
    int id;
    int x;
    int y;
    int width; // Zero fills the remaining client width.
    int height; // Zero fills the remaining client height.
    bool monospace;
    bool diagnostic;
    std::wstring text;
};

class Host {
public:
    Host(bool inspection, bool release_tap) : inspection_(inspection), live_(desktop_, release_tap)
    {
    }
    ~Host()
    {
        poller_.stop();
        if (font_) {
            DeleteObject(font_);
        }
        if (mono_font_) {
            DeleteObject(mono_font_);
        }
        if (title_font_) { DeleteObject(title_font_); }
    }

    bool create(HINSTANCE instance, int show)
    {
        instance_ = instance;
        WNDCLASSEXW klass{sizeof(klass)};
        klass.lpfnWndProc = window_proc;
        klass.hInstance = instance;
        klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        klass.hIconSm = klass.hIcon;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        klass.lpszClassName = window_class;
        if (!RegisterClassExW(&klass)) {
            MessageBoxW(nullptr, L"Could not register the utility window.", L"Gamepad Desktop", MB_ICONERROR);
            return false;
        }
        RECT bounds{0, 0, 488, 324};
        const UINT dpi = GetDpiForSystem();
        bounds.right = MulDiv(bounds.right, static_cast<int>(dpi), 96);
        bounds.bottom = MulDiv(bounds.bottom, static_cast<int>(dpi), 96);
        // AdjustWindowRectExForDpi does not account for scrollbars.
        bounds.right += GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
        bounds.bottom += GetSystemMetricsForDpi(SM_CYHSCROLL, dpi);
        AdjustWindowRectExForDpi(&bounds, host_style, FALSE, WS_EX_CONTROLPARENT, dpi);
        int width = bounds.right - bounds.left;
        int height = bounds.bottom - bounds.top;
        int left = CW_USEDEFAULT;
        int top = CW_USEDEFAULT;
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            width = (std::min)(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left - 24));
            height = (std::min)(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top - 24));
            left = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
            top = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
        }
        const HWND created = CreateWindowExW(WS_EX_CONTROLPARENT, window_class,
            dashboard_title(live_.release_tap()),
            host_style, left, top, width, height, nullptr, nullptr, instance, this);
        if (!created) {
            const auto message = L"Could not initialize Gamepad Desktop. Win32 error "
                + std::to_wstring(initialization_error_ ? initialization_error_ : GetLastError());
            MessageBoxW(nullptr, message.c_str(), L"Gamepad Desktop", MB_ICONERROR);
            return false;
        }
        ShowWindow(created, show);
        UpdateWindow(created);
        return true;
    }

    int run()
    {
        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (settings_window_ && settings_pages_.shortcut(settings_window_, message)) { continue; }
            if (!(settings_window_ && IsDialogMessageW(settings_window_, &message))
                && !IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (result == -1) {
            live_.pause(monotonic_now());
            pause_cursor(true);
            MessageBoxW(window_, L"Message loop failed; attempting to close Gamepad Desktop.", L"Gamepad Desktop", MB_ICONERROR);
            close(false);
            return 1;
        }
        if (IsWindow(window_)) { close(false); }
        return static_cast<int>(message.wParam);
    }

private:
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    HWND child(int id) const
    {
        for (const auto& item : children_) {
            if (item.id == id) {
                return item.window;
            }
        }
        return nullptr;
    }

    void text(int id, const std::wstring& value)
    {
        for (auto& item : children_) {
            if (item.id == id && item.text != value) {
                SetWindowTextW(item.window, value.c_str());
                item.text = value;
                break;
            }
        }
    }

    void show_child(int id, bool visible)
    {
        const auto window = child(id);
        if (((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0) != visible) {
            ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
        }
    }

    void enable_child(int id, bool enabled)
    {
        const auto window = child(id);
        if ((IsWindowEnabled(window) != FALSE) != enabled) { EnableWindow(window, enabled); }
    }

    void add(int id, const wchar_t* klass, const wchar_t* title, DWORD style,
             int x, int y, int width, int height, bool monospace = false)
    {
        HWND control = CreateWindowExW(0, klass, title, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!control && !initialization_error_) {
            initialization_error_ = GetLastError();
        }
        children_.push_back({control, id, x, y, width, height, monospace, adding_diagnostics_, title});
        if (simulation_control(id)) {
            SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) & ~WS_TABSTOP);
            ShowWindow(control, SW_HIDE);
        }
    }

    bool set_fonts()
    {
        HFONT font = CreateFontW(-px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT mono = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        HFONT title = CreateFontW(-px(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font || !mono || !title) {
            if (font) { DeleteObject(font); }
            if (mono) { DeleteObject(mono); }
            if (title) { DeleteObject(title); }
            return false;
        }
        for (const auto& item : children_) {
            SendMessageW(item.window, WM_SETFONT, reinterpret_cast<WPARAM>(
                (item.id == AppTitle && !live_.release_tap()) || item.id == Banner ? title : item.monospace ? mono : font), TRUE);
        }
        if (font_) { DeleteObject(font_); }
        if (mono_font_) { DeleteObject(mono_font_); }
        if (title_font_) { DeleteObject(title_font_); }
        font_ = font;
        mono_font_ = mono;
        title_font_ = title;
        return true;
    }

    void layout()
    {
        RECT bounds{};
        GetClientRect(window_, &bounds);
        const int width = (std::max)(static_cast<int>(bounds.right), px(diagnostics_ ? 688 : 488));
        const int height = (std::max)(static_cast<int>(bounds.bottom), px(diagnostics_ ? 1030 : 324));
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS};
        horizontal.nMax = width - 1;
        horizontal.nPage = static_cast<UINT>((std::max)(bounds.right, 0L));
        horizontal.nPos = GetScrollPos(window_, SB_HORZ);
        const int x_offset = SetScrollInfo(window_, SB_HORZ, &horizontal, TRUE);
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS};
        vertical.nMax = height - 1;
        vertical.nPage = static_cast<UINT>((std::max)(bounds.bottom, 0L));
        vertical.nPos = GetScrollPos(window_, SB_VERT);
        const int y_offset = SetScrollInfo(window_, SB_VERT, &vertical, TRUE);
        for (const auto& item : children_) {
            if (simulation_control(item.id)) { ShowWindow(item.window, SW_HIDE); continue; }
            if (item.diagnostic) {
                ShowWindow(item.window, diagnostics_ ? SW_SHOWNA : SW_HIDE);
                if (!diagnostics_) { continue; }
            }
            MoveWindow(item.window, px(item.x) - x_offset, px(item.y) - y_offset,
                item.width ? px(item.width) : width - px(item.x + 16),
                item.height ? px(item.height) : height - px(item.y + 16), TRUE);
        }
        SendMessageW(child(Events), LB_SETHORIZONTALEXTENT, px(1600), 0);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void scroll(int bar, WPARAM request)
    {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(window_, bar, &info);
        switch (LOWORD(request)) {
        case SB_LINEUP: info.nPos -= px(24); break;
        case SB_LINEDOWN: info.nPos += px(24); break;
        case SB_PAGEUP: info.nPos -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: info.nPos += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: info.nPos = info.nTrackPos; break;
        case SB_TOP: info.nPos = info.nMin; break;
        case SB_BOTTOM: info.nPos = info.nMax; break;
        default: return;
        }
        info.fMask = SIF_POS;
        SetScrollInfo(window_, bar, &info, TRUE);
        layout();
    }

    bool initialize()
    {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_UPDOWN_CLASS | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        if (!InitCommonControlsEx(&common)) { initialization_error_ = GetLastError(); return false; }
        dpi_ = GetDpiForWindow(window_);
        for (const auto& control : dashboard_controls()) {
            add(control.id, control.window_class, control.id == AppTitle ? dashboard_title(live_.release_tap()) : control.title, control.style,
                control.x, control.y, control.width, control.height);
        }

        adding_diagnostics_ = true;
        add(Infrastructure, L"STATIC", L"", SS_LEFT, 24, 340, 0, 24);
        add(Intro, L"STATIC", L"Controller preference", SS_LEFT, 24, 381, 0, 24);
        add(SlotChoice, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 24, 414, 298, 160);
        SendMessageW(child(SlotChoice), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic - first active pad"));
        for (unsigned index = 0; index < slot_count; ++index) {
            const auto label = L"Slot " + std::to_wstring(index) + L" - waiting";
            SendMessageW(child(SlotChoice), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(child(SlotChoice), CB_SETCURSEL, 0, 0);
        add(Select, L"BUTTON", L"&Use preference", BS_PUSHBUTTON | WS_TABSTOP, 338, 411, 182, 32);
        add(ConfigNotice, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
            24, 462, 0, 140);
        add(LiveNotice, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
            24, 612, 0, 92);
        for (unsigned index = 0; index < slot_count; ++index) {
            add(Slot0 + index, L"STATIC", L"Waiting for reader", SS_LEFT, 24, 716 + 42 * index, 0, 40, true);
        }
        add(Enable, L"BUTTON", L"&Enable M0 simulation", BS_PUSHBUTTON | WS_TABSTOP, 16, 772, 242, 32);
        add(Pause, L"BUTTON", L"&Pause desktop", BS_PUSHBUTTON | WS_TABSTOP, 24, 894, 200, 32);
        add(RestorePointer, L"BUTTON", L"Restore &Windows pointer", BS_PUSHBUTTON | WS_TABSTOP, 238, 894, 246, 32);
        add(StatusLine, L"STATIC", L"", SS_LEFT, 16, 812, 0, 44);
        add(SimulationGroup, L"BUTTON", L"Diagnostics - SIMULATED gates, not live observers",
            BS_GROUPBOX, 16, 866, 0, 118);
        add(Context, L"BUTTON", L"Allow simulated context", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 892, 238, 24);
        add(Session, L"BUTTON", L"Simulated session available", BS_AUTOCHECKBOX | WS_TABSTOP, 280, 892, 238, 24);
        add(Baseline, L"BUTTON", L"Simulated physical baseline known", BS_AUTOCHECKBOX | WS_TABSTOP, 534, 892, 380, 24);
        add(Modifier, L"BUTTON", L"Simulate modifier held", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 922, 238, 24);
        add(MouseButton, L"BUTTON", L"Simulate mouse button held", BS_AUTOCHECKBOX | WS_TABSTOP, 280, 922, 238, 24);
        add(Takeover, L"BUTTON", L"Simulate takeover", BS_PUSHBUTTON | WS_TABSTOP, 534, 920, 174, 28);
        add(ContextSwitch, L"BUTTON", L"Simulate context switch", BS_PUSHBUTTON | WS_TABSTOP, 720, 920, 242, 28);
        add(SimulationNotice, L"STATIC",
            L"These controls affect ONLY the M0 dry-run model. They NEVER authorize live output.",
            SS_LEFT, 28, 954, 930, 24);
        SendMessageW(child(Context), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(child(Session), BM_SETCHECK, BST_CHECKED, 0);
        add(EventHeading, L"STATIC", L"Recent state changes (memory only)",
            SS_LEFT, 24, 942, 0, 24);
        add(Events, L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL,
            24, 975, 0, 0, true);
        adding_diagnostics_ = false;
        if (initialization_error_ || !set_fonts()) {
            if (!initialization_error_) { initialization_error_ = ERROR_NOT_ENOUGH_MEMORY; }
            return false;
        }
        if (!app_ui_.refresh(window_)) { initialization_error_ = ERROR_NOT_ENOUGH_MEMORY; return false; }
        layout();
        add_event(L"Build: " GAMEPAD_BUILD_LABEL_W);
        add_event(inspection_ ? L"Inspection only: automatic input is disabled."
                              : L"Automatic control waits for fresh neutral then controller input.");
        update_status();
        hotkey_registered_ = RegisterHotKey(window_, hotkey_id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10) != FALSE;
        if (!hotkey_registered_) {
            hotkey_error_ = GetLastError();
            add_event(L"Emergency hotkey registration FAILED (conflict or OS error " + std::to_wstring(hotkey_error_) + L").");
        }
        desktop_.initialize(window_, hotkey_registered_);
        config_message_ = settings_path(settings_path_);
        if (config_message_.empty()) { load_configuration(); }
        else { config_available_ = false; }
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (!taskbar_created_) {
            add_event(L"Taskbar restart notification registration FAILED; tray may not recover after Explorer restart.");
        }
        install_tray();
        try {
            poller_.start();
            reader_started_ = true;
        } catch (const std::system_error& error) {
            reader_error_ = error.code().value();
            add_event(L"XInput worker could not start: system error " + std::to_wstring(reader_error_));
        }
        if (!SetTimer(window_, timer_id, 16, nullptr)) {
            initialization_error_ = GetLastError();
            return false;
        }
        refresh();
        return true;
    }

    void add_event(const std::wstring& value)
    {
        const auto line = L"+" + std::to_wstring(monotonic_now() - started_at_) + L" ms  " + value;
        const HWND list = child(Events);
        if (SendMessageW(list, LB_GETCOUNT, 0, 0) >= event_limit) {
            SendMessageW(list, LB_DELETESTRING, 0, 0);
        }
        const auto index = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        if (index == LB_ERR || index == LB_ERRSPACE) {
            event_view_failed_ = true;
        } else {
            SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
        }
    }

    void accept(const Result& result)
    {
        if (result.action) {
            const auto& action = *result.action;
            std::wstring line = action.kind == ActionKind::ADown ? L"SIMULATED A DOWN" : L"SIMULATED A UP";
            line += action.cleanup ? L" (owned cleanup): " : L": ";
            line += reason_text(action.reason);
            add_event(line);
        }
        if (!last_status_ || *last_status_ != result.status) {
            add_event(std::wstring{state_text(result.status.state)} + L": "
                      + std::wstring{reason_text(result.status.reason)});
            last_status_ = result.status;
            update_status();
        }
    }

    void update_status()
    {
        const auto status = core_.status();
        text(StatusLine, std::wstring{state_text(status.state)} + L" - "
             + std::wstring{reason_text(status.reason)} + L"\r\nOwned SIMULATED A: "
             + (core_.owns_a() ? L"down" : L"none") + L" | This diagnostic path NEVER emits desktop input");
        const auto selected = live_.selected_slot();
        std::wstring selection = L"Controller: " + (selected ? L"slot " + std::to_wstring(*selected) : L"waiting for activity");
        if (selected) {
            const auto& pad = snapshot_.slots[*selected];
            selection += pad.connection == Connection::Connected ? L" - connected" : L" - unavailable";
            if (pad.connection == Connection::Connected && !neutral(pad)) { selection += L" (activity)"; }
        }
        text(Selection, selection);
        if (tray_present_) {
            NOTIFYICONDATAW icon{sizeof(icon)};
            icon.hWnd = window_;
            icon.uID = 1;
            icon.uFlags = NIF_TIP;
            const auto tip = (std::wstring{dashboard_title(live_.release_tap())} + L" - " + std::wstring{live_.status()}).substr(0, 127);
            wcscpy_s(icon.szTip, tip.c_str());
            if (!Shell_NotifyIconW(NIM_MODIFY, &icon)) {
                tray_update_failed_ = true;
            }
        }
    }

    Gates gates() const
    {
        const auto checked = [&](int id) { return SendMessageW(child(id), BM_GETCHECK, 0, 0) == BST_CHECKED; };
        return {checked(Context), context_generation_, checked(Session), checked(Baseline),
                static_cast<std::uint16_t>(checked(Modifier) ? 1 : 0),
                static_cast<std::uint16_t>(checked(MouseButton) ? 1 : 0), physical_generation_};
    }

    void dismiss_keyboard(std::wstring_view reason)
    {
        suspend_navigation();
        if (!keyboard_.visible()) {
            keyboard_.block(reason); // Invalidate a pending opener without disturbing ordinary pointer routing.
            return;
        }
        keyboard_.dismiss(reason);
        if (!keyboard_popup_.update(window_, keyboard_, voice_view())) { render_live(); }
    }

    void configure_voice(bool enabled, std::wstring_view configured_path)
    {
        keyboard_.set_voice(nullptr);
        const bool allowed = enabled && !inspection_;
        std::filesystem::path model{configured_path};
        if (allowed && model.empty()) {
            wchar_t executable[32768]{};
            const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
            if (!length || length >= std::size(executable)) {
                voice_.configure(false, {});
                add_event(L"Local voice unavailable: executable model directory could not be resolved");
                return;
            }
            model = std::filesystem::path{executable}.parent_path() / L"models" / L"ggml-tiny.en.bin";
        }
        voice_.configure(allowed, std::move(model));
        if (allowed) { keyboard_.set_voice(&voice_); }
    }

    VoicePresentation voice_view() const
    {
        return voice_presentation(keyboard_.voice_status());
    }

    void refresh()
    {
        if (!shutdown_.allows_work()) { return; }
        live_.observe_text_fault(keyboard_.delivery().fault_generation(), monotonic_now());
        if (refreshing_) { return; }
        refreshing_ = true;
        struct RefreshGuard {
            Host& host;
            ~RefreshGuard()
            {
                host.refreshing_ = false;
                if (host.close_requested_) {
                    host.close_requested_ = false;
                    PostMessageW(host.window_, WM_CLOSE, 0, 0);
                }
            }
        } guard{*this};
        snapshot_ = poller_.latest();
        const auto now = monotonic_now();
        accept(core_.step(snapshot_, gates(), now));
        const auto desktop_requests = live_.desktop_requests();
        const bool prerequisites = !inspection_ && config_available_ && reader_started_
            && hotkey_registered_ && desktop_.available() && !core_.enabled() && !cursor_.recovery_pending();
        const bool keyboard_safe = prerequisites && !live_.owned() && !live_.faulted();
        bool native_keyboard = false;
        if (keyboard_safe && settings_.keyboard_in_native_mode && !keyboard_.recovery_required()) {
            native_keyboard = keyboard_.native_session();
            if (!native_keyboard) {
                const auto observation = desktop_.observe(0);
                native_keyboard = observation.available && observation.baseline_known && observation.routing_stable
                    && !observation.physical_held && observation.mode == live::Mode::Pointer
                    && live::keyboard_route_allowed(observation, true)
                    && (live_.automatically_paused() || observation.block == live::BlockReason::Auto);
            }
        }
        keyboard_.step(snapshot_, now, {keyboard_safe && !live_.automatically_paused() && !native_keyboard,
            live_.preferred_slot(), native_keyboard});
        live_.observe_text_fault(keyboard_.delivery().fault_generation(), monotonic_now());
        live_.hold_for_keyboard(keyboard_.blocks_pointer(), now);
        live_.automatic_step(snapshot_, now, prerequisites && !keyboard_.recovery_required());
        observe_self_caption();
        if (desktop_requests != live_.desktop_requests() && !inspection_) {
            if (cursor_.recovery_pending() || !cursor_.enable()) {
                live_.suspend_automatic(now, L"Cursor recovery required before resuming");
                cursor_status();
            }
        }
        update_cursor_presentation();
        cursor_status();
        if (!keyboard_popup_.update(window_, keyboard_, voice_view())) {
            keyboard_.block(L"Keyboard display unavailable. View closes.");
        }
        update_navigation();
        render_live();
        // Keep diagnostics rendering out of most input-loop iterations.
        if (now - rendered_at_ < 100) { return; }
        rendered_at_ = now;
        update_status();
        for (unsigned index = 0; index < slot_count; ++index) {
            if (diagnostics_) { text(Slot0 + index, slot_text(index, snapshot_.slots[index])); }
            const auto connection = snapshot_.slots[index].connection;
            if (!SendMessageW(child(SlotChoice), CB_GETDROPPEDSTATE, 0, 0)
                && (!slot_labels_[index] || *slot_labels_[index] != connection)) {
                const auto selection = SendMessageW(child(SlotChoice), CB_GETCURSEL, 0, 0);
                const auto label = L"Slot " + std::to_wstring(index) + L" - "
                    + (connection == Connection::Connected ? L"connected" : connection == Connection::Disconnected
                        ? L"disconnected" : connection == Connection::Error ? L"read error" : L"waiting");
                SendMessageW(child(SlotChoice), CB_DELETESTRING, index + 1, 0);
                SendMessageW(child(SlotChoice), CB_INSERTSTRING, index + 1, reinterpret_cast<LPARAM>(label.c_str()));
                SendMessageW(child(SlotChoice), CB_SETCURSEL, selection, 0);
                slot_labels_[index] = connection;
            }
        }
        if (diagnostics_) {
            text(LiveNotice, std::wstring{live_.status()} + L"\r\n"
                + desktop_.infrastructure() + L"\r\nOwned accepted bits (1=primary, 2=secondary, 4=Escape): "
                + std::to_wstring(live_.owned())
                + L"\r\n" + desktop_.attention_error()
                + L"\r\nEligibility: " + desktop_.diagnostic_summary()
                + L"\r\n" + navigation_filter_.status()
                + L"\r\nKeyboard: " + keyboard_.delivery().status()
                + L"\r\nMaximum controller-sample to dispatch: "
                + std::to_wstring(keyboard_.delivery().maximum_dispatch_ms()) + L" ms (software only)");
        }
    }

    void suspend_navigation()
    {
        if (navigation_allowed_) { ++navigation_scope_; navigation_allowed_ = false; }
        navigation_filter_.suspend();
    }

    void update_navigation()
    {
        if (!shutdown_.allows_work()) { suspend_navigation(); return; }
        const bool requested = navigation_filter_requested(settings_.experimental_navigation_filter, inspection_, config_available_);
        navigation_filter_.set_enabled(requested);
        const auto now = monotonic_now();
        bool allowed = false;
        NavigationForeground foreground;
        std::uint64_t context = 0;
        NavigationGate gate = requested ? NavigationGate::Prerequisites : NavigationGate::Off;
        live::BlockReason block = live::BlockReason::None;
        const bool keyboard = keyboard_.visible();
        const bool prerequisites = requested && reader_started_ && hotkey_registered_
            && desktop_.available() && !core_.enabled() && !cursor_.recovery_pending() && !live_.faulted()
            && !(live_.owned() && !live_.armed()) && !keyboard_.recovery_required();
        const bool fresh = snapshot_.sequence && snapshot_.sampled_at <= now
            && now - snapshot_.sampled_at <= navigation_policy_age_ms;
        if (prerequisites && !fresh) { gate = NavigationGate::StaleController; }
        if (prerequisites && fresh) {
            if (keyboard) {
                const auto observation = desktop_.observe_text(keyboard_.delivery().owned_key());
                allowed = keyboard_.navigation_filter_eligible(observation);
                context = observation.target.context;
                gate = allowed ? NavigationGate::Keyboard : NavigationGate::KeyboardBlocked;
            } else {
                const auto observation = desktop_.observe(live_.owned());
                allowed = navigation_desktop_eligible(observation, !live_.automatically_paused()
                    && !keyboard_.blocks_pointer(), prerequisites);
                context = observation.context;
                block = observation.block;
                gate = allowed ? NavigationGate::Desktop : live_.automatically_paused()
                    ? NavigationGate::NativeMode : NavigationGate::DesktopBlocked;
            }
            foreground = desktop_.observed_foreground();
            if (navigation_settings_blocked(reinterpret_cast<std::uintptr_t>(settings_window_), foreground)) {
                allowed = false;
                gate = NavigationGate::Settings;
            }
        }
        if (allowed != navigation_allowed_ || foreground != navigation_foreground_
            || context != navigation_context_ || keyboard != navigation_keyboard_) { ++navigation_scope_; }
        navigation_allowed_ = allowed;
        navigation_foreground_ = foreground;
        navigation_context_ = context;
        navigation_keyboard_ = keyboard;
        // issued precedes validation: slow validation cannot grant a fresh lease on old evidence.
        navigation_filter_.publish({foreground, now, navigation_scope_, allowed, gate, block});
    }

    void render_live()
    {
        const auto live_status = std::wstring{live_.status()};
        const auto effective = live_.effective_mode();
        const bool app_yield = effective == live::EffectiveMode::AutoNative;
        text(Banner, inspection_ ? L"Preview - input disabled"
            : cursor_.recovery_pending() || live_.faulted() || keyboard_.recovery_required() || !config_available_
                || !desktop_.available() || !hotkey_registered_ || !reader_started_
                || effective == live::EffectiveMode::Blocked ? L"Needs attention"
            : keyboard_.visible() ? L"Keyboard"
            : live_.automatically_paused() || app_yield ? L"Native gamepad"
            : effective == live::EffectiveMode::Physical ? L"Mouse and keyboard"
            : core_.enabled() ? L"Simulation"
            : live_.controller_active() ? L"Active" : L"Ready");
        const wchar_t* summary = L"Move a controller stick to use the desktop.";
        if (inspection_) { summary = L"Inspection only. No desktop input is sent."; }
        else if (cursor_.recovery_pending()) { summary = L"Restore the Windows pointer from Settings."; }
        else if (live_.faulted()) { summary = L"Release physical buttons, then choose Retry release."; }
        else if (keyboard_.recovery_required()) { summary = L"Text output needs recovery. Release physical keys, then Retry release."; }
        else if (keyboard_.visible()) { summary = L"Type with this controller. Menu Enter; View closes."; }
        else if (live_.automatically_paused()) { summary = L"Desktop mapping off. Menu + View switches back."; }
        else if (!config_available_) { summary = L"Open Settings to resolve a configuration error."; }
        else if (!desktop_.available() || !hotkey_registered_ || !reader_started_) { summary = L"Input unavailable. See Details."; }
        else if (live_.owned() && !live_.armed()) { summary = L"Finishing a button release..."; }
        else if (live_.controller_active()) { summary = L"Mouse or keyboard input takes over automatically."; }
        else if (effective == live::EffectiveMode::Physical) { summary = L"Release physical controls, center the controller, then move a stick to take over."; }
        else if (app_yield) { summary = L"Auto selected native input. Menu+View requests desktop for this app."; }
        else if (effective == live::EffectiveMode::Blocked) { summary = L"Desktop request blocked by an eligibility check. See Details."; }
        else if (live_.selected_slot()) { summary = L"Center the stick, then move it to continue."; }
        text(LiveStatus, summary);
        if (!inspection_ && keyboard_.blocks_pointer() && !keyboard_.status().empty()) { text(LiveStatus, keyboard_.status()); }
        if (!keyboard_popup_.error().empty()) { text(LiveStatus, keyboard_popup_.error() + L". View closes the keyboard."); }
        if (effective == live::EffectiveMode::Blocked && !live_.faulted() && !keyboard_.recovery_required()
            && !cursor_.recovery_pending()) { text(LiveStatus, std::wstring{live::block_reason_text(live_.blocked_reason())}); }
        show_child(ResumeAutomatic, (effective != live::EffectiveMode::Desktop && effective != live::EffectiveMode::Physical)
            || cursor_.replacement_suspended());
        enable_child(ResumeAutomatic, !inspection_ && desktop_.available() && hotkey_registered_ && reader_started_
                     && !live_.faulted() && !keyboard_.recovery_required() && config_available_ && !cursor_.recovery_pending());
        show_child(RetryCleanup, live_.faulted() || keyboard_.recovery_required());
        std::wstring infrastructure = !hotkey_registered_ ? L"UNAVAILABLE: Ctrl+Alt+F10 conflict/error " + std::to_wstring(hotkey_error_)
            : !desktop_.available() ? L"Input observer unavailable. See Details."
            : !reader_started_ ? L"UNAVAILABLE: controller reader error " + std::to_wstring(reader_error_)
            : settings_.keyboard_input_mode == KeyboardInputMode::OneHanded
                ? L"LS click/Y: keys   Menu+View: mode   Ctrl+Alt+F10"
                : L"Y: keyboard   Menu + View: toggle   Ctrl+Alt+F10: pause";
        text(Infrastructure, infrastructure);
        std::wstring config = L"Build: " GAMEPAD_BUILD_LABEL_W L"\r\n";
        if (!keyboard_.last_dismissal().empty()) { config += L"Last keyboard dismissal: " + keyboard_.last_dismissal() + L"\r\n"; }
        config += config_available_ ? config_summary_ : L"Settings unavailable. Open Settings for the error and Reload file.";
        config += cursor_.owns_hide() ? L"\r\nCursor replacement owns native hiding."
            : cursor_.replacement_suspended() ? L"\r\nCustom cursor suspended; choose Desktop control to retry."
            : L"\r\nCustom cursor appears after controller use; mouse input restores Windows pointer.";
        if (!cursor_.error().empty()) { config += L"\r\nCursor error: see Settings / Restore Windows pointer."; }
        if (dpi_font_failed_) { config += L"\r\nDPI font allocation failed; previous font retained."; }
        if (event_view_failed_) { config += L"\r\nDiagnostic event display failed."; }
        if (settings_.experimental_navigation_filter || navigation_filter_.faulted()) {
            config += L"\r\n" + navigation_filter_.status();
        }
        text(ConfigNotice, config);
        if (last_live_status_ != live_status) {
            last_live_status_ = live_status;
            add_event(live_status);
            update_status();
        }
    }

    void load_configuration()
    {
        if (!shutdown_.allows_work()) { return; }
        suspend_navigation();
        keyboard_.block(L"Configuration changed. View closes.");
        if (settings_path_.empty()) {
            config_message_ = settings_path(settings_path_);
            if (!config_message_.empty()) { config_available_ = false; return; }
        }
        settings_file_ = load_settings(settings_path_);
        config_available_ = settings_file_.state == LoadState::Loaded || settings_file_.state == LoadState::Missing;
        config_message_ = settings_file_.error;
        if (!config_available_) { return; }
        const auto next = settings_file_.settings;
        const auto now = monotonic_now();
        const auto pointer_error = live_.set_pointer_settings(next.pointer, now);
        const auto exclusion_error = live_.set_exclusions(next.exclusions, now);
        if (!pointer_error.empty() || !exclusion_error.empty()) {
            config_available_ = false;
            config_message_ = std::wstring{pointer_error} + L" " + std::wstring{exclusion_error};
            return;
        }
        settings_ = next;
        desktop_.set_automatic_game_switching(next.automatic_game_switching);
        keyboard_.set_input_mode(next.keyboard_input_mode);
        keyboard_.set_native_mode_enabled(next.keyboard_in_native_mode);
        keyboard_.set_prediction_enabled(next.word_suggestions);
        configure_voice(next.voice_enabled, next.voice_model_path);
        if (!cursor_.apply()) { cursor_status(); }
        config_summary_ = settings_file_.state == LoadState::Missing ? L"Default settings (not saved yet)." : L"Saved settings loaded; automatic control waits for fresh input.";
        config_message_ = config_summary_;
    }

    void update_band_controls()
    {
        const bool enabled = SendDlgItemMessageW(settings_window_, IDC_MOTION_PROFILE, CB_GETCURSEL, 0, 0) == 1;
        for (int i = 0; i < 3; ++i) {
            EnableWindow(GetDlgItem(settings_window_, IDC_BAND_LOW + i), enabled);
            EnableWindow(GetDlgItem(settings_window_, IDC_SPIN_FIRST + 8 + i), enabled);
        }
    }

    void update_voice_controls()
    {
        update_voice_settings_controls(settings_window_, VoiceRecognizer::supported());
    }

    void browse_voice_model()
    {
        std::array<wchar_t, voice_model_path_limit + 1> file{};
        const auto current = settings_text(IDC_VOICE_MODEL_PATH);
        if (current.size() <= voice_model_path_limit) { std::copy(current.begin(), current.end(), file.begin()); }
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = settings_window_;
        dialog.lpstrFilter = L"tiny.en model (*.bin)\0*.bin\0All files\0*.*\0";
        dialog.lpstrFile = file.data();
        dialog.nMaxFile = static_cast<DWORD>(file.size());
        dialog.lpstrTitle = L"Choose the verified tiny.en English model";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
        if (GetOpenFileNameW(&dialog)) {
            SetDlgItemTextW(settings_window_, IDC_VOICE_MODEL_PATH, file.data());
        } else if (const auto error = CommDlgExtendedError(); error != 0) {
            settings_result(L"The model picker could not open (error " + std::to_wstring(error) + L").");
        }
    }

    void populate_settings()
    {
        const auto fields = settings_fields(edit_base_);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            SetDlgItemTextW(settings_window_, IDC_SPEED + static_cast<int>(i), fields[i].c_str());
        }
        for (std::size_t i = 0; i < cursor_styles.size(); ++i) {
            if (cursor_styles[i].style == edit_base_.cursor_style) {
                SendDlgItemMessageW(settings_window_, IDC_CURSOR_STYLE, CB_SETCURSEL, i, 0);
            }
        }
        SendDlgItemMessageW(settings_window_, IDC_MOTION_PROFILE, CB_SETCURSEL,
            edit_base_.pointer.profile == live::MotionProfile::Smooth ? 0 : 1, 0);
        update_band_controls();
        SendDlgItemMessageW(settings_window_, IDC_KEYBOARD_INPUT_MODE, CB_SETCURSEL,
            edit_base_.keyboard_input_mode == KeyboardInputMode::FaceButtons ? 0 : 1, 0);
        CheckDlgButton(settings_window_, IDC_AUTO_GAME_SWITCHING, edit_base_.automatic_game_switching ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(settings_window_, IDC_NATIVE_KEYBOARD, edit_base_.keyboard_in_native_mode ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(settings_window_, IDC_NAVIGATION_FILTER, edit_base_.experimental_navigation_filter ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(settings_window_, IDC_VOICE_ENABLED, edit_base_.voice_enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(settings_window_, IDC_WORD_SUGGESTIONS, edit_base_.word_suggestions ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(settings_window_, IDC_VOICE_MODEL_PATH, edit_base_.voice_model_path.c_str());
        update_voice_controls();
        SetDlgItemTextW(settings_window_, IDC_EXCLUSIONS, exclusion_text(edit_base_).c_str());
        InvalidateRect(GetDlgItem(settings_window_, IDC_CURSOR_PREVIEW), nullptr, TRUE);
    }

    std::wstring settings_text(int id) const
    {
        const auto control = GetDlgItem(settings_window_, id);
        const int length = GetWindowTextLengthW(control);
        std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(control, text.data(), length + 1);
        text.resize(static_cast<std::size_t>(copied));
        return text;
    }

    void settings_result(const std::wstring& message)
    {
        SetDlgItemTextW(settings_window_, IDC_SETTINGS_RESULT, message.c_str());
    }

    void update_cursor_presentation()
    {
        start_cursor_fallback_.update(cursor_, cursor_surface_.observe(), live_.controller_active(),
            settings_, live_.auto_override_active());
        const auto status = start_cursor_fallback_.status();
        if (status != cursor_surface_status_) {
            cursor_surface_status_ = status;
            add_event(status.empty() ? L"Cursor surface clear: normal presentation policy applies." : cursor_surface_status_);
        }
    }

    static CaptionButton caption_button(LRESULT hit)
    {
        if (hit == HTMINBUTTON) { return CaptionButton::Minimize; }
        if (hit == HTMAXBUTTON) { return CaptionButton::Maximize; }
        if (hit == HTCLOSE) { return CaptionButton::Close; }
        return CaptionButton::None;
    }

    CaptionContext caption_context(HWND target)
    {
        CaptionContext context;
        context.window = reinterpret_cast<std::uintptr_t>(target);
        context.foreground = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
        context.focus = reinterpret_cast<std::uintptr_t>(GetFocus());
        const auto slot = live_.selected_slot();
        const auto sample = poller_.latest();
        const auto now = monotonic_now();
        DWORD process = 0;
        const auto thread = target ? GetWindowThreadProcessId(target, &process) : 0;
        context.eligible = (target == window_ || target == settings_window_) && target
            && thread == GetCurrentThreadId() && process == GetCurrentProcessId()
            && GetForegroundWindow() == target && IsWindowVisible(target) && IsWindowEnabled(target) && !IsIconic(target)
            && !inspection_ && shutdown_.allows_work() && live_.controller_active() && live_.armed()
            && !live_.faulted() && !live_.automatically_paused() && !keyboard_.blocks_pointer()
            && !(live_.owned() & ~static_cast<unsigned>(live::Primary))
            && !cursor_.recovery_pending() && slot && *slot < sample.slots.size()
            && sample.sequence && sample.sampled_at <= now && now - sample.sampled_at <= live::live_sample_age_ms;
        if (slot && *slot < sample.slots.size()) {
            const auto& pad = sample.slots[*slot];
            context.slot = *slot;
            context.epoch = pad.connection_epoch;
            context.primary_down = (pad.buttons & a_button) != 0;
            context.eligible = context.eligible && pad.connection == Connection::Connected;
        }
        context.primary_owned = (live_.owned() & live::Primary) != 0;
        POINT point{};
        if (context.eligible && GetCursorPos(&point) && GetAncestor(WindowFromPoint(point), GA_ROOT) == target) {
            const auto hit = DefWindowProcW(target, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
            context.button = caption_button(hit);
        }
        return context;
    }

    void observe_self_caption()
    {
        if (self_caption_.pending()) {
            self_caption_.observe(caption_context(reinterpret_cast<HWND>(self_caption_.window())));
        }
    }

    bool caption_message(HWND target, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONDBLCLK) {
            const bool tagged = static_cast<ULONG_PTR>(GetMessageExtraInfo()) == live::output_tag;
            if (!tagged) { self_caption_.cancel(); return false; }
            auto context = caption_context(target);
            const auto message_button = caption_button(static_cast<LRESULT>(wparam));
            context.eligible = context.eligible && context.button == message_button;
            context.button = message_button; // Retain the message's scope even when current hit/eligibility is lost.
            return self_caption_.down(context, tagged, message == WM_NCLBUTTONDBLCLK);
        }
        if (message == WM_NCLBUTTONUP || message == WM_LBUTTONUP) {
            if (!self_caption_.pending()) { return false; }
            const bool tagged = static_cast<ULONG_PTR>(GetMessageExtraInfo()) == live::output_tag;
            auto context = caption_context(target);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (message == WM_LBUTTONUP && !ClientToScreen(target, &point)) { context.eligible = false; }
            const auto hit = DefWindowProcW(target, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
            context.eligible = context.eligible && context.button == caption_button(hit)
                && (message != WM_NCLBUTTONUP || context.button == caption_button(static_cast<LRESULT>(wparam)));
            const bool ready = self_caption_.up(context, tagged);
            if (ready && !PostMessageW(window_, self_caption_message, 0, 0)) {
                self_caption_.cancel();
                add_event(L"Caption action could not be queued; no window command was performed.");
            }
            return tagged;
        }
        if (message == self_caption_message) {
            if (const auto action = self_caption_.take(caption_context(reinterpret_cast<HWND>(self_caption_.window())))) {
                const auto window = reinterpret_cast<HWND>(action->window);
                const WPARAM command = action->button == CaptionButton::Minimize ? SC_MINIMIZE
                    : action->button == CaptionButton::Maximize ? (IsZoomed(window) ? SC_RESTORE : SC_MAXIMIZE) : SC_CLOSE;
                // The tagged up has already been dispatched. lParam=0 does not
                // enter the non-client mouse tracking loop that starves WM_TIMER.
                SendMessageW(window, WM_SYSCOMMAND, command, 0);
            }
            return true;
        }
        if (message == WM_NCMOUSEMOVE || message == WM_MOUSEMOVE) { observe_self_caption(); }
        if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED || message == WM_KILLFOCUS
            || message == WM_ACTIVATE || message == WM_DESTROY || message == WM_NCDESTROY) { self_caption_.cancel(); }
        return false;
    }

    void cursor_status()
    {
        if (cursor_.recovery_pending()) {
            keyboard_.block(L"Pointer recovery required. View closes.");
            live_.suspend_automatic(monotonic_now(), L"PAUSED - Windows pointer restoration needs explicit recovery");
            accept(core_.pause(monotonic_now()));
        }
        const auto error = cursor_.error();
        if (last_cursor_error_ != error) {
            last_cursor_error_ = error;
            if (!error.empty()) {
                add_event(error);
                if (settings_window_) { settings_result(error); }
            }
        }
    }

    bool pause_cursor(bool retry = false)
    {
        self_caption_.cancel();
        const bool restored = cursor_.pause(retry);
        cursor_status();
        return restored;
    }

    bool restore_pointer(bool report_dialog = true)
    {
        keyboard_.block(L"Pointer restored. View closes.");
        live_.suspend_automatic(monotonic_now(), L"Pointer restored; Menu+View or Desktop control resumes mapping");
        accept(core_.pause(monotonic_now()));
        const bool restored = cursor_.restore();
        cursor_status();
        const auto result = restored
            ? L"Windows pointer restored. Menu+View or Desktop control resumes mapping. No settings saved."
            : cursor_.error();
        add_event(result);
        if (restored) { config_summary_ = L"Windows pointer show request succeeded. Automatic control manually paused."; }
        if (settings_window_) { settings_result(result); }
        if (!restored && report_dialog) {
            show();
            MessageBoxW(window_, result.c_str(), L"Gamepad Desktop - restore failed", MB_OK | MB_ICONERROR);
        }
        render_live();
        return restored;
    }

    void draw_cursor_preview(const DRAWITEMSTRUCT& item)
    {
        FillRect(item.hDC, &item.rcItem, GetSysColorBrush(COLOR_BTNFACE));
        auto label = item.rcItem;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, GetSysColor(COLOR_BTNTEXT));
        const auto index = SendDlgItemMessageW(settings_window_, IDC_CURSOR_STYLE, CB_GETCURSEL, 0, 0);
        double size = 0;
        if (index < 0 || static_cast<std::size_t>(index) >= cursor_styles.size()
            || !settings_number(settings_text(IDC_DIAMETER), size) || size < 20 || size > 96 || std::floor(size) != size) {
            DrawTextW(item.hDC, L"Choose style / valid size", -1, &label, DT_CENTER | DT_WORDBREAK);
            return;
        }
        const auto dpi = GetDpiForWindow(settings_window_);
        const int header = MulDiv(16, static_cast<int>(dpi), 96);
        const int available_width = item.rcItem.right - item.rcItem.left;
        const int available_height = item.rcItem.bottom - item.rcItem.top - header;
        if (available_width <= 0 || available_height <= 0) { return; }
        const auto style = cursor_styles[static_cast<std::size_t>(index)].style;
        // The actual renderer's premultiplied pixels, composited locally: no overlay or native visibility call.
        try {
            const auto geometry_value = cursor_geometry({}, dpi, static_cast<int>(size), style);
            if (!geometry_value) { settings_result(L"Cursor preview DPI unsupported; live cursor settings are unchanged."); return; }
            const auto geometry = *geometry_value;
            const auto pixels = cursor_pixels(dpi, static_cast<int>(size), style);
            const int side = geometry.side;
            const int nominal = ring_geometry({}, dpi, static_cast<int>(size))->side;
            const int inset = (nominal - side) / 2;
            const int width = nominal * 2;
            std::vector<std::uint32_t> preview(static_cast<std::size_t>(width) * nominal);
            for (int y = 0; y < nominal; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int source_x = x % nominal - inset, source_y = y - inset;
                    const auto pixel = source_x >= 0 && source_x < side && source_y >= 0 && source_y < side
                        ? pixels[static_cast<std::size_t>(source_y) * side + source_x] : 0;
                    const auto a = pixel >> 24;
                    const unsigned background = x < nominal ? 242 : 32;
                    const unsigned backdrop = (background * (255 - a) + 127) / 255;
                    const unsigned red = ((pixel >> 16) & 255) + backdrop;
                    const unsigned green = ((pixel >> 8) & 255) + backdrop;
                    const unsigned blue = (pixel & 255) + backdrop;
                    preview[static_cast<std::size_t>(y) * width + x] = (red << 16) | (green << 8) | blue;
                }
            }
            // Keep dot visually compact relative to other styles, even though its allocation is smaller.
            const double scale = (std::min)({1.0, available_width / (nominal * 2.0), available_height / static_cast<double>(nominal)});
            DrawTextW(item.hDC, scale < 1 ? L"Light / dark (fit)" : L"Light / dark (actual)", -1, &label, DT_CENTER | DT_SINGLELINE);
            const int draw_width = (std::max)(1, static_cast<int>(width * scale));
            const int draw_height = (std::max)(1, static_cast<int>(nominal * scale));
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -nominal;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            const int saved_dc = SaveDC(item.hDC);
            if (!saved_dc) { settings_result(L"Cursor preview DC save failed."); return; }
            if (!SetStretchBltMode(item.hDC, HALFTONE) || !SetBrushOrgEx(item.hDC, 0, 0, nullptr)) {
                RestoreDC(item.hDC, saved_dc);
                settings_result(L"Cursor preview filtering unavailable.");
                return;
            }
            const auto result = StretchDIBits(item.hDC, item.rcItem.left + (available_width - draw_width) / 2,
                item.rcItem.top + header + (available_height - draw_height) / 2, draw_width, draw_height,
                0, 0, width, nominal, preview.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            RestoreDC(item.hDC, saved_dc);
            if (result == GDI_ERROR || result == 0) { settings_result(L"Cursor preview drawing failed; live cursor settings are unchanged."); }
        } catch (const std::bad_alloc&) {
            settings_result(L"Cursor preview allocation failed; live cursor settings are unchanged.");
        }
    }

    void apply_settings()
    {
        if (!shutdown_.allows_work()) { return; }
        suspend_navigation();
        keyboard_.block(L"Settings changed. View closes.");
        SettingsFields fields;
        for (std::size_t i = 0; i < fields.size(); ++i) { fields[i] = settings_text(IDC_SPEED + static_cast<int>(i)); }
        Settings next;
        const auto style_index = SendDlgItemMessageW(settings_window_, IDC_CURSOR_STYLE, CB_GETCURSEL, 0, 0);
        if (style_index < 0 || static_cast<std::size_t>(style_index) >= cursor_styles.size()) {
            settings_result(L"Choose a valid cursor style. Previous values unchanged.");
            return;
        }
        auto editor = edit_base_;
        editor.cursor_style = cursor_styles[static_cast<std::size_t>(style_index)].style;
        const auto profile = SendDlgItemMessageW(settings_window_, IDC_MOTION_PROFILE, CB_GETCURSEL, 0, 0);
        if (profile != 0 && profile != 1) {
            settings_result(L"Choose a valid motion profile. Previous values unchanged.");
            return;
        }
        editor.pointer.profile = profile == 0 ? live::MotionProfile::Smooth : live::MotionProfile::SpeedBands;
        const auto keyboard_mode = SendDlgItemMessageW(settings_window_, IDC_KEYBOARD_INPUT_MODE, CB_GETCURSEL, 0, 0);
        if (keyboard_mode != 0 && keyboard_mode != 1) {
            settings_result(L"Choose a valid keyboard input mode. Previous values unchanged.");
            return;
        }
        editor.keyboard_input_mode = keyboard_mode == 0 ? KeyboardInputMode::FaceButtons : KeyboardInputMode::OneHanded;
        const auto automatic = IsDlgButtonChecked(settings_window_, IDC_AUTO_GAME_SWITCHING);
        if (automatic != BST_CHECKED && automatic != BST_UNCHECKED) {
            settings_result(L"Choose a valid automatic game switching state. Previous values unchanged.");
            return;
        }
        editor.automatic_game_switching = automatic == BST_CHECKED;
        const auto native_keyboard = IsDlgButtonChecked(settings_window_, IDC_NATIVE_KEYBOARD);
        if (native_keyboard != BST_CHECKED && native_keyboard != BST_UNCHECKED) {
            settings_result(L"Choose a valid native-mode keyboard option. Previous values unchanged.");
            return;
        }
        editor.keyboard_in_native_mode = native_keyboard == BST_CHECKED;
        const auto filter = IsDlgButtonChecked(settings_window_, IDC_NAVIGATION_FILTER);
        if (filter != BST_CHECKED && filter != BST_UNCHECKED) {
            settings_result(L"Choose a valid experimental navigation filter option. Previous values unchanged.");
            return;
        }
        editor.experimental_navigation_filter = filter == BST_CHECKED;
        const auto voice = IsDlgButtonChecked(settings_window_, IDC_VOICE_ENABLED);
        if (voice != BST_CHECKED && voice != BST_UNCHECKED) {
            settings_result(L"Choose a valid local voice option. Previous values unchanged.");
            return;
        }
        editor.voice_enabled = voice == BST_CHECKED;
        const auto prediction = IsDlgButtonChecked(settings_window_, IDC_WORD_SUGGESTIONS);
        if (prediction != BST_CHECKED && prediction != BST_UNCHECKED) {
            settings_result(L"Choose a valid word suggestions option. Previous values unchanged.");
            return;
        }
        editor.word_suggestions = prediction == BST_CHECKED;
        editor.voice_model_path = settings_text(IDC_VOICE_MODEL_PATH);
        if (auto error = parse_settings_fields(fields,
            settings_text(IDC_EXCLUSIONS), editor, next); !error.empty()) {
            settings_result(error + L" Previous values unchanged.");
            return;
        }
        const auto now = monotonic_now();
        live_.pause(now, L"Settings apply: automatic control waits for new neutral and input");
        accept(core_.pause(now));
        if (!pause_cursor()) { settings_result(cursor_.error() + L" Settings were not saved."); render_live(); return; }
        if (auto error = save_settings(settings_path_, next, settings_file_); !error.empty()) {
            config_summary_ = L"Settings NOT saved. Previous values retained; see Settings.";
            config_message_ = error;
            settings_result(error);
            render_live();
            return;
        }
        const auto pointer_error = live_.set_pointer_settings(next.pointer, now);
        const auto exclusion_error = live_.set_exclusions(next.exclusions, now);
        if (!pointer_error.empty() || !exclusion_error.empty()) {
            config_available_ = false;
            config_message_ = L"Saved, but runtime apply rejected: " + std::wstring{pointer_error} + L" " + std::wstring{exclusion_error};
            settings_result(config_message_);
        } else {
            settings_ = edit_base_ = next;
            desktop_.set_automatic_game_switching(next.automatic_game_switching);
            keyboard_.set_input_mode(next.keyboard_input_mode);
            keyboard_.set_native_mode_enabled(next.keyboard_in_native_mode);
            keyboard_.set_prediction_enabled(next.word_suggestions);
            configure_voice(next.voice_enabled, next.voice_model_path);
            config_available_ = true;
            const bool cursor_applied = cursor_.apply();
            cursor_status();
            config_message_ = config_summary_ = cursor_applied
                ? L"Settings saved. Automatic control waits for new neutral and controller input."
                : L"Settings saved, but cursor recovery is required before automatic control.";
            settings_result(config_summary_ + L"\r\n" + cursor_.error()
                + (live_.faulted() ? L" CLEANUP FAULT remains: use Retry owned cleanup in the main window." : L""));
            populate_settings();
        }
        render_live();
    }

    void open_settings()
    {
        if (!shutdown_.allows_work()) { return; }
        if (!settings_window_) {
            edit_base_ = settings_;
            settings_window_ = CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_SETTINGS), window_,
                settings_proc, reinterpret_cast<LPARAM>(this));
            if (!settings_window_) {
                config_summary_ = L"Settings window could not open (error " + std::to_wstring(GetLastError()) + L").";
                render_live();
                return;
            }
        }
        ShowWindow(settings_window_, SW_SHOW);
        SetForegroundWindow(settings_window_);
    }

    static INT_PTR CALLBACK settings_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, DWLP_USER));
        if (message == WM_INITDIALOG) {
            host = reinterpret_cast<Host*>(lparam);
            host->settings_window_ = window;
            SetWindowLongPtrW(window, DWLP_USER, lparam);
            for (int i = 0; i < settings_field_count; ++i) {
                const auto edit = GetDlgItem(window, IDC_SPEED + i);
                SendMessageW(edit, EM_SETLIMITTEXT, 64, 0);
                SendDlgItemMessageW(window, IDC_SPIN_FIRST + i, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edit), 0);
                SendDlgItemMessageW(window, IDC_SPIN_FIRST + i, UDM_SETRANGE32, 0, 10000);
                SendDlgItemMessageW(window, IDC_SPIN_FIRST + i, UDM_SETPOS32, 0, 5000);
            }
            SendDlgItemMessageW(window, IDC_EXCLUSIONS, EM_SETLIMITTEXT, exclusion_text_limit, 0);
            SendDlgItemMessageW(window, IDC_VOICE_MODEL_PATH, EM_SETLIMITTEXT, voice_model_path_limit, 0);
            for (const auto& choice : cursor_styles) {
                SendDlgItemMessageW(window, IDC_CURSOR_STYLE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
            }
            for (const auto label : {L"Smooth", L"Speed bands"}) {
                SendDlgItemMessageW(window, IDC_MOTION_PROFILE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            }
            for (const auto label : {L"Face buttons (A/B/X/Y)", L"One-handed (stick flicks)"}) {
                SendDlgItemMessageW(window, IDC_KEYBOARD_INPUT_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            }
            host->populate_settings();
            if (!host->settings_pages_.initialize(window) || !host->app_ui_.refresh(window)) {
                host->settings_result(L"Settings appearance initialization failed. Close and reopen Settings.");
                return TRUE;
            }
            host->settings_result(host->config_message_ + L"\r\n" + host->cursor_.error());
            return TRUE;
        }
        if (!host) { return FALSE; }
        if (host->caption_message(window, message, wparam, lparam)) { return TRUE; }
        if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT
            || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORBTN) {
            return reinterpret_cast<INT_PTR>(host->app_ui_.control_color(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam)));
        }
        if (message == WM_PAINT) { host->app_ui_.paint(window, true); return TRUE; }
        if (message == WM_THEMECHANGED || message == WM_SETTINGCHANGE || message == WM_SYSCOLORCHANGE) {
            if (!host->app_ui_.refresh(window)) { host->settings_result(host->app_ui_.error()); }
            return TRUE;
        }
        if (message == WM_DPICHANGED) {
            const auto* rect = reinterpret_cast<const RECT*>(lparam);
            if (!host->settings_pages_.fit_window(window, HIWORD(wparam), rect)
                || !host->settings_pages_.dpi_changed(window, HIWORD(wparam))) {
                host->settings_result(L"Settings layout failed. Close and reopen Settings.");
            }
            return TRUE;
        }
        if (message == WM_DRAWITEM && host->app_ui_.draw_item(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) { return TRUE; }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
            DestroyWindow(window);
            return TRUE;
        }
        if (message == WM_NCDESTROY) { host->settings_window_ = nullptr; return FALSE; }
        if (!host->shutdown_.allows_work()) { return FALSE; }
        if (message == WM_DRAWITEM && wparam == IDC_CURSOR_PREVIEW) {
            host->draw_cursor_preview(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wparam) == IDC_MOTION_PROFILE && HIWORD(wparam) == CBN_SELCHANGE) {
            host->update_band_controls();
            return TRUE;
        }
        if (message == WM_COMMAND && ((LOWORD(wparam) == IDC_CURSOR_STYLE && HIWORD(wparam) == CBN_SELCHANGE)
            || (LOWORD(wparam) == IDC_DIAMETER && HIWORD(wparam) == EN_CHANGE))) {
            InvalidateRect(GetDlgItem(window, IDC_CURSOR_PREVIEW), nullptr, TRUE);
            return TRUE;
        }
        if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
            switch (LOWORD(wparam)) {
            case IDC_VOICE_ENABLED: host->update_voice_controls(); return TRUE;
            case IDC_VOICE_BROWSE: host->browse_voice_model(); return TRUE;
            case IDC_RESTORE_CURSOR: host->restore_pointer(); return TRUE;
            case IDC_APPLY: host->apply_settings(); return TRUE;
            case IDC_DEFAULTS:
                host->edit_base_ = {};
                host->populate_settings();
                host->settings_result(L"Defaults in editor only. Apply / save to use them; then center the controller.");
                return TRUE;
            case IDC_RELOAD:
                host->keyboard_.block(L"Settings reloaded. View closes.");
                host->live_.pause(monotonic_now(), L"Reload settings: new neutral and controller input required");
                host->accept(host->core_.pause(monotonic_now()));
                if (!host->pause_cursor()) { host->settings_result(host->cursor_.error()); host->render_live(); return TRUE; }
                host->load_configuration();
                host->edit_base_ = host->settings_;
                host->populate_settings();
                host->settings_result(host->config_message_ + (host->live_.faulted() ? L" Cleanup fault still requires recovery." : L""));
                host->render_live();
                return TRUE;
            }
        }
        if (message == WM_NOTIFY) {
            const auto& notification = *reinterpret_cast<const NMHDR*>(lparam);
            LRESULT drawing = 0;
            if (host->app_ui_.custom_draw(notification, drawing)) {
                SetWindowLongPtrW(window, DWLP_MSGRESULT, drawing); return TRUE;
            }
            if (notification.idFrom == IDC_SETTINGS_TABS && notification.code == TCN_SELCHANGE) {
                host->settings_pages_.select(window, TabCtrl_GetCurSel(GetDlgItem(window, IDC_SETTINGS_TABS)));
                return TRUE;
            }
            const auto* change = reinterpret_cast<NMUPDOWN*>(lparam);
            if (change->hdr.code == UDN_DELTAPOS && change->hdr.idFrom >= IDC_SPIN_FIRST
                && change->hdr.idFrom < IDC_SPIN_FIRST + settings_field_count) {
                const auto index = static_cast<std::size_t>(change->hdr.idFrom - IDC_SPIN_FIRST);
                double value = 0;
                if (!settings_number(host->settings_text(IDC_SPEED + static_cast<int>(index)), value)) {
                    host->settings_result(L"Correct this numeric value before using its arrows.");
                } else {
                    constexpr std::array<double, settings_field_count> minimum{100, 5, 120, 0, 1, 0, 1, 20, 1, 1, 1};
                    constexpr std::array<double, settings_field_count> maximum{5000, 100, 4800, 90, 95, 90, 95, 96, 100, 100, 100};
                    constexpr std::array<double, settings_field_count> step{50, 1, 120, 1, 1, 1, 1, 2, 1, 1, 1};
                    value = std::clamp(value - change->iDelta * step[index], minimum[index], maximum[index]);
                    std::wostringstream text;
                    text << std::setprecision(15) << value;
                    SetDlgItemTextW(window, IDC_SPEED + static_cast<int>(index), text.str().c_str());
                }
                SetWindowLongPtrW(window, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }
        }
        return FALSE;
    }

    void command(int id)
    {
        self_caption_.cancel();
        if (!shutdown_.allows_work()) { return; }
        suspend_navigation();
        if (id == Select || id == Enable || id == Pause || id == ResumeAutomatic || id == RetryCleanup) {
            keyboard_.block(L"Desktop control changed. View closes.");
        }
        switch (id) {
        case Select: {
            const auto index = SendMessageW(child(SlotChoice), CB_GETCURSEL, 0, 0);
            if (index == CB_ERR) {
                add_event(L"No dropdown choice available; select None or a slot.");
                break;
            }
            accept(core_.select_slot(index == 0 ? std::nullopt
                                                : std::optional<unsigned>{static_cast<unsigned>(index - 1)}, monotonic_now()));
            const auto error = live_.prefer_slot(core_.selected_slot(), monotonic_now());
            if (!error.empty()) { add_event(std::wstring{error}); }
            else { add_event(index == 0 ? L"Automatic controller selection." : L"Controller preference set to slot " + std::to_wstring(index - 1) + L"."); }
            update_status();
            break;
        }
        case Enable:
            live_.pause(monotonic_now(), L"M0 simulation owns controller routing until stopped");
            accept(core_.enable(monotonic_now()));
            break;
        case Pause:
            live_.suspend_automatic(monotonic_now());
            accept(core_.pause(monotonic_now()));
            break;
        case ResumeAutomatic:
            if (!inspection_ && desktop_.available() && reader_started_ && hotkey_registered_
                && config_available_ && !cursor_.recovery_pending() && !live_.faulted() && !keyboard_.recovery_required()) {
                accept(core_.pause(monotonic_now()));
                if (live_.request_desktop(monotonic_now()) && !cursor_.enable()) {
                    live_.suspend_automatic(monotonic_now(), L"Cursor recovery required before resuming");
                    cursor_status();
                }
            }
            break;
        case RetryCleanup:
            live_.observe_text_fault(keyboard_.delivery().fault_generation(), monotonic_now());
            keyboard_.recover();
            live_.observe_text_fault(keyboard_.delivery().fault_generation(), monotonic_now());
            live_.recover(monotonic_now());
            break;
        case RestorePointer: restore_pointer(); return;
        case OpenSettings: open_settings(); return;
        case Diagnostics:
            diagnostics_ = !diagnostics_;
            text(Diagnostics, diagnostics_ ? L"Hide &details" : L"&Details");
            if (!diagnostics_) {
                accept(core_.pause(monotonic_now()));
                SetScrollPos(window_, SB_HORZ, 0, TRUE);
                SetScrollPos(window_, SB_VERT, 0, TRUE);
            }
            {
                RECT bounds{0, 0, px(diagnostics_ ? 688 : 488), px(diagnostics_ ? 760 : 324)};
                AdjustWindowRectExForDpi(&bounds, host_style, FALSE, WS_EX_CONTROLPARENT, dpi_);
                MONITORINFO monitor{sizeof(monitor)};
                if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                    SetWindowPos(window_, nullptr, 0, 0,
                        std::min<LONG>(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left),
                        std::min<LONG>(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top),
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            layout();
            break;
        case Exit: close(); return;
        case Show: show(); break;
        case Takeover: ++physical_generation_; add_event(L"SIMULATED physical takeover."); break;
        case ContextSwitch: ++context_generation_; add_event(L"SIMULATED context generation changed."); break;
        case Context: case Session: case Baseline: case Modifier: case MouseButton: break;
        default: return;
        }
        update_cursor_presentation();
        cursor_status();
        refresh();
    }

    void install_tray()
    {
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = window_;
        icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        icon.uCallbackMessage = tray_message;
        icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(icon.szTip, dashboard_title(live_.release_tap()));
        tray_present_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
        if (tray_present_) {
            icon.uVersion = NOTIFYICON_VERSION_4;
            if (!Shell_NotifyIconW(NIM_SETVERSION, &icon)) {
                Shell_NotifyIconW(NIM_DELETE, &icon);
                tray_present_ = false;
            }
        }
        tray_update_failed_ = false;
        add_event(tray_present_ ? L"Tray installed: Show / Pause / Exit."
                               : L"Tray installation FAILED. Window remains available for Pause and Exit.");
        update_status();
    }

    void show()
    {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window_);
    }

    void tray_menu(LPARAM event, WPARAM position)
    {
        if (LOWORD(event) == NIN_SELECT || LOWORD(event) == NIN_KEYSELECT) {
            show();
        } else if (LOWORD(event) == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu();
            if (!menu) {
                add_event(L"Tray menu creation FAILED; opening the diagnostic window.");
                show();
                return;
            }
            const bool built = AppendMenuW(menu, MF_STRING, Show, L"Show Gamepad Desktop")
                && AppendMenuW(menu, MF_STRING, Pause, L"Pause")
                && AppendMenuW(menu, MF_STRING, ResumeAutomatic, L"Desktop control")
                && AppendMenuW(menu, MF_STRING, RestorePointer, L"Restore Windows pointer")
                && AppendMenuW(menu, MF_STRING, Exit, L"Exit");
            if (!built) {
                add_event(L"Tray menu population FAILED; opening the diagnostic window.");
                DestroyMenu(menu);
                show();
                return;
            }
            POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
            if (point.x == -1 && point.y == -1) {
                GetCursorPos(&point);
            }
            SetForegroundWindow(window_);
            const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                point.x, point.y, 0, window_, nullptr);
            DestroyMenu(menu);
            PostMessageW(window_, WM_NULL, 0, 0);
            if (choice) {
                command(static_cast<int>(choice));
            }
        }
    }

    bool close(bool user_requested = true)
    {
        self_caption_.cancel();
        if (!shutdown_.allows_work()) { return false; }
        suspend_navigation();
        live_.observe_text_fault(keyboard_.delivery().fault_generation(), monotonic_now());
        keyboard_.dismiss();
        if (refreshing_ || keyboard_.busy()) {
            close_requested_ = true;
            return false;
        }
        shutdown_.begin();
        keyboard_.recover();
        live_.observe_text_fault(keyboard_.delivery().fault_generation(), monotonic_now());
        live_.suspend_automatic(monotonic_now(), L"Closing; controller input stopped");
        if (!cursor_.shutdown()) {
            cursor_status();
            render_live();
            if (user_requested && cursor_.recovery_pending()) {
                MessageBoxW(window_, (cursor_.error() + L"\r\nNot closing. Use Settings / tray Restore Windows pointer, then retry Exit.").c_str(),
                    L"Gamepad Desktop - pointer recovery required", MB_OK | MB_ICONERROR);
                shutdown_.veto(); // Manual pause remains latched; explicit recovery/retry is usable.
                return false;
            }
            if (user_requested) {
                MessageBoxW(window_, (cursor_.error() + L"\r\nClosing with no native hiding owned; Windows will release process resources.").c_str(),
                    L"Gamepad Desktop - cursor runtime cleanup", MB_OK | MB_ICONWARNING);
            }
            OutputDebugStringW(cursor_.error().c_str());
        }
        if (live_.owned() && user_requested) {
            MessageBoxW(window_,
                L"Owned synthetic input could not be safely released. No new output will be sent.\r\n"
                L"After returning to an ordinary desktop, physically press/release the affected mouse button "
                L"or Escape to repair shared input state. Closing cannot guarantee cleanup across desktop, "
                L"integrity, or concurrent physical holds.",
                L"Gamepad Desktop - unresolved owned input", MB_OK | MB_ICONWARNING);
        }
        if (keyboard_.recovery_required() && user_requested) {
            MessageBoxW(window_,
                L"Typing stopped with unresolved or uncertain accepted input. A partial edit may have reached the app.\r\n"
                L"Do not automatically replay edits. Closing cannot guarantee key cleanup across protected desktops or physical overlap.",
                L"Gamepad Desktop - unresolved text output", MB_OK | MB_ICONWARNING);
        }
        accept(core_.shutdown(monotonic_now()));
        if (!navigation_filter_.shutdown()) {
            add_event(navigation_filter_.status());
            if (user_requested) {
                MessageBoxW(window_, navigation_filter_.status().c_str(),
                    L"Gamepad Desktop - navigation filter shutdown", MB_OK | MB_ICONWARNING);
            }
        }
        shutdown_.finish();
        DestroyWindow(window_);
        return true;
    }

    void destroyed()
    {
        shutdown_.finish();
        navigation_filter_.shutdown();
        live_.observe_text_fault(keyboard_.delivery().fault_generation(), monotonic_now());
        keyboard_.dismiss();
        keyboard_.set_voice(nullptr);
        voice_.shutdown();
        keyboard_.recover();
        live_.observe_text_fault(keyboard_.delivery().fault_generation(), monotonic_now());
        keyboard_popup_.close();
        live_.pause(monotonic_now());
        if (!cursor_.shutdown()) {
            // Forced window/session teardown cannot be vetoed or made crash-safe.
            OutputDebugStringW(cursor_.error().c_str());
        }
        desktop_.shutdown();
        core_.shutdown(monotonic_now());
        poller_.stop();
        KillTimer(window_, timer_id);
        if (hotkey_registered_) {
            UnregisterHotKey(window_, hotkey_id);
            hotkey_registered_ = false;
        }
        if (tray_present_) {
            NOTIFYICONDATAW icon{sizeof(icon)};
            icon.hWnd = window_;
            icon.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &icon);
            tray_present_ = false;
        }
        PostQuitMessage(0);
    }

    LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (caption_message(window_, message, wparam, lparam)) { return 0; }
        if (taskbar_created_ && message == taskbar_created_) {
            install_tray();
            refresh();
            return 0;
        }
        switch (message) {
        case WM_CREATE: return initialize() ? 0 : -1;
        case WM_PAINT: app_ui_.paint(window_, false); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(app_ui_.control_color(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam)));
        case WM_NOTIFY: {
            LRESULT result = 0;
            if (app_ui_.custom_draw(*reinterpret_cast<NMHDR*>(lparam), result)) { return result; }
            break;
        }
        case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:
            if (!app_ui_.refresh(window_)) { add_event(app_ui_.error()); }
            if (settings_window_) { app_ui_.style_controls(settings_window_); InvalidateRect(settings_window_, nullptr, TRUE); }
            return 0;
        case restore_cursor_message: return restore_pointer(false) ? 1 : 2;
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) {
                command(LOWORD(wparam));
            }
            return 0;
        case WM_TIMER:
            if (wparam == timer_id) { refresh(); }
            return 0;
        case keyboard_popup_message: refresh(); return 0;
        case WM_INPUT:
            if (desktop_.raw_input(reinterpret_cast<HRAWINPUT>(lparam))) {
                dismiss_keyboard(L"Physical input took over; queued edits cancelled");
                live_.physical_takeover(monotonic_now());
                pause_cursor();
                render_live();
            }
            return DefWindowProcW(window_, message, wparam, lparam);
        case WM_INPUT_DEVICE_CHANGE:
            dismiss_keyboard(L"Input device changed; queued edits cancelled");
            desktop_.invalidate();
            live_.invalidate(monotonic_now(), L"PAUSED - physical input device changed; fresh baseline required");
            pause_cursor();
            render_live();
            return 0;
        case WM_WTSSESSION_CHANGE:
            dismiss_keyboard(L"Session changed; queued edits cancelled");
            desktop_.session_changed(wparam);
            live_.pause(monotonic_now(), L"Session transition; automatic control waits for fresh input");
            accept(core_.pause(monotonic_now()));
            pause_cursor();
            render_live();
            return 0;
        case live::context_message:
            if (wparam) {
                dismiss_keyboard(L"Unsafe desktop changed; queued edits cancelled");
                live_.invalidate(monotonic_now(), L"PAUSED - unsafe desktop/foreground boundary; fresh neutral required");
                pause_cursor();
            }
            refresh();
            return 0;
        case WM_DISPLAYCHANGE:
            dismiss_keyboard(L"Display changed; queued edits cancelled");
            desktop_.invalidate();
            live_.invalidate(monotonic_now(), L"PAUSED - display geometry changed; fresh neutral required");
            pause_cursor();
            render_live();
            return 0;
        case WM_HOTKEY:
            if (wparam == hotkey_id) {
                add_event(L"Emergency hotkey: explicit pause.");
                command(Pause);
            }
            return 0;
        case tray_message: tray_menu(lparam, wparam); return 0;
        case WM_SIZE: layout(); return 0;
        case WM_HSCROLL: scroll(SB_HORZ, wparam); return 0;
        case WM_VSCROLL: scroll(SB_VERT, wparam); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            RECT bounds{0, 0, px(488), px(324)};
            AdjustWindowRectExForDpi(&bounds, host_style, FALSE, WS_EX_CONTROLPARENT, dpi_);
            info->ptMinTrackSize = {bounds.right - bounds.left, bounds.bottom - bounds.top};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            dpi_font_failed_ = !set_fonts();
            const auto* bounds = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, bounds->left, bounds->top,
                bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_POWERBROADCAST:
            if (wparam == PBT_APMSUSPEND || wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
                dismiss_keyboard(L"Power state changed; queued edits cancelled");
                add_event(L"Host suspend/resume: automatic control waits for eligible context and fresh neutral.");
                live_.pause(monotonic_now(), L"Power transition; center controller before using it");
                accept(core_.pause(monotonic_now()));
                pause_cursor();
            }
            return TRUE;
        case WM_QUERYENDSESSION: command(Pause); pause_cursor(true); return TRUE;
        case WM_ENDSESSION:
            if (wparam) { close(false); }
            return 0;
        case WM_CLOSE: close(); return 0;
        case WM_DESTROY: destroyed(); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        Host* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            host = static_cast<Host*>(create->lpCreateParams);
            host->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        }
        return host ? host->dispatch(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }

    HINSTANCE instance_ = nullptr;
    bool inspection_ = false;
    HWND window_ = nullptr;
    HWND settings_window_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    HFONT mono_font_ = nullptr;
    HFONT title_font_ = nullptr;
    AppUi app_ui_;
    SettingsPages settings_pages_;
    SelfCaption self_caption_;
    std::vector<Child> children_;
    Core core_;
    live::Desktop desktop_;
    live::Live live_{desktop_};
    NativeVoice voice_;
    KeyboardDelivery keyboard_{desktop_};
    KeyboardPopup keyboard_popup_;
    NativeNavigationFilter navigation_filter_;
    HostShutdown shutdown_;
    NavigationForeground navigation_foreground_;
    std::uint64_t navigation_scope_ = 1, navigation_context_ = 0;
    bool navigation_allowed_ = false, navigation_keyboard_ = false;
    bool refreshing_ = false;
    bool close_requested_ = false;
    CursorRing ring_;
    NativeCursorVisibility cursor_visibility_;
    CursorPresentation cursor_{ring_, cursor_visibility_};
    NativeCursorSurface cursor_surface_;
    StartCursorFallback start_cursor_fallback_;
    std::wstring cursor_surface_status_;
    Settings settings_;
    Settings edit_base_;
    SettingsFile settings_file_;
    std::filesystem::path settings_path_;
    std::wstring config_message_;
    std::wstring config_summary_;
    bool config_available_ = false;
    bool diagnostics_ = false;
    bool adding_diagnostics_ = false;
    std::wstring last_cursor_error_;
    std::array<std::optional<Connection>, slot_count> slot_labels_;
    Snapshot snapshot_;
    ControllerPoller poller_;
    Tick started_at_ = monotonic_now();
    Tick rendered_at_ = 0;
    std::wstring last_live_status_;
    std::optional<Status> last_status_;
    std::uint64_t context_generation_ = 1;
    std::uint64_t physical_generation_ = 0;
    DWORD initialization_error_ = 0;
    DWORD hotkey_error_ = 0;
    int reader_error_ = 0;
    bool reader_started_ = false;
    bool hotkey_registered_ = false;
    bool tray_present_ = false;
    bool tray_update_failed_ = false;
    bool event_view_failed_ = false;
    bool dpi_font_failed_ = false;
    UINT taskbar_created_ = 0;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        MessageBoxW(nullptr, L"Could not read command-line arguments. No input or cursor visibility changes were started.",
            L"Gamepad Desktop", MB_OK | MB_ICONERROR);
        return 1;
    }
    const std::array<std::wstring_view, 1> argument{argc == 2 ? argv[1] : L""};
    const auto mode = argc == 1 ? LaunchMode::Desktop : argc == 2 ? launch_mode(argument) : LaunchMode::Invalid;
    const bool restore = mode == LaunchMode::RestoreCursor;
    const bool inspection = mode == LaunchMode::Inspect;
    const bool one_hand_preview = mode == LaunchMode::OneHandedKeyboard;
    const bool keyboard_preview = one_hand_preview || mode == LaunchMode::Keyboard;
    const bool valid_arguments = mode != LaunchMode::Invalid;
    LocalFree(argv);
    if (!valid_arguments) {
        MessageBoxW(nullptr, L"Usage: GamepadDesktop.exe [--release-tap | --inspect | --keyboard-preview | --keyboard-preview-one-handed | --restore-cursor]\r\nRelease-tap keeps the custom cursor and delays short A clicks until release. Word suggestions are configured in Settings > Typing. Previews have no external output.",
            L"Gamepad Desktop", MB_OK | MB_ICONERROR);
        return 2;
    }
    MapperInstanceGuard singleton;
    if (singleton.already_running()) {
        if (keyboard_preview || inspection || launch_uses_release_tap(mode)) {
            MessageBoxW(nullptr,
                L"Exit the running Gamepad Desktop instance before opening a no-output mode or changing click timing.\r\n"
                L"The running instance was not changed. No new input was started.",
                L"Gamepad Desktop - instance already running", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
        // Never wait for the existing process (or a modal second-instance notice) to exit.
        if (const HWND existing = FindWindowW(window_class, nullptr)) {
            if (restore) {
                DWORD_PTR acknowledgement = 0;
                const auto sent = SendMessageTimeoutW(existing, restore_cursor_message, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &acknowledgement);
                if (!sent || acknowledgement != 1) {
                    MessageBoxW(nullptr,
                        L"The running instance did not confirm pointer recovery (busy, older version, or restore failure).\r\n"
                        L"Use its Settings / tray Restore Windows pointer. If unavailable, exit it and retry --restore-cursor.\r\n"
                        L"This recovery process did not independently change global visibility.",
                        L"Gamepad Desktop - recovery not confirmed", MB_OK | MB_ICONERROR);
                    return 1;
                }
            } else if (!PostMessageW(existing, WM_COMMAND, Show, 0)) {
                MessageBoxW(nullptr, L"Could not contact the running instance. Use its Settings / tray Restore Windows pointer.",
                    L"Gamepad Desktop", MB_OK | MB_ICONERROR);
                return 1;
            }
        } else {
            MessageBoxW(nullptr, L"Close the existing instance (or wait for it to finish starting/stopping), then retry.",
                L"Gamepad Desktop", MB_OK | MB_ICONWARNING);
            return 1;
        }
        return 0;
    }
    if (!singleton.acquired()) {
        MessageBoxW(nullptr, L"The instance guard is unavailable. This instance will exit without enabling input.",
                    L"Gamepad Desktop", MB_OK | MB_ICONWARNING);
        return 1;
    }
    int result = 0;
    if (restore) {
        NativeCursorVisibility visibility;
        const bool restored = visibility.initialize() && visibility.show(true);
        auto error = visibility.error();
        const bool released = visibility.shutdown();
        if (!released) { error += L"\r\n" + visibility.error(); }
        MessageBoxW(nullptr, restored && released
            ? L"Windows pointer show request succeeded. No desktop mapping was started and no settings were saved."
            : error.c_str(), L"Gamepad Desktop - pointer recovery", MB_OK | (restored && released ? MB_ICONINFORMATION : MB_ICONERROR));
        result = restored && released ? 0 : 1;
    } else if (keyboard_preview) {
        result = gamepad::run_keyboard_preview(instance, show,
            one_hand_preview ? KeyboardInputMode::OneHanded : KeyboardInputMode::FaceButtons);
    } else {
        Host host(inspection, launch_uses_release_tap(mode));
        result = host.create(instance, show) ? host.run() : 1;
    }
    return result;
}
