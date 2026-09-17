#include "Desktop.h"
#include "XInputReader.h"

#include <WtsApi32.h>
#include <Shellapi.h>
#include <algorithm>
#include <cwchar>
#include <limits>

namespace gamepad::live {
namespace {
bool named(HANDLE object, const wchar_t* name)
{
    wchar_t value[256]{};
    DWORD needed = 0;
    return GetUserObjectInformationW(object, UOI_NAME, value, sizeof(value), &needed)
        && std::wcscmp(value, name) == 0;
}

Rect desktop_rect()
{
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536
        || std::int64_t{x} + width - 1 > (std::numeric_limits<int>::max)()
        || std::int64_t{y} + height - 1 > (std::numeric_limits<int>::max)()) { return {}; }
    return {x, y, static_cast<int>(std::int64_t{x} + width - 1),
                  static_cast<int>(std::int64_t{y} + height - 1)};
}

bool key_event(const Output* intent)
{
    return intent && (intent->kind == OutputKind::EscapeDown || intent->kind == OutputKind::EscapeUp);
}

Rect inclusive(RECT rect)
{
    if (rect.right <= rect.left || rect.bottom <= rect.top) { return {}; }
    return {rect.left, rect.top, rect.right - 1, rect.bottom - 1};
}

AppIdentity app_identity(HWND foreground)
{
    const auto root = GetAncestor(foreground, GA_ROOTOWNER);
    DWORD process = 0, owner_process = 0;
    if (!root || !GetWindowThreadProcessId(foreground, &process)
        || !GetWindowThreadProcessId(root, &owner_process) || process != owner_process) { return {}; }
    return {reinterpret_cast<std::uintptr_t>(root), process};
}

TextWindow text_window(HWND window)
{
    DWORD process = 0;
    const auto thread = window ? GetWindowThreadProcessId(window, &process) : 0;
    return {reinterpret_cast<std::uintptr_t>(window), process, thread};
}

struct ProcessToken {
    DWORD integrity;
    bool ui_access;
};

std::optional<ProcessToken> process_token(HANDLE process, DWORD* failure = nullptr)
{
    if (failure) { *failure = ERROR_SUCCESS; }
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        if (failure) { *failure = GetLastError(); }
        return {};
    }
    std::optional<ProcessToken> result;
    // Mandatory labels are small; fixed aligned storage avoids per-observation allocation.
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[4096]{};
    DWORD needed = 0, ui_access = 1;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &needed)
        && GetTokenInformation(token, TokenUIAccess, &ui_access, sizeof(ui_access), &needed)) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer);
        if (IsValidSid(label->Label.Sid)) {
            const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
            if (count) { result = ProcessToken{*GetSidSubAuthority(label->Label.Sid, count - 1), ui_access != 0}; }
        }
    } else if (failure) { *failure = GetLastError(); }
    if (!result && failure && !*failure) { *failure = ERROR_INVALID_DATA; }
    CloseHandle(token);
    return result;
}
} // namespace

Desktop* Desktop::instance_ = nullptr;

Desktop::~Desktop() { shutdown(); }

bool Desktop::initialize(HWND host, bool emergency_ready)
{
    host_ = host;
    instance_ = this;
    const RAWINPUTDEVICE devices[] = {{1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, host},
                                      {1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, host}};
    raw_registered_ = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
    const auto raw_error = raw_registered_ ? 0 : GetLastError();
    session_registered_ = WTSRegisterSessionNotification(host, NOTIFY_FOR_THIS_SESSION) != FALSE;
    const auto session_error = session_registered_ ? 0 : GetLastError();
    const bool session_known = ProcessIdToSessionId(GetCurrentProcessId(), &session_) != FALSE;
    const auto token = process_token(GetCurrentProcess());
    integrity_ = token ? token->integrity : 0;
    const bool token_allowed = token && process_integrity_allowed(integrity_, integrity_, token->ui_access);
    foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, event_callback, 0, 0, WINEVENT_OUTOFCONTEXT);
    focus_hook_ = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS,
        nullptr, event_callback, 0, 0, WINEVENT_OUTOFCONTEXT);
    ready_ = emergency_ready && raw_registered_ && session_registered_ && session_known
        && foreground_hook_ && token_allowed;
    infrastructure_ = ready_ ? L"Live observers READY: Raw Input sink, WTS, desktop/token checks, WinEvent notifications"
        : L"LIVE UNAVAILABLE: emergency/observer/token initialization failed. Raw error "
            + std::to_wstring(raw_error) + L"; WTS error " + std::to_wstring(session_error)
            + L"; WinEvents " + (foreground_hook_ ? L"ready" : L"FAILED")
            + L". A medium/high-integrity host without UIAccess is required.";
    return ready_;
}

