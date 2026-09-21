// Explicit opt-in owned-window test. Never registered with CTest or run by a build.
#include "Desktop.h"
#include "KeyboardPopup.h"
#include "XInputReader.h"
#include "Compatibility.h"

#include <algorithm>
#include <iostream>
#include <span>
#include <string>

namespace {
using namespace overworld;
constexpr wchar_t class_name[] = L"OverworldOwnedTextSmoke";

class OwnedBoundary final : public TextBoundary {
public:
    live::Desktop desktop;
    HWND window = nullptr;
    HWND edit = nullptr;
    std::size_t accepted_events = 0;
    TextObservation observe_text(std::optional<TextKey> owned) override
    {
        auto observation = desktop.observe_text(owned);
        if (observation.target.windows[0].window != reinterpret_cast<std::uintptr_t>(window)
            || observation.target.windows[2].window != reinterpret_cast<std::uintptr_t>(edit)) {
            observation.allowed = observation.cleanup_allowed = false;
        }
        for (const auto& target : observation.target.windows) {
            if (target.process != GetCurrentProcessId()) { observation.allowed = observation.cleanup_allowed = false; }
        }
        return observation;
    }
    Tick text_now() const override { return monotonic_now(); }
    unsigned send_text(TextEvent event) override
    {
        // Independent guard forbids even cleanup from touching a foreign or different field.
        if (!text_eligible(observe_text(event.up ? std::optional<TextKey>{event.key} : std::nullopt))) { return 0; }
        const auto accepted = desktop.send_text(event);
        accepted_events += accepted;
        return accepted;
    }
};

struct Action {
    std::uint16_t buttons;
    std::int16_t aim;
    std::uint8_t shift;
    std::wstring_view expected;
    bool change_focus = false;
    std::int16_t aim_x = 0;
};

constexpr Action actions[] = {
    {0, 0, 0, L"seed"}, {0, 0, 0, L"seed"}, {0x8000, 0, 0, L"seed"},
    {0, 28000, 0, L"seed"},
    {0x0100, 28000, 0, L"see"}, {0, 28000, 0, L"see"},
    {0x1000, 28000, 0, L"seea"}, {0, 28000, 0, L"seea"},
    {0x0200, 28000, 0, L"seea "}, {0, 28000, 0, L"seea "},
    {0x2000, 28000, 0, L"seea b"}, {0, 28000, 0, L"seea b"},
    {0x0010, 28000, 0, L"seea b"}, {0, 28000, 0, L"seea b\r\n"},
    {0x1000, 28000, 255, L"seea b\r\nA"}, {0, 28000, 0, L"seea b\r\nA"},
    {0x0020, 28000, 0, L"seea b\r\nA"},
    {0, 0, 0, L"seea b\r\nA"}, {0, 0, 0, L"seea b\r\nA"},
    {0x8000, 0, 0, L"seea b\r\nA"}, {0, 28000, 0, L"seea b\r\nA"},
    {0x1000, 28000, 0, L"seea b\r\nA", true},
    {0, 28000, 0, L"seea b\r\nA"}, {0x0020, 28000, 0, L"seea b\r\nA"}
};

constexpr Action one_handed_actions[] = {
    {0, 0, 0, L"seed"}, {0, 0, 0, L"seed"}, {0x0040, 0, 0, L"seed"}, {0, 0, 0, L"seed"},
    // Action group, then right: erase preexisting text without face buttons.
    {0, 28000, 0, L"seed", false, -28000}, {0, 0, 0, L"seed"},
    {0, 0, 0, L"seed", false, 28000}, {0, 0, 0, L"see"},
    // Up group, then down: a.
    {0, 28000, 0, L"see"}, {0, 0, 0, L"see"},
    {0, -28000, 0, L"see"}, {0, 0, 0, L"seea"},
    // Action group, then down: space.
    {0, 28000, 0, L"seea", false, -28000}, {0, 0, 0, L"seea"},
    {0, -28000, 0, L"seea"}, {0, 0, 0, L"seea "},
    // Action group, then left: cycle to capitals without inserting the icon.
    {0, 28000, 0, L"seea ", false, -28000}, {0, 0, 0, L"seea "},
    {0, 0, 0, L"seea ", false, -28000}, {0, 0, 0, L"seea "},
    {0, 28000, 0, L"seea "}, {0, 0, 0, L"seea "},
    {0, -28000, 0, L"seea "}, {0, 0, 0, L"seea A"},
    // Action group, then up: Enter.
    {0, 28000, 0, L"seea A", false, -28000}, {0, 0, 0, L"seea A"},
    {0, 28000, 0, L"seea A"}, {0, 0, 0, L"seea A\r\n"},
    {0x0020, 0, 0, L"seea A\r\n"}, {0, 0, 0, L"seea A\r\n"}, {0, 0, 0, L"seea A\r\n"},
    {0x0040, 0, 0, L"seea A\r\n"}, {0, 0, 0, L"seea A\r\n"},
    {0, 28000, 0, L"seea A\r\n"}, {0, 0, 0, L"seea A\r\n"},
    {0, 0, 0, L"seea A\r\n", true, 28000}, {0, 0, 0, L"seea A\r\n"},
    {0x0020, 0, 0, L"seea A\r\n"}
};

struct Smoke {
    OwnedBoundary boundary;
    KeyboardDelivery keyboard{boundary};
    KeyboardPopup popup;
    Snapshot snapshot;
    HWND other = nullptr;
    HWND background = nullptr;
    Tick deadline = 0;
    std::size_t action = 0;
    std::size_t verified = 0;
    std::size_t ticks = 0;
    std::size_t accepted_before_focus_change = 0;
    bool awaiting_receipt = false;
    bool focus_changed = false;
    bool physical_abort = false;
    bool started = false;
    std::optional<Tick> ready_since;
    Tick maximum_dispatch_ms = 0;
    int result = 1;
    std::span<const Action> script{actions};
    bool one_handed = false;

