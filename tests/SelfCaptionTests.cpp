#include "SelfCaption.h"

#include <Windows.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>

using namespace gamepad;
namespace {
void check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
CaptionContext input(CaptionButton button = CaptionButton::Minimize)
{
    return {1, 1, 2, 0, 3, button, true, true, true};
}
void unit_tests()
{
    {
        SelfCaption action;
        auto context = input();
        context.primary_down = false; // Accepted down; reader sees A-up before queued NC-down dispatch.
        check(action.down(context, true), "queued owned down must not fall into native tracking after latest A-up");
        check(action.pending(), "still-owned accepted down is valid quick-release evidence");
        check(!action.take(context), "quick release must still wait for processed up");
        context.primary_owned = false;
        check(action.up(context, true) && action.take(context).has_value(),
            "quick-release gesture commits only after matching processed up and released ledger");
    }
    for (int invalid = 0; invalid < 5; ++invalid) {
        SelfCaption action;
        auto context = input();
        if (invalid == 0) { context.primary_owned = context.primary_down = false; } // Both down/up already queued.
        if (invalid == 1) { context.eligible = false; } // Stale sample, changed identity or lost hit.
        if (invalid == 2) { context.foreground = 0; }
        if (invalid == 3) { context.eligible = false; context.primary_down = false; }
        if (invalid == 4) { context.primary_owned = false; } // Unowned/cleanup message.
        check(action.down(context, true), "ineligible tagged self caption down must always be consumed");
        check(!action.pending(), "consuming an invalid message must not authorize a caption action");
        context = input(); context.primary_owned = context.primary_down = false;
        check(!action.up(context, true) && !action.take(context), "later queued up cannot revive rejected down");
    }
    for (const auto button : {CaptionButton::Minimize, CaptionButton::Maximize, CaptionButton::Close}) {
        SelfCaption action;
        auto context = input(button);
        check(!action.down(context, false) && !action.pending(), "physical mouse must pass to Windows");
        check(action.down(context, true), "owned tagged down must avoid native tracking");
        check(!action.take(context), "down cannot commit");
        context.primary_down = context.primary_owned = false;
        action.observe(context);
        check(!action.take(context), "SendInput acceptance without processed up must not commit");
        check(action.up(context, true), "matching tagged up with released ledger must arm commit");
        check(action.take(context)->button == button && !action.take(context), "exactly one matching caption command");
    }
    for (int change = 0; change < 8; ++change) {
        SelfCaption action; auto context = input();
        check(action.down(context, true), "begin");
        switch (change) {
        case 0: ++context.window; break;
        case 1: ++context.foreground; break;
        case 2: ++context.focus; break;
        case 3: ++context.slot; break;
        case 4: ++context.epoch; break;
        case 5: context.eligible = false; break;
        case 6: context.button = CaptionButton::None; break;
        case 7: context.button = CaptionButton::Close; break;
        }
        action.observe(context);
        context.primary_down = context.primary_owned = false;
        check(!action.up(context, true) && !action.take(context) && !action.pending(), "identity/leave/unsafe must cancel");
    }
    for (int kind = 0; kind < 6; ++kind) {
        SelfCaption action; auto context = input();
        check(action.down(context, true), "begin");
        if (kind == 0) { action.cancel(); } // Physical takeover/pause/device/destruction.
        if (kind == 1) { check(action.down(context, true), "repeat down consumed without repeating action"); }
        if (kind == 2) { context.primary_owned = false; action.observe(context); } // Cleanup up while A held.
        if (kind == 3) { context.primary_down = false; check(!action.up(context, true), "failed release cannot commit"); }
        if (kind == 4) {
            context.primary_down = context.primary_owned = false;
            check(!action.up(context, false), "untagged physical up cannot commit");
        }
        if (kind == 5) {
            context.primary_down = context.primary_owned = false;
            check(action.up(context, true), "ready");
            context.primary_down = context.primary_owned = true; action.observe(context);
        }
        context.primary_down = context.primary_owned = false;
        check(!action.up(context, true) && !action.take(context), "cancelled action cannot revive on later up");
    }
    SelfCaption action; auto context = input();
    check(action.down(context, true, true) && !action.pending(), "tagged double-click must not perform two commands");
    context.primary_owned = false;
    check(action.down(context, true) && !action.pending(), "unaccepted tagged down is consumed without authorizing ownership");
    context = input(); context.button = CaptionButton::None;
    check(!action.down(context, true), "caption drag and system menus remain native");
    std::puts("PASS pure caption commands, processed-up barrier, identity/cancel/cleanup/failure/repeat guards");
}

struct Probe {
    SelfCaption action;
    CaptionContext context;
    HWND window = nullptr;
    unsigned timers = 0;
    bool baseline = false, completed = false, correct = false, up_processed = false, up_already_queued = false;
    static constexpr UINT commit = WM_APP + 21;
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* self = reinterpret_cast<Probe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Probe*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) { return DefWindowProcW(window, message, wparam, lparam); }
        if (message == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
        if (message == WM_NCLBUTTONDOWN) {
            std::printf("caption down baseline=%d\n", self->baseline);
            if (!self->baseline) { self->context.primary_down = false; } // Exact fresh-A-up/queued-NC-down interleaving.
            if (self->up_already_queued) { self->context.primary_owned = false; }
            if (!self->baseline && self->action.down(self->context, true)) { return 0; }
        }
        if (message == WM_TIMER) {
            if (++self->timers == 5) {
                // Fake boundary ledger/sample only. No SendInput or physical button state change.
                self->context.primary_owned = self->context.primary_down = false;
                self->action.observe(self->context);
                check(!self->action.take(self->context), "no system command before queued up is processed");
                RECT rect{}; GetWindowRect(window, &rect);
                const auto hit = self->context.button == CaptionButton::Minimize ? HTMINBUTTON
                    : self->context.button == CaptionButton::Maximize ? HTMAXBUTTON : HTCLOSE;
                PostMessageW(window, WM_NCLBUTTONUP, hit, MAKELPARAM(rect.right - 60, rect.top + 14));
            }
            return 0;
        }
        if (message == WM_NCLBUTTONUP && !self->baseline) {
            self->up_processed = true;
            if (self->action.up(self->context, true)) { PostMessageW(window, commit, 0, 0); }
            else if (self->up_already_queued) {
                DefWindowProcW(window, message, wparam, lparam);
                self->correct = !self->action.pending() && IsWindow(window) && !IsIconic(window) && !IsZoomed(window);
                self->completed = true;
            }
            return 0;
        }
        if (message == commit) {
            const auto action = self->action.take(self->context);
            check(action.has_value() && self->up_processed, "up processed and ledger released before SC command");
            const auto command = action->button == CaptionButton::Minimize ? SC_MINIMIZE
                : action->button == CaptionButton::Maximize ? SC_MAXIMIZE : SC_CLOSE;
            DefWindowProcW(window, WM_SYSCOMMAND, command, 0);
            self->correct = command == SC_MINIMIZE ? IsIconic(window) != FALSE
                : command == SC_MAXIMIZE ? IsZoomed(window) != FALSE : !IsWindow(window);
            self->completed = true;
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};

void message_tests(bool baseline)
{
    const auto foreground = GetForegroundWindow();
    WNDCLASSW type{}; type.lpfnWndProc = Probe::proc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"GamepadDesktop.OwnedCaptionTest";
    check(RegisterClassW(&type), "register owned caption class");
    for (const bool up_already_queued : {false, true}) {
    for (const auto button : {CaptionButton::Minimize, CaptionButton::Maximize, CaptionButton::Close}) {
        Probe probe; probe.context = input(button); probe.baseline = baseline;
        probe.up_already_queued = up_already_queued;
        const auto window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, type.lpszClassName,
            L"Owned caption message-only test", WS_OVERLAPPEDWINDOW,
            -30000, -30000, 488, 324, nullptr, nullptr, type.hInstance, &probe);
        check(window != nullptr, "create owned caption window");
        probe.context.window = reinterpret_cast<std::uintptr_t>(window);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        check(SetTimer(window, 1, 16, nullptr), "timer");
        RECT rect{}; GetWindowRect(window, &rect);
        const auto hit = button == CaptionButton::Minimize ? HTMINBUTTON
            : button == CaptionButton::Maximize ? HTMAXBUTTON : HTCLOSE;
        PostMessageW(window, WM_NCLBUTTONDOWN, hit, MAKELPARAM(rect.right - 60, rect.top + 14));
        const auto deadline = GetTickCount64() + 2000;
        while (!probe.completed && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
            Sleep(1);
        }
        if (IsWindow(window)) { KillTimer(window, 1); DestroyWindow(window); }
        check(probe.completed && probe.correct && probe.timers >= 5, "caption command must complete with continuing timers");
        std::printf("PASS owned message caption=%d timers=%u processed_up=%d up_already_queued=%d\n",
            static_cast<int>(button), probe.timers, probe.up_processed, up_already_queued);
    }
    }
    UnregisterClassW(type.lpszClassName, type.hInstance);
    check(GetForegroundWindow() == foreground, "owned message-only tests must not change foreground");
}
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        unit_tests();
        if (argc == 2) {
            const std::string_view option{argv[1]};
            check(option == "--owned-messages" || option == "--tracker-baseline", "unknown test option");
            // Baseline intentionally enters the reproduced native tracking hang.
            // Run it ONLY under an external bounded process watchdog.
            message_tests(option == "--tracker-baseline");
        } else { check(argc == 1, "invalid arguments"); }
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