void Desktop::shutdown()
{
    ready_ = false;
    if (foreground_hook_) { UnhookWinEvent(foreground_hook_); foreground_hook_ = nullptr; }
    if (focus_hook_) { UnhookWinEvent(focus_hook_); focus_hook_ = nullptr; }
    if (session_registered_) { WTSUnRegisterSessionNotification(host_); session_registered_ = false; }
    if (raw_registered_) {
        const RAWINPUTDEVICE devices[] = {{1, 2, RIDEV_REMOVE, nullptr}, {1, 6, RIDEV_REMOVE, nullptr}};
        RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
        raw_registered_ = false;
    }
    last_routing_.reset();
    if (instance_ == this) { instance_ = nullptr; }
}

void CALLBACK Desktop::event_callback(HWINEVENTHOOK, DWORD event, HWND window,
                                      LONG object, LONG child, DWORD, DWORD)
{
    if (!instance_) { return; }
    auto& self = *instance_;
    const auto foreground = GetForegroundWindow();
    const auto foreground_identity = text_window(foreground);
    GUITHREADINFO gui{sizeof(gui)};
    if (!foreground_identity.thread || !GetGUIThreadInfo(foreground_identity.thread, &gui)) {
        PostMessageW(self.host_, context_message, 0, 0);
        return;
    }
    const auto focused = text_window(gui.hwndFocus);
    bool changed = self.focus_.observe(foreground_identity, focused);
    if (event == EVENT_OBJECT_FOCUS) {
        const auto source = text_window(window);
        // Sibling fields remain relevant even when focus has already returned by
        // the time the out-of-context notification arrives.
        const bool relevant = source.thread && (window == foreground || window == gui.hwndFocus
            || IsChild(foreground, window) || IsChild(window, gui.hwndFocus) || IsChild(gui.hwndFocus, window));
        changed = self.focus_.notify({source, object, child}, relevant) || changed;
        if (changed) { PostMessageW(self.host_, context_message, 0, 0); }
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND) {
        if (window != foreground) {
            self.focus_.invalidate();
            changed = true;
            if (app_identity(window) != app_identity(foreground)) { self.reset_activation(); }
        }
        if (!changed) { return; }
        self.attention_checked_.reset();
        self.update_activation(app_identity(foreground));
        DWORD process = 0;
        std::wstring basename;
        if (!GetWindowThreadProcessId(foreground, &process)) {
            // An expired queued HWND is not a new authorization identity. Inspect
            // the current route instead of stranding input after a dialog closes.
            PostMessageW(self.host_, context_message, 0, 0);
        } else if (!self.allowed_process(process, self.exclusions().empty() ? nullptr : &basename)
            || executable_excluded(basename, self.exclusions())) {
            self.invalidate();
        } else {
            // Eligible activations (including our own clicks) revise routing, not ownership.
            PostMessageW(self.host_, context_message, 0, 0);
        }
    }
}

void Desktop::invalidate()
{
    attention_checked_.reset();
    game_handoff_.reset();
    reset_activation();
    ++generation_;
    if (host_) { PostMessageW(host_, context_message, 1, 0); }
}

void Desktop::set_automatic_game_switching(bool enabled)
{
    if (automatic_game_switching_ == enabled) { return; }
    automatic_game_switching_ = enabled;
    attention_checked_.reset();
    game_handoff_.reset();
    reset_activation();
    attention_error_.clear();
}

void Desktop::session_changed(WPARAM event)
{
    if (event == WTS_SESSION_LOCK || event == WTS_CONSOLE_DISCONNECT || event == WTS_REMOTE_DISCONNECT
        || event == WTS_SESSION_LOGOFF) { unlocked_ = false; }
    if (event == WTS_SESSION_UNLOCK || event == WTS_CONSOLE_CONNECT || event == WTS_REMOTE_CONNECT) {
        unlocked_ = true;
    }
    invalidate();
}