    void finish(bool passed, std::wstring_view stage)
    {
        const auto original_keyboard_status = keyboard.status();
        const auto original_delivery_status = keyboard.delivery().status();
        const auto original_popup_error = popup.error();
        const bool foreground_matches = GetForegroundWindow() == boundary.window;
        const bool focus_matches = GetFocus() == (focus_changed ? other : boundary.edit);
        const bool timed_out = deadline != 0 && monotonic_now() > deadline;
        if (!passed || keyboard.recovery_required()) {
            const auto observation = boundary.observe_text(keyboard.delivery().owned_key());
            std::wcerr << L"Owned smoke failure: stage=" << stage
                << L" tick=" << ticks << L" action=" << action << L" snapshot_sequence=" << snapshot.sequence
                << L" awaiting_receipt=" << awaiting_receipt << L" physical_abort=" << physical_abort
                << L" started=" << started
                << L" foreground_matches=" << foreground_matches << L" focus_matches=" << focus_matches
                << L" timeout=" << timed_out << L" deliberate_focus_change=" << focus_changed
                << L"\nBoundary: available=" << boundary.desktop.available()
                << L" eligible=" << text_eligible(observation) << L" allowed=" << observation.allowed
                << L" cleanup_allowed=" << observation.cleanup_allowed << L" stable=" << observation.stable
                << L" physical_held=" << observation.physical_held
                << L" target_identity_valid=" << text_target_valid(observation.target)
                << L"\nBoundary reason: " << observation.reason
                << L"\nInfrastructure: " << boundary.desktop.infrastructure()
                << L"\nOriginal keyboard status: " << original_keyboard_status
                << L"\nOriginal delivery status: " << original_delivery_status
                << L"\nPopup error: " << original_popup_error << L'\n';
        }
        KillTimer(boundary.window, 1);
        keyboard.dismiss();
        keyboard.recover();
        popup.close();
        result = passed && !keyboard.recovery_required() ? 0 : 1;
        std::cerr << "Owned smoke: operation_checks=" << verified
            << " scenario_accepted_events=" << boundary.accepted_events
            << " accepted_events=" << keyboard.delivery().accepted_events()
            << " completed_edits=" << keyboard.delivery().completed_edits()
            << " pending_up=" << keyboard.delivery().owned()
            << " uncertain=" << keyboard.delivery().uncertain()
            << " max_sample_to_dispatch_ms=" << maximum_dispatch_ms << '\n';
        if (result != 0) { std::wcerr << L"After cleanup: " << keyboard.delivery().status() << L'\n'; }
        boundary.desktop.shutdown();
        if (background) { DestroyWindow(background); }
        DestroyWindow(boundary.window);
        PostQuitMessage(result);
    }

