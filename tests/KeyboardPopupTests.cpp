#include "KeyboardPopup.h"
#include "KeyboardVisual.h"

#include <iostream>
#include <stdexcept>

using namespace gamepad;
namespace gamepad {
struct KeyboardPopupTestAccess {
    static bool update(KeyboardPopup& popup, HWND host, KeyboardDelivery& keyboard, VoicePresentation voice = {})
    {
        return popup.update_window(host, keyboard, voice, host, false);
    }
    static HWND window(const KeyboardPopup& popup) { return popup.window_; }
    static KeyboardVisualFrame frame(const KeyboardPopup& popup) { return popup.visual_->frame(); }
    static bool prepare(KeyboardPopup& popup, HWND host, KeyboardDelivery& keyboard)
    {
        return popup.prepare_window(host, keyboard, {}, host, true);
    }
};
}
namespace {
void check(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }
class Theme final : public KeyboardThemeSource {
public:
    HRESULT result = S_OK;
    unsigned reads = 0;
    HRESULT read(KeyboardTheme& theme) override { ++reads; theme = {}; return result; }
};
class Boundary final : public TextBoundary {
public:
    Tick now = 1000;
    unsigned events = 0;
    TextObservation observation;
    Boundary()
    {
        observation.allowed = observation.cleanup_allowed = observation.stable = true;
        observation.physical_held = false;
        observation.target.windows = {{{1, 1, 1}, {1, 1, 1}, {2, 1, 1}}};
    }
    TextObservation observe_text(std::optional<TextKey>) override { return observation; }
    Tick text_now() const override { return now; }
    unsigned send_text(TextEvent) override { ++events; return 1; }
};
class Shell final : public KeyboardShellSource {
public:
    KeyboardShellObservation observation;
    KeyboardShellObservation observe(HWND) override { return observation; }
};
struct Fixture {
    Boundary boundary;
    KeyboardDelivery keyboard{boundary};
    Snapshot snapshot;
    Fixture() { snapshot.slots[0].connection = Connection::Connected; snapshot.slots[0].connection_epoch = 1; }
    void tick(std::uint16_t buttons = 0)
    {
        snapshot.slots[0].buttons = buttons;
        snapshot.sampled_at = boundary.now += 16;
        ++snapshot.sequence;
        keyboard.step(snapshot, boundary.now, {true, {}});
    }
    void open()
    {
        tick(); tick(); tick(0x8000); tick();
        check(keyboard.accepts_typing(), "deliberate opener must reacquire typing");
    }
    void word()
    {
        snapshot.slots[0].left_x = snapshot.slots[0].left_y = 26000;
        tick(); tick(0x8000); tick(); tick(0x1000); tick();
        snapshot.slots[0].left_x = snapshot.slots[0].left_y = 0;
        tick();
        check(keyboard.prediction_view().count == 3, "two accepted own letters show real candidates");
    }
};
void geometry()
{
    for (UINT dpi : {96u, 144u, 192u, 768u}) {
        for (RECT work : {RECT{0, 0, 1920, 1040}, RECT{-1920, -1080, 0, -40}, RECT{-100, 50, 200, 250}}) {
            for (int height : {keyboard_visual_height, keyboard_prediction_height}) {
                const auto bounds = keyboard_popup_bounds(work, dpi, height);
                check(bounds && bounds->left >= work.left && bounds->top >= work.top
                    && bounds->right <= work.right && bounds->bottom <= work.bottom
                    && bounds->right > bounds->left && bounds->bottom > bounds->top, "bounded work-area geometry");
                const auto anchored = keyboard_popup_bounds(work, dpi, height, RECT{INT_MIN, INT_MAX, 0, 0});
                check(anchored && anchored->left == work.left && anchored->bottom == work.bottom, "off-edge anchor clamps without overflow");
            }
        }
    }
    check(!keyboard_popup_bounds({0, 0, 0, 0}, 96, 350)
        && !keyboard_popup_bounds({INT_MIN, 0, INT_MAX, 1000}, 96, 350)
        && !keyboard_popup_bounds({0, 0, 1000, 1000}, 0, 350)
        && !keyboard_popup_bounds({0, 0, 1000, 1000}, 96, 0), "invalid geometry fails explicitly");
}
void lifecycle(HWND host)
{
    const auto initial_dpi = GetDpiForWindow(host);
    const auto normal_height = MulDiv(keyboard_visual_height, initial_dpi, 96);
    const auto expanded_height = MulDiv(keyboard_prediction_height, initial_dpi, 96);
    Fixture f;
    Theme theme;
    KeyboardPopup popup(&theme);
    const auto update = [&](VoicePresentation voice = {}) {
        const auto events = f.boundary.events;
        if (!KeyboardPopupTestAccess::update(popup, host, f.keyboard, voice)) {
            std::wcerr << popup.error() << L'\n';
            throw std::runtime_error("owned popup update failed");
        }
        check(f.boundary.events == events, "presentation must not emit even fake input");
        const auto window = KeyboardPopupTestAccess::window(popup);
        if (window) {
            check(!(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE), "owned test popup must stay hidden");
            check((GetWindowLongPtrW(window, GWL_EXSTYLE) & (WS_EX_NOACTIVATE | WS_EX_LAYERED))
                == (WS_EX_NOACTIVATE | WS_EX_LAYERED), "production noactivate layered styles retained");
        }
    };
    f.open(); update();
    auto window = KeyboardPopupTestAccess::window(popup);
    check(KeyboardPopupTestAccess::frame(popup).height == normal_height, "initial empty strip is collapsed");
    f.word(); update();
    check(KeyboardPopupTestAccess::frame(popup).height == expanded_height, "real suggestions expand popup HWND");
    const auto allocations = KeyboardPopupTestAccess::frame(popup).surface_allocations;
    for (unsigned i = 0; i < 3; ++i) {
        f.tick(0x0008); f.tick(); update();
        check(KeyboardPopupTestAccess::frame(popup).height == expanded_height
            && KeyboardPopupTestAccess::frame(popup).surface_allocations == allocations,
            "changing selected candidate length does not resize or recreate the popup surface");
    }
    auto before = KeyboardPopupTestAccess::frame(popup);
    for (unsigned i = 0; i < 40; ++i) { update(); }
    check(KeyboardPopupTestAccess::frame(popup).rasterizations == before.rasterizations
        && KeyboardPopupTestAccess::frame(popup).surface_allocations == before.surface_allocations, "idle popup reuses surface");
    for (UINT dpi : {144u, 192u, 96u}) {
        RECT suggested{-1600, -900, -800, -200};
        SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        update();
        check(KeyboardPopupTestAccess::frame(popup).height == MulDiv(keyboard_prediction_height, dpi, 96),
            "DPI transition retains expanded section scale");
        f.tick(0x0002); f.tick(); update();
        check(f.keyboard.prediction_hint().empty()
            && KeyboardPopupTestAccess::frame(popup).height == MulDiv(keyboard_visual_height, dpi, 96),
            "Down hides footer text and collapses native height at current DPI");
        const auto events = f.boundary.events;
        f.tick(0x0001); f.tick();
        check(f.boundary.events == events, "collapsed suggestions cannot append old suffix");
        f.tick(0x0020); f.tick(); update(); f.open(); f.word(); update();
        window = KeyboardPopupTestAccess::window(popup);
    }
    update({VoicePhase::Loading, 0, {}});
    check(KeyboardPopupTestAccess::frame(popup).height == normal_height, "voice collapses suggestions without losing voice status");
    update();
    check(SetWindowPos(window, nullptr, -30000, -20000, 588, 624, SWP_NOACTIVATE | SWP_NOZORDER), "owned negative placement");
    update();
    check(KeyboardPopupTestAccess::frame(popup).width == 588, "real resized window dimensions are rendered");
    f.boundary.observation.prediction_privacy = PredictionPrivacy::Protected; f.tick(); update();
    check(f.keyboard.prediction_hint().empty() && KeyboardPopupTestAccess::frame(popup).height == normal_height,
        "quiet suppression has no null-layout error or empty strip");
    f.keyboard.set_prediction_enabled(false); update();
    check(KeyboardPopupTestAccess::frame(popup).height == normal_height, "saved opt-out remains visually absent");

    theme.result = E_ACCESSDENIED;
    SendMessageW(window, WM_THEMECHANGED, 0, 0);
    check(!KeyboardPopupTestAccess::update(popup, host, f.keyboard) && !popup.error().empty()
        && !f.keyboard.accepts_typing(), "real theme failure hides and blocks the current keyboard");
    const auto reads = theme.reads;
    theme.result = S_OK;
    for (unsigned i = 0; i < 5; ++i) {
        check(!KeyboardPopupTestAccess::update(popup, host, f.keyboard), "same failed session cannot retry");
    }
    check(theme.reads == reads, "no blanket repeated rendering retries");
    f.tick(0x0020); f.tick(); update();
    check(!popup.error().empty(), "close retains diagnostic until an actual successful new draw");
    const auto events = f.boundary.events;
    f.open(); update();
    check(popup.error().empty() && theme.reads > reads && f.boundary.events == events,
        "explicit reopen creates a fresh renderer, clears error only after success and never replays input");
    f.keyboard.block(L"Key release needs recovery. View closes."); update();
    check(KeyboardPopupTestAccess::frame(popup).height == normal_height, "error status still renders while suggestions are absent");
    f.tick(0x0020); f.tick(); update(); f.open(); update();
    window = KeyboardPopupTestAccess::window(popup);
    check(SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER), "owned zero-size transition");
    check(!KeyboardPopupTestAccess::update(popup, host, f.keyboard)
        && popup.error().find(L"0x80070057") != std::wstring::npos, "invalid native client size reports real E_INVALIDARG");
    f.tick(0x0020); f.tick(); update(); f.open(); update();
    check(popup.error().empty() && KeyboardPopupTestAccess::frame(popup).height == normal_height,
        "deliberate new session recovers valid geometry rather than reusing invalid native size");
    window = KeyboardPopupTestAccess::window(popup);
    RECT invalid{};
    SendMessageW(window, WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&invalid));
    check(!KeyboardPopupTestAccess::update(popup, host, f.keyboard) && !popup.error().empty(),
        "malformed DPI transition fails explicitly rather than using guessed geometry");
}
void shell_placement(HWND host)
{
    Fixture f;
    Theme theme;
    Shell shell;
    KeyboardPopup popup(&theme, &shell);
    f.open();
    check(KeyboardPopupTestAccess::update(popup, host, f.keyboard), "owned popup initial frame");
    RECT before{};
    check(GetWindowRect(KeyboardPopupTestAccess::window(popup), &before), "owned initial geometry");
    MONITORINFO monitor{sizeof(monitor)};
    check(GetMonitorInfoW(MonitorFromWindow(KeyboardPopupTestAccess::window(popup), MONITOR_DEFAULTTONEAREST), &monitor),
        "owned popup monitor");
    const auto work = monitor.rcWork;
    const LONG width = before.right - before.left;
    const LONG height = before.bottom - before.top;
    // A small synthetic shell obstruction over the default popup; no external shell query.
    shell.observation.obstruction = RECT{before.left + width / 2 - 10, before.top,
        before.left + width / 2 + 10, before.top + 10};
    const auto expected = keyboard_popup_bounds(work, GetDpiForWindow(host), keyboard_visual_height, before, shell.observation.obstruction);
    const auto events = f.boundary.events;
    if (expected) {
        check(KeyboardPopupTestAccess::prepare(popup, host, f.keyboard), "pre-delivery shell avoidance succeeds");
        RECT after{};
        check(GetWindowRect(KeyboardPopupTestAccess::window(popup), &after) && EqualRect(&after, &*expected),
            "actual hidden HWND follows shared unoccluded placement");
        check(after.right - after.left == width && after.bottom - after.top == height, "popup renderer is not scaled down");
        check(KeyboardPopupTestAccess::update(popup, host, f.keyboard), "relocated popup still renders");
        const auto cached = KeyboardPopupTestAccess::frame(popup);
        for (unsigned i = 0; i < 10; ++i) { check(KeyboardPopupTestAccess::update(popup, host, f.keyboard), "stable shell updates"); }
        check(KeyboardPopupTestAccess::frame(popup).rasterizations == cached.rasterizations
            && KeyboardPopupTestAccess::frame(popup).surface_allocations == cached.surface_allocations, "shell idle retains render caches");
    }
    shell.observation.obstruction = work;
    check(!KeyboardPopupTestAccess::prepare(popup, host, f.keyboard) && !f.keyboard.accepts_typing()
        && popup.error().find(L"No full-size") != popup.error().npos, "no-space blocks before the next typing step");
    f.tick(0x1000); f.tick();
    check(f.boundary.events == events, "obstructed keyboard sends no character or replay");
    shell.observation.obstruction.reset();
    check(!KeyboardPopupTestAccess::update(popup, host, f.keyboard), "removing shell cannot silently resume failed keyboard session");
    f.tick(0x0020); f.tick();
    check(KeyboardPopupTestAccess::update(popup, host, f.keyboard), "explicit close clears failed presentation session");
    f.open();
    check(KeyboardPopupTestAccess::update(popup, host, f.keyboard), "deliberate reopen returns to ordinary placement");
    shell.observation.available = false;
    check(!KeyboardPopupTestAccess::prepare(popup, host, f.keyboard) && !f.keyboard.accepts_typing(),
        "unknown shell geometry cannot continue blindly");
}
}
int wmain(int argc, wchar_t** argv)
{
    try {
        check(argc == 2 && std::wstring_view(argv[1]) == L"--render-only", "requires --render-only; hidden owned windows only");
        const auto foreground = GetForegroundWindow(), focus = GetFocus();
        const auto host = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Static", L"Owned keyboard test",
            WS_POPUP, -30000, -30000, 800, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(host != nullptr, "owned host creation");
        geometry();
        lifecycle(host);
        shell_placement(host);
        DestroyWindow(host);
        check(GetForegroundWindow() == foreground && GetFocus() == focus, "popup rendering never takes focus");
        std::cout << "PASS hidden production popup: strip collapse, DPI/resize/negative geometry, quiet suppression, failure latch and deliberate reopen; no native input\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    return 0;
}