bool Desktop::normal_desktop() const
{
    if (!unlocked_) { return false; }
    PWTSINFOEXW info = nullptr;
    DWORD bytes = 0;
    const bool queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_,
        WTSSessionInfoEx, reinterpret_cast<LPWSTR*>(&info), &bytes) != FALSE;
    const bool active = queried && info && bytes >= sizeof(WTSINFOEXW) && info->Level == 1
        && info->Data.WTSInfoExLevel1.SessionState == WTSActive
        && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    if (info) { WTSFreeMemory(info); }
    if (!active) { return false; }
    USEROBJECTFLAGS flags{};
    DWORD needed = 0;
    const auto station = GetProcessWindowStation();
    if (!named(station, L"WinSta0")
        || !GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &needed)
        || !(flags.dwFlags & WSF_VISIBLE)
        || !named(GetThreadDesktop(GetCurrentThreadId()), L"Default")) { return false; }
    // GetAsyncKeyState can return zero without this access on another process's
    // foreground thread. Requesting the right is a read-only permission check;
    // no input hook is installed.
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_HOOKCONTROL);
    if (!desktop) { return false; }
    const bool ordinary = named(desktop, L"Default");
    CloseDesktop(desktop);
    return ordinary;
}

bool Desktop::allowed_process(DWORD process, std::wstring* basename)
{
    diagnostic_.process = process;
    diagnostic_.token_known = false;
    diagnostic_.session = diagnostic_.integrity = diagnostic_.error = 0;
    diagnostic_.ui_access = false;
    diagnostic_.query = Query::Session;
    DWORD session = 0;
    if (!ProcessIdToSessionId(process, &session)) { diagnostic_.error = GetLastError(); return false; }
    diagnostic_.session = session;
    if (session != session_) { return false; }
    diagnostic_.query = Query::ProcessOpen;
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) { diagnostic_.error = GetLastError(); return false; }
    diagnostic_.query = Query::Token;
    const auto token = process_token(handle, &diagnostic_.error);
    if (token) {
        diagnostic_.token_known = true;
        diagnostic_.integrity = token->integrity;
        diagnostic_.ui_access = token->ui_access;
    }
    bool allowed = token && process_integrity_allowed(integrity_, token->integrity, token->ui_access);
    if (allowed && basename) {
        diagnostic_.query = Query::Image;
        wchar_t path[32768]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        allowed = QueryFullProcessImageNameW(handle, 0, path, &length) != FALSE;
        if (!allowed) { diagnostic_.error = GetLastError(); }
        if (allowed) {
            const std::wstring_view image{path, length};
            const auto separator = image.find_last_of(L"\\/");
            *basename = image.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
            allowed = !basename->empty();
        }
    }
    CloseHandle(handle);
    if (allowed) { diagnostic_.query = Query::None; diagnostic_.error = 0; }
    return allowed;
}