    bool own_focus() const
    {
        return GetForegroundWindow() == boundary.window && GetFocus() == (focus_changed ? other : boundary.edit);
    }

    void tick()
    {
        ++ticks;
        if (!started) {
            if (monotonic_now() > deadline) { finish(false, L"await_owned_foreground"); return; }
            // A background launch may not activate immediately. Never send until the
            // user/Windows has focused this owned field and released physical inputs.
            if (!own_focus() || !text_eligible(boundary.observe_text(std::nullopt))) {
                ready_since.reset();
                return;
            }
            const auto now = monotonic_now();
            if (!ready_since) { ready_since = now; }
            if (now - *ready_since < 500) { return; }
            SendMessageW(boundary.edit, EM_SETSEL, 4, 4);
            started = true;
        }
        if (physical_abort || monotonic_now() > deadline || !own_focus()) { finish(false, L"tick_guard"); return; }
        if (awaiting_receipt) {
            wchar_t actual[128]{}, second[64]{};
            GetWindowTextW(boundary.edit, actual, static_cast<int>(std::size(actual)));
            GetWindowTextW(other, second, static_cast<int>(std::size(second)));
            if (script[action].expected != actual) { finish(false, L"original_field_receipt"); return; }
            if (second[0] != 0) { finish(false, L"unexpected_second_field_text"); return; }
            if (keyboard.recovery_required()) { finish(false, L"receipt_pending_key_recovery"); return; }
            if (focus_changed && (keyboard.accepts_typing()
                || keyboard.delivery().accepted_events() != accepted_before_focus_change)) { finish(false, L"changed_focus_rejection"); return; }
            ++verified;
            if (++action == script.size()) { finish(!keyboard.visible(), L"final_dismissal"); return; }
        }
        const auto& next = script[action];
        if (next.change_focus) {
            accepted_before_focus_change = keyboard.delivery().accepted_events();
            SetFocus(other);
            focus_changed = true;
        }
        if (!own_focus()) { finish(false, L"before_controller_step_focus"); return; }
        const HWND foreground = GetForegroundWindow();
        const HWND focus = GetFocus();
        auto& pad = snapshot.slots[0];
        pad.connection = Connection::Connected;
        pad.connection_epoch = 1;
        pad.buttons = next.buttons;
        pad.left_y = next.aim;
        pad.left_x = next.aim_x;
        pad.left_trigger = next.shift;
        snapshot.sampled_at = monotonic_now();
        ++snapshot.sequence;
        keyboard.step(snapshot, snapshot.sampled_at, {true, {}});
        if (one_handed && keyboard.visible() && keyboard.model().input_mode() != KeyboardInputMode::OneHanded) {
            finish(false, L"one_handed_mode_lost");
            return;
        }
        maximum_dispatch_ms = (std::max)(maximum_dispatch_ms, keyboard.delivery().maximum_dispatch_ms());
        if (!popup.update(boundary.window, keyboard)) {
            finish(false, L"popup_update");
            return;
        }
        if (GetForegroundWindow() != foreground || GetFocus() != focus) { finish(false, L"popup_focus_preservation"); return; }
        if (!one_handed && !focus_changed && action >= 3 && action < 16 && !keyboard.accepts_typing()) {
            finish(false, L"typing_not_ready"); return;
        }
        awaiting_receipt = true; // Dispatch WM_CHAR before verifying this operation, never only the final buffer.
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* smoke = reinterpret_cast<Smoke*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        smoke = static_cast<Smoke*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(smoke));
    }
    if (smoke) {
        if (message == WM_COMMAND && HIWORD(wparam) == EN_CHANGE
            && reinterpret_cast<HWND>(lparam) == smoke->boundary.edit) {
            // Model accessibility providers that repeat focus after edits, plus unrelated notifications.
            NotifyWinEvent(EVENT_OBJECT_FOCUS, smoke->boundary.edit, OBJID_CLIENT, CHILDID_SELF);
            NotifyWinEvent(EVENT_OBJECT_FOCUS, smoke->background, OBJID_CLIENT, CHILDID_SELF);
        }
        if (message == live::context_message && wparam) {
            smoke->keyboard.block(L"Unsafe desktop changed");
        }
        if (message == WM_INPUT && smoke->boundary.desktop.raw_input(reinterpret_cast<HRAWINPUT>(lparam))) {
            smoke->ready_since.reset();
            if (smoke->started) {
                smoke->physical_abort = true;
                smoke->keyboard.block(L"Physical input interrupted owned test");
            }
        }
        if (message == WM_TIMER) { smoke->tick(); return 0; }
        if (message == WM_CLOSE) { smoke->finish(false, L"window_close"); return 0; }
        if (message == WM_WTSSESSION_CHANGE) {
            smoke->boundary.desktop.session_changed(wparam);
            smoke->keyboard.block(L"Session changed");
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int wmain(int argc, wchar_t** argv)
{
    const bool one_handed = argc == 3 && std::wstring_view{argv[2]} == L"--one-handed";
    if ((argc != 2 && !one_handed) || std::wstring_view{argv[1]} != L"--allow-owned-text-smoke") {
        std::cerr << "Build-only by default. Execution needs approval and --allow-owned-text-smoke [--one-handed].\n";
        return 2;
    }
    const HANDLE singleton = CreateMutexW(nullptr, TRUE, mapper_mutex_name);
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton) { CloseHandle(singleton); }
        std::cerr << "Exit mapper/preview before the owned input test; nothing started.\n";
        return 1;
    }
    struct InstanceGuard {
        HANDLE handle;
        ~InstanceGuard() { ReleaseMutex(handle); CloseHandle(handle); }
    } guard{singleton};
    Smoke smoke;
    if (one_handed) {
        smoke.one_handed = true;
        smoke.script = one_handed_actions;
        smoke.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
    }
    WNDCLASSEXW type{sizeof(type)};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = window_proc;
    type.lpszClassName = class_name;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&type)) { return 1; }
    const auto window = CreateWindowExW(0, class_name, L"Owned keyboard test - click the first text field (5 seconds)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 260, nullptr, nullptr, type.hInstance, &smoke);
    if (!window) { return 1; }
    smoke.boundary.window = window;
    smoke.boundary.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"seed",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL,
        20, 25, 650, 85, window, nullptr, type.hInstance, nullptr);
    smoke.other = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        20, 130, 650, 30, window, nullptr, type.hInstance, nullptr);
    smoke.background = CreateWindowExW(0, class_name, L"Owned background focus fixture", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, type.hInstance, &smoke);
    if (!smoke.background) { smoke.finish(false, L"background_fixture_creation"); return 1; }
    const bool hotkey = RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10) != FALSE;
    if (!smoke.boundary.edit || !smoke.other || !hotkey || !smoke.boundary.desktop.initialize(window, hotkey)) {
        std::wcerr << L"Owned smoke failure: stage=initialization edit_ready=" << (smoke.boundary.edit != nullptr)
            << L" second_edit_ready=" << (smoke.other != nullptr) << L" hotkey_ready=" << hotkey
            << L"\nInfrastructure: " << smoke.boundary.desktop.infrastructure() << L'\n';
        DestroyWindow(window);
        return 1;
    }
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(smoke.boundary.edit);
    SendMessageW(smoke.boundary.edit, EM_SETSEL, 4, 4);
    smoke.deadline = monotonic_now() + 5000;
    if (!SetTimer(window, 1, 16, nullptr)) { smoke.finish(false, L"timer_creation"); return 1; }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_HOTKEY) { smoke.finish(false, L"emergency_hotkey"); break; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    std::cout << (smoke.result == 0 ? "PASS per-edit typing, controls, reopen, popup focus and changed-target rejection\n"
        : "FAIL owned live-keyboard smoke\n");
    return smoke.result;
}