bool Desktop::raw_input(HRAWINPUT handle)
{
    RAWINPUT input{};
    UINT size = sizeof(input);
    const UINT received = GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
    if (received == static_cast<UINT>(-1) || received < sizeof(RAWINPUTHEADER)) {
        ready_ = false;
        infrastructure_ = L"LIVE UNAVAILABLE: Raw Input read failed; restart after resolving observer failure";
        ++activity_;
        return true;
    }
    if (input.header.dwType == RIM_TYPEMOUSE) {
        const auto& mouse = input.data.mouse;
        if (!input.header.hDevice && mouse.ulExtraInformation == output_tag) { return false; }
        if (!mouse_report_activity((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0,
                                   mouse.lLastX, mouse.lLastY, mouse.usButtonFlags)) { return false; }
        struct Button { USHORT down; USHORT up; unsigned bit; };
        constexpr Button buttons[] = {
            {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, Primary},
            {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, Secondary},
            {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 16},
            {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 32},
            {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 64}
        };
        for (const auto& button : buttons) {
            if (mouse.usButtonFlags & button.down) { physical_mouse_ |= button.bit; }
            if (mouse.usButtonFlags & button.up) { physical_mouse_ &= ~button.bit; }
        }
    } else if (input.header.dwType == RIM_TYPEKEYBOARD) {
        const auto& keyboard = input.data.keyboard;
        if (!raw_keyboard_activity(input.header.hDevice != nullptr, keyboard.VKey, keyboard.ExtraInformation)) { return false; }
        if (keyboard.VKey == VK_ESCAPE) { physical_escape_ = (keyboard.Flags & RI_KEY_BREAK) == 0; }
        if (keyboard.VKey < physical_keys_.size()) {
            physical_keys_[keyboard.VKey] = (keyboard.Flags & RI_KEY_BREAK) == 0;
        }
    } else {
        return false;
    }
    ++activity_;
    return true;
}

unsigned Desktop::physical_state(unsigned owned, std::optional<TextKey> text_owned)
{
    unsigned asynchronous = 0;
    input_settling_ = false;
    diagnostic_.physical = InputCategory::None;
    diagnostic_.settling = false;
    for (int key = 1; key < 256; ++key) {
        const bool down = physical_async_key(static_cast<unsigned>(key)) && (GetAsyncKeyState(key) & 0x8000) != 0;
        if (text_owned && key == text_virtual_key(*text_owned)) {
            if (text_key_conflict(static_cast<unsigned>(key), down, physical_keys_[key], text_owned)) {
                asynchronous |= Other;
                diagnostic_.physical = InputCategory::Owned;
                diagnostic_.physical_async = down;
                diagnostic_.physical_raw = physical_keys_[key];
            }
            continue;
        }
        unsigned bit = Other;
        if (key == VK_LBUTTON) { bit = Primary; }
        else if (key == VK_RBUTTON) { bit = Secondary; }
        else if (key == VK_ESCAPE) { bit = Escape; }
        const bool raw = bit == Primary || bit == Secondary ? (physical_mouse_ & bit) != 0
            : bit == Escape ? physical_escape_ : physical_keys_[key];
        const auto state = released_keys_.observe(static_cast<unsigned>(key), down, raw, now());
        if (!(owned & bit) && state == KeyState::Settling) {
            input_settling_ = diagnostic_.settling = true;
            diagnostic_.physical = InputCategory::Owned;
            diagnostic_.physical_async = true;
            diagnostic_.physical_raw = false;
            continue;
        }
        if (down || raw) {
            asynchronous |= bit;
            diagnostic_.physical = input_category(static_cast<unsigned>(key), (owned & bit) != 0);
            diagnostic_.physical_async = down;
            diagnostic_.physical_raw = raw;
        }
    }
    const unsigned overlap = (physical_mouse_ & (Primary | Secondary))
        | (physical_escape_ ? Escape : 0);
    return physical_holds(asynchronous, overlap, owned);
}

bool Desktop::read_routing(const Output* intent, Routing& routing, bool text)
{
    routing.desktop = desktop_rect();
    POINT cursor{};
    if (!GetCursorPos(&cursor)) { diagnostic_.query = Query::Cursor; diagnostic_.error = GetLastError(); return false; }
    routing.cursor = {cursor.x, cursor.y};
    const HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD thread = GetWindowThreadProcessId(foreground, &process);
    GUITHREADINFO gui{sizeof(gui)};
    if (!thread || !GetGUIThreadInfo(thread, &gui)) {
        diagnostic_.query = thread ? Query::GuiThread : Query::ForegroundThread;
        diagnostic_.error = GetLastError();
        diagnostic_.role = 0;
        diagnostic_.window = foreground;
        diagnostic_.process = process;
        diagnostic_.thread = thread;
        diagnostic_.token_known = false;
        return false;
    }
    routing.app = app_identity(foreground);
    const Point point = intent && intent->kind == OutputKind::Move ? intent->point : routing.cursor;
    // Let Windows do ordinary non-client/menu hit testing. No synchronous messages
    // to other apps, client-area confinement, or one-menu-window transaction.
    // WS_EX_LAYERED | WS_EX_TRANSPARENT overlays are skipped by Windows hit testing.
    // While Explorer rubber-band selection or another drag owns capture, it is
    // the mouse recipient, not a transient/disabled window underneath the cursor.
    const HWND hit = key_event(intent) ? nullptr
        : gui.hwndCapture ? gui.hwndCapture : WindowFromPoint({point.x, point.y});
    if (!key_event(intent) && !hit) {
        diagnostic_.query = Query::WindowIdentity;
        diagnostic_.role = 5;
        diagnostic_.window = nullptr;
        diagnostic_.token_known = false;
        return false;
    }
    const HWND windows[] = {foreground, gui.hwndActive, gui.hwndFocus,
        key_event(intent) && !text ? nullptr : gui.hwndCapture, gui.hwndMenuOwner,
        hit, hit ? GetAncestor(hit, GA_ROOT) : nullptr};
    routing.flags = routing_flags(gui.flags);
    for (std::size_t i = 0; i < routing.windows.size(); ++i) {
        auto& state = routing.windows[i];
        state.window = windows[i];
        if (!state.window) { continue; }
        SetLastError(ERROR_SUCCESS);
        state.thread = GetWindowThreadProcessId(state.window, &state.process);
        if (!state.thread) {
            diagnostic_.query = Query::WindowIdentity;
            diagnostic_.role = static_cast<int>(i);
            diagnostic_.window = state.window;
            diagnostic_.error = GetLastError();
            return false;
        }
        state.visible = IsWindowVisible(state.window) && !IsIconic(state.window);
        state.enabled = IsWindowEnabled(state.window) != FALSE;
    }
    return true;
}

std::wstring_view Desktop::attention_pause_reason(HWND foreground)
{
    auto_reason_ = AutoReason::None;
    if (!automatic_game_switching_) { return {}; }
    const auto current = monotonic_now();
    if (!attention_checked_ || current < *attention_checked_ || current - *attention_checked_ >= 100
        || foreground != attention_foreground_) {
        QUERY_USER_NOTIFICATION_STATE state{};
        const auto result = SHQueryUserNotificationState(&state);
        attention_checked_ = current;
        attention_foreground_ = foreground;
        attention_valid_ = SUCCEEDED(result);
        if (attention_valid_) {
            attention_state_ = static_cast<NotificationState>(state);
            attention_error_.clear();
            RECT window{}, clip{};
            MONITORINFO monitor{sizeof(monitor)};
            DWORD process = 0, shell = 0;
            GetWindowThreadProcessId(GetShellWindow(), &shell);
            const bool known = GetWindowThreadProcessId(foreground, &process) && GetWindowRect(foreground, &window)
                && GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST), &monitor)
                && GetClipCursor(&clip);
            diagnostic_.shell_process = shell;
            diagnostic_.shell_candidate = shell && process == shell;
            attention_shell_process_ = shell;
            attention_process_ = process;
            fullscreen_capture_ = known && confinement_candidate(process, shell, GetCurrentProcessId())
                && fullscreen_pointer_confined(
                inclusive(window), inclusive(monitor.rcMonitor), inclusive(clip), desktop_rect());
            if (!known) {
                attention_error_ = L"Auto pointer-confinement observation unavailable; using Windows notification state.";
            }
        } else {
            attention_error_ = L"Automatic waiting - Windows fullscreen-state query failed (HRESULT "
                + std::to_wstring(result) + L")";
        }
    }
    diagnostic_.shell_process = attention_shell_process_;
    diagnostic_.shell_candidate = attention_shell_process_ && attention_shell_process_ == attention_process_;
    if (!attention_valid_) {
        auto_reason_ = AutoReason::Unavailable;
        return L"Automatic waiting - Windows fullscreen-state query failed; see Diagnostics";
    }
    const auto root = app_identity(foreground).root_owner;
    const auto reason = game_handoff_.update(true, root ? root : reinterpret_cast<std::uintptr_t>(foreground),
        attention_state_, fullscreen_capture_);
    if (attention_state_ == NotificationState::ExclusiveD3D) { auto_reason_ = AutoReason::ExclusiveD3D; }
    else if (attention_state_ == NotificationState::Presentation) { auto_reason_ = AutoReason::Presentation; }
    else if (attention_state_ == NotificationState::NotPresent) { auto_reason_ = AutoReason::NotPresent; }
    else if (!notification_pause_reason(attention_state_).empty()) { auto_reason_ = AutoReason::Unavailable; }
    else if (!reason.empty()) { auto_reason_ = AutoReason::Confinement; }
    return reason;
}

AppActivation Desktop::update_activation(AppIdentity identity)
{
    const auto activation = observe_activation(identity);
    if (activation.generation != activation_generation_) { game_handoff_.reset(); }
    activation_generation_ = activation.generation;
    game_handoff_.foreground_changed(identity.root_owner);
    return activation;
}

Observation Desktop::observe(unsigned owned, const Output* intent)
{
    return observe_route(owned, intent, {}, nullptr);
}

Observation Desktop::observe_route(unsigned owned, const Output* intent, std::optional<TextKey> text_owned, Routing* text_route)
{
    Observation result;
    diagnostic_ = {};
    result.block = BlockReason::Infrastructure;
    result.available = ready_;
    result.context = generation_;
    result.activity = activity_;
    result.desktop = desktop_rect();
    result.bounds = result.desktop;
    const auto finish = [&]() {
        diagnostic_.block = result.block;
        diagnostic_.app = result.app;
        if (!result.allowed && result.routing_stable) {
            if (!unsafe_) { ++generation_; }
            unsafe_ = true;
        } else if (result.allowed) {
            unsafe_ = false;
        }
        result.context = generation_;
        result.activity = activity_;
        result.available = ready_;
        return result;
    };
    if (!ready_) { return finish(); }
    result.reason = L"PAUSED - normal unlocked input desktop/session cannot be established";
    result.block = BlockReason::DesktopSession;
    if (!normal_desktop()) { reset_activation(); return finish(); }
    Routing route;
    result.reason = L"PAUSED - foreground or mouse/key recipient cannot be inspected";
    result.block = BlockReason::Routing;
    if (!read_routing(intent, route, text_route != nullptr)) {
        result.routing_pending = diagnostic_.query != Query::Cursor && routing_query_disappeared(
            diagnostic_.error, !diagnostic_.window || !IsWindow(diagnostic_.window));
        result.routing_stable = !result.routing_pending;
        reset_activation();
        return finish();
    }
    result.app = update_activation(route.app);
    result.desktop = result.bounds = route.desktop;
    result.cursor = route.cursor;
    bool excluded = false;
    Diagnostic excluded_diagnostic;
    // Focus/capture/menu-owner HWNDs can legitimately be hidden helpers. Their
    // processes still need eligibility, but only the foreground/point surfaces
    // need to be visible and enabled.
    const auto usable_surface = [](const WindowState& window) {
        return !window.window || (window.visible && window.enabled);
    };
    const bool usable = usable_surface(route.windows[0])
        && (route.windows[3].window || usable_surface(route.windows[5]));
    std::array<DWORD, 7> checked{};
    std::size_t count = 0;
    for (std::size_t role = 0; role < route.windows.size(); ++role) {
        const auto& window = route.windows[role];
        if (!window.window) { continue; }
        if (std::find(checked.begin(), checked.begin() + count, window.process) != checked.begin() + count) { continue; }
        std::wstring basename;
        result.reason = L"PAUSED - foreground/recipient session, integrity, or UIAccess unsupported";
        result.block = BlockReason::Recipient;
        diagnostic_.role = static_cast<int>(role);
        diagnostic_.window = window.window;
        diagnostic_.thread = window.thread;
        if (!allowed_process(window.process, exclusions().empty() ? nullptr : &basename)) { return finish(); }
        checked[count++] = window.process;
        if (executable_excluded(basename, exclusions()) && !excluded) {
            excluded = true;
            excluded_diagnostic = diagnostic_;
        }
    }
    const auto attention = attention_pause_reason(route.windows[0].window);
    result.automatic = auto_reason_;
    // Re-establish the input desktop and physical state after potentially blocking
    // process/session queries, then take a non-message-sending routing snapshot last.
    result.reason = L"PAUSED - input desktop/session changed during recipient validation";
    result.block = BlockReason::DesktopSession;
    if (!normal_desktop()) { reset_activation(); return finish(); }
    result.physical_held = physical_state(owned, text_owned);
    result.baseline_known = true;
    Routing final;
    result.block = BlockReason::Routing;
    if (!read_routing(intent, final, text_route != nullptr)) {
        // A disappearing helper is not permission to send. Keep already-known
        // safety failures hard, and defer only the unobserved routing itself.
        result.routing_pending = diagnostic_.query != Query::Cursor && routing_query_disappeared(
            diagnostic_.error, !diagnostic_.window || !IsWindow(diagnostic_.window))
            && !excluded && usable && attention.empty() && !result.physical_held;
        result.routing_stable = !result.routing_pending;
        reset_activation();
        return finish();
    }
    result.app = update_activation(final.app);
    focus_.observe(text_window(final.windows[0].window), text_window(final.windows[2].window));
    if (text_route) { *text_route = final; }
    result.cleanup_allowed = true; // Exclusions never prohibit already-owned releases.
    result.routing_stable = route == final && (!input_settling_ || text_route);
    if (!last_routing_ || *last_routing_ != final) {
        ++routing_revision_;
        last_routing_ = final;
    }
    result.routing = routing_revision_;
    result.mode = (route.flags & GUI_INMENUMODE) ? Mode::Menu : Mode::Pointer;
    result.reason = L"PAUSED - executable exclusion matches foreground or actual recipient";
    result.block = BlockReason::Exclusion;
    if (excluded) {
        diagnostic_.role = excluded_diagnostic.role;
        diagnostic_.window = excluded_diagnostic.window;
        diagnostic_.process = excluded_diagnostic.process;
        diagnostic_.thread = excluded_diagnostic.thread;
        diagnostic_.session = excluded_diagnostic.session;
        diagnostic_.integrity = excluded_diagnostic.integrity;
        diagnostic_.token_known = excluded_diagnostic.token_known;
        diagnostic_.ui_access = excluded_diagnostic.ui_access;
        return finish();
    }
    result.reason = L"PAUSED - recipient is hidden, minimized, or disabled";
    result.block = BlockReason::Surface;
    if (!usable) {
        const auto role = !usable_surface(route.windows[0]) ? 0 : 5;
        const auto& window = route.windows[role];
        if (window.process != diagnostic_.process) { diagnostic_.token_known = false; }
        diagnostic_.role = role;
        diagnostic_.window = window.window;
        diagnostic_.process = window.process;
        diagnostic_.thread = window.thread;
        return finish();
    }
    result.reason = L"PAUSED - virtual desktop geometry/cursor unavailable or unsupported";
    result.block = BlockReason::Geometry;
    if (!valid(result.desktop) || !contains(result.desktop, result.cursor)) { return finish(); }
    result.reason = L"PAUSED - swapped mouse buttons unsupported in this first trial";
    result.block = BlockReason::SwappedButtons;
    if (GetSystemMetrics(SM_SWAPBUTTON)) { return finish(); }
    if (!attention.empty()) {
        result.block = heuristic_auto(result.automatic) ? BlockReason::Auto
            : result.automatic == AutoReason::NotPresent ? BlockReason::AutoNotPresent : BlockReason::AutoUnavailable;
        result.reason = attention;
        apply_auto_policy(result);
        return finish();
    }
    result.allowed = true;
    result.block = result.physical_held ? BlockReason::Physical : input_settling_ ? BlockReason::Settling : BlockReason::None;
    apply_auto_policy(result);
    result.reason = result.routing_stable ? L"Desktop eligible - neutral alone does not claim controller use"
                                          : L"LIVE - routing changing; event deferred";
    return finish();
}

unsigned Desktop::send(const Output& output, Rect desktop)
{
    INPUT input{};
    if (output.kind == OutputKind::EscapeDown || output.kind == OutputKind::EscapeUp) {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_ESCAPE;
        input.ki.dwFlags = output.kind == OutputKind::EscapeUp ? KEYEVENTF_KEYUP : 0;
        input.ki.dwExtraInfo = output_tag;
    } else {
        input.type = INPUT_MOUSE;
        input.mi.dwExtraInfo = output_tag;
        switch (output.kind) {
        case OutputKind::Move: {
            const auto normalized = absolute(output.point, desktop);
            input.mi.dx = normalized.x;
            input.mi.dy = normalized.y;
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            break;
        }
        case OutputKind::Wheel: input.mi.dwFlags = MOUSEEVENTF_WHEEL; input.mi.mouseData = static_cast<DWORD>(output.wheel); break;
        case OutputKind::PrimaryDown: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
        case OutputKind::PrimaryUp: input.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
        case OutputKind::SecondaryDown: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
        case OutputKind::SecondaryUp: input.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
        default: return 0;
        }
    }
    const auto accepted = SendInput(1, &input, sizeof(input));
    if (accepted == 1) {
        if (output.kind == OutputKind::PrimaryUp) { released_keys_.accepted_up(VK_LBUTTON, now()); }
        if (output.kind == OutputKind::SecondaryUp) { released_keys_.accepted_up(VK_RBUTTON, now()); }
        if (output.kind == OutputKind::EscapeUp) { released_keys_.accepted_up(VK_ESCAPE, now()); }
    }
    return accepted;
}

TextObservation Desktop::observe_text(std::optional<TextKey> owned)
{
    TextObservation result;
    if (!focus_hook_) { result.reason = L"Keyboard unavailable: focus observer registration failed"; return result; }
    const auto revision = focus_.revision();
    const Output intent{OutputKind::EscapeDown};
    Routing route;
    const auto observation = observe_route(0, &intent, owned, &route);
    for (std::size_t i = 0; i < result.target.windows.size(); ++i) {
        const auto& window = route.windows[i];
        result.target.windows[i] = {reinterpret_cast<std::uintptr_t>(window.window), window.process, window.thread};
    }
    result.target.context = observation.context;
    result.target.activity = observation.activity;
    result.target.focus_revision = focus_.revision();
    // Keep observe_route's ordinary Auto/unsafe generation unchanged. Only this
    // deliberate text session may use an otherwise fully validated heuristic block.
    result.allowed = observation.available && keyboard_route_allowed(observation, native_keyboard_session()) && observation.baseline_known
        && observation.mode == Mode::Pointer && !route.windows[3].window && !route.windows[4].window
        && !(route.flags & (GUI_INMOVESIZE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE));
    result.cleanup_allowed = observation.available && observation.cleanup_allowed && observation.baseline_known;
    result.physical_held = observation.physical_held != 0;
    result.stable = observation.routing_stable && revision == focus_.revision();
    result.settling = input_settling_;
    result.reason = !text_target_valid(result.target) ? L"Keyboard not opened: native focus identity is unavailable"
        : observation.mode == Mode::Menu || route.windows[3].window || route.windows[4].window
            ? L"Keyboard not opened: close menus and release capture first"
        : observation.reason;
    return result;
}

unsigned Desktop::send_text(TextEvent event)
{
    auto input = keyboard_input(event);
    const auto accepted = SendInput(1, &input, sizeof(input));
    if (accepted == 1 && event.up) { released_keys_.accepted_up(text_virtual_key(event.key), now()); }
    return accepted;
}

Tick Desktop::now() const { return monotonic_now(); }

std::wstring Desktop::diagnostic_summary() const
{
    constexpr std::wstring_view roles[]{L"foreground", L"active", L"focus", L"capture", L"menu", L"hit", L"hit-root"};
    constexpr std::wstring_view queries[]{L"none", L"session", L"process-open", L"token", L"image",
        L"foreground-thread", L"GUI-thread", L"window-identity", L"cursor"};
    wchar_t name[64]{};
    if (diagnostic_.window) { GetClassNameW(diagnostic_.window, name, static_cast<int>(std::size(name))); }
    return std::wstring{block_reason_text(diagnostic_.block)} + L"; role="
        + std::wstring{diagnostic_.role >= 0 && diagnostic_.role < 7 ? roles[diagnostic_.role] : L"none"}
        + L"; HWND=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(diagnostic_.window))
        + L"; class=" + name + L"; PID=" + std::to_wstring(diagnostic_.process)
        + L"; TID=" + std::to_wstring(diagnostic_.thread) + L"; session=" + std::to_wstring(diagnostic_.session)
        + L"; integrity=" + std::to_wstring(diagnostic_.integrity)
        + L"; token-known=" + std::to_wstring(diagnostic_.token_known) + L"; UIAccess=" + std::to_wstring(diagnostic_.ui_access)
        + L"; query=" + std::wstring{queries[static_cast<unsigned>(diagnostic_.query)]}
        + L"; error=" + std::to_wstring(diagnostic_.error)
        + L"; " + input_diagnostic_text(diagnostic_.physical, diagnostic_.physical_raw,
            diagnostic_.physical_async, diagnostic_.settling)
        + L"; notification=" + std::to_wstring(static_cast<unsigned>(attention_state_))
        + L"; confined=" + std::to_wstring(fullscreen_capture_) + L"; shell-PID=" + std::to_wstring(diagnostic_.shell_process)
        + L"; shell-window-process=" + std::to_wstring(diagnostic_.shell_candidate)
        + L"; app-root=" + std::to_wstring(diagnostic_.app.identity.root_owner)
        + L"; app-PID=" + std::to_wstring(diagnostic_.app.identity.process)
        + L"; activation=" + std::to_wstring(diagnostic_.app.generation)
        + L"; app-override=" + std::to_wstring(auto_override_active());
}

} // namespace gamepad::live
