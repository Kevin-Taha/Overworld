#include "CursorPresentation.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace overworld;
namespace {
void check(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

struct FakeOverlay final : CursorOverlay {
    explicit FakeOverlay(std::vector<std::string>& log) : calls(log) {}
    bool present(int diameter, CursorStyle next) override
    {
        calls.emplace_back("publish");
        if (fail) { problem = L"Fake render failure"; return false; }
        visible = true; size = diameter; style = next;
        return true;
    }
    void hide() override { calls.emplace_back("conceal"); visible = false; }
    void reset() override { calls.emplace_back("reset"); problem.clear(); fail = false; }
    const std::wstring& error() const override { return problem; }
    std::vector<std::string>& calls;
    bool visible = false, fail = false;
    int size = 0;
    CursorStyle style = CursorStyle::Ring;
    std::wstring problem;
};

struct FakeVisibility final : CursorVisibility {
    explicit FakeVisibility(std::vector<std::string>& log) : calls(log) {}
    bool initialize() override
    {
        calls.emplace_back("initialize");
        if (fail_init) { problem = L"Fake init failure"; return false; }
        initialized = true;
        return true;
    }
    NativeCursorState observe() override { calls.emplace_back("observe"); return state; }
    bool show(bool visible) override
    {
        check(initialized, "visibility requires initialized runtime");
        calls.emplace_back(visible ? "show-native" : "hide-native");
        if ((visible && fail_show) || (!visible && fail_hide)) { problem = L"Fake visibility failure"; return false; }
        state = visible ? NativeCursorState::Visible : NativeCursorState::Hidden;
        return true;
    }
    bool shutdown() override
    {
        calls.emplace_back("shutdown");
        if (fail_shutdown) { problem = L"Fake uninitialize failure"; return false; }
        initialized = false;
        return true;
    }
    const std::wstring& error() const override { return problem; }
    std::vector<std::string>& calls;
    NativeCursorState state = NativeCursorState::Visible;
    bool initialized = false, fail_init = false, fail_hide = false, fail_show = false, fail_shutdown = false;
    std::wstring problem;
};

struct Fixture {
    std::vector<std::string> calls;
    FakeOverlay overlay{calls};
    FakeVisibility visibility{calls};
    CursorPresentation cursor{overlay, visibility};
    Settings settings;
    void active() { cursor.update(true, settings); }
    int count(std::string_view operation) const
    {
        return static_cast<int>(std::count(calls.begin(), calls.end(), operation));
    }
};

std::uint32_t at(const std::vector<std::uint32_t>& pixels, const RingGeometry& geometry, int x, int y)
{
    return pixels[static_cast<std::size_t>(y) * geometry.side + x];
}
} // namespace

int main()
{
    unsigned count = 0;
    const auto test = [&](const char* name, const std::function<void()>& body) {
        body(); ++count; std::cout << "PASS " << name << '\n';
    };
    try {
        test("failed cursor image queries recover without repeated polling or identity changes", [] {
            CursorImageQuerySchedule query;
            check(query.take(false, 1000), "first unknown image must be queried");
            for (Tick now = 1001; now < 2000; ++now) {
                check(!query.take(false, now), "failed image lookup must not repeat on each cursor tick");
            }
            check(query.take(false, 2000), "one transient image lookup failure must not cache Unknown forever");
            check(!query.take(true, 3000), "live verified process image can remain cached");
            query.reset();
            check(query.take(false, 3001), "new root identity must allow an immediate lookup");
            check(query.take(false, 999), "clock rollback must reestablish image evidence");
        });
        test("retrying unknown image never fabricates Start or shows unowned hiding", [] {
            CursorImageQuerySchedule query;
            CursorSurfaceRoot root{{}, L"Windows.UI.Core.CoreWindow", true, true, false, false};
            Fixture f;
            StartCursorFallback fallback;
            check(query.take(false, 1000), "initial image probe");
            fallback.update(f.cursor, classify_cursor_surface(root), false, f.settings);
            check(!fallback.active() && f.count("show-native") == 0, "query failure must stay inconclusive");
            check(query.take(false, 2000), "bounded retry after failed lookup");
            root.executable = L"StartMenuExperienceHost.exe";
            fallback.update(f.cursor, classify_cursor_surface(root), false, f.settings);
            check(fallback.active() && f.count("show-native") == 0 && !f.cursor.owns_hide(),
                "verified Start can establish fallback without force-showing unowned native state");
        });
        test("Start classifier requires exact executable class and validated visible root", [] {
            CursorSurfaceRoot root{L"StartMenuExperienceHost.exe", L"Windows.UI.Core.CoreWindow", true, true, false, false};
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Start, "observed Start root");
            root.executable = L"STARTMENUEXPERIENCEHOST.EXE";
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Start, "Windows executable basename is case insensitive");
            for (const auto name : {L"explorer.exe", L"ApplicationFrameHost.exe", L"StartMenuExperienceHost.exe.fake", L"Other.exe"}) {
                root.executable = name;
                check(classify_cursor_surface(root) == CursorSurfaceObservation::Other, "generic/spoof-suffix CoreWindow must not qualify");
            }
            root.executable = L"StartMenuExperienceHost.exe";
            for (const auto name : {L"Shell_TrayWnd", L"XamlExplorerHostIslandWindow", L"CabinetWClass", L"Other"}) {
                root.window_class = name;
                check(classify_cursor_surface(root) == CursorSurfaceObservation::Other, "Start process alone is not enough");
            }
            root.window_class = L"Windows.UI.Core.CoreWindow";
            root.visible = false;
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Other, "hidden root");
            root.visible = true; root.iconic = true;
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Other, "iconic root");
            root.iconic = false; root.cloaked = true;
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Other, "cloaked root");
        });
        test("Start classifier keeps missing or invalid identity inconclusive", [] {
            CursorSurfaceRoot root{L"StartMenuExperienceHost.exe", L"Windows.UI.Core.CoreWindow", true, true, false, false};
            root.identity_valid = false;
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Unknown, "invalid root identity");
            root.identity_valid = true; root.cloaked.reset();
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Unknown, "failed cloak observation");
            root.cloaked = false; root.executable = {};
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Unknown, "failed image observation");
            root.window_class = {};
            check(classify_cursor_surface(root) == CursorSurfaceObservation::Unknown, "failed class observation");
        });
        test("foreground or hit Start wins and unknown cannot prove departure", [] {
            using enum CursorSurfaceObservation;
            for (const auto other : {Start, Other, Unknown}) {
                check(combine_cursor_surfaces(Start, other) == Start && combine_cursor_surfaces(other, Start) == Start,
                    "either observed Start root requests fallback");
            }
            check(combine_cursor_surfaces(Other, Other) == Other, "both roots prove departure");
            check(combine_cursor_surfaces(Other, Unknown) == Unknown
                && combine_cursor_surfaces(Unknown, Other) == Unknown
                && combine_cursor_surfaces(Unknown, Unknown) == Unknown, "missing root is not proof of departure");
        });
        test("Start entry restores owned hide once before concealing overlay", [] {
            Fixture f; f.active(); f.calls.clear();
            StartCursorFallback fallback;
            fallback.update(f.cursor, CursorSurfaceObservation::Start, true, f.settings);
            check(f.calls == std::vector<std::string>{"show-native", "conceal"}, "pause must restore before conceal");
            check(fallback.active() && !f.cursor.owns_hide() && !f.cursor.recovery_pending()
                && !f.cursor.replacement_suspended(), "successful presentation fallback must not suspend desktop mapping");
            for (int i = 0; i < 100; ++i) { fallback.update(f.cursor, CursorSurfaceObservation::Start, true, f.settings); }
            check(f.count("show-native") == 1 && f.count("hide-native") == 0 && f.count("publish") == 0,
                "repeated Start polls do not mutate global visibility again");
        });
        test("Start fallback leaves unowned external hiding untouched even under explicit escape", [] {
            Fixture f; f.visibility.state = NativeCursorState::Hidden;
            StartCursorFallback fallback;
            fallback.update(f.cursor, CursorSurfaceObservation::Start, true, f.settings, true);
            check(f.calls == std::vector<std::string>{"conceal"} && !f.cursor.owns_hide(),
                "Start fallback must not initialize or force-show unowned native hiding");
            check(!f.cursor.recovery_pending() && !f.cursor.replacement_suspended(), "normal input is not faulted");
        });
        test("inconclusive Start departure retains native fallback without reacquiring", [] {
            Fixture f; f.active();
            StartCursorFallback fallback;
            fallback.update(f.cursor, CursorSurfaceObservation::Start, true, f.settings);
            f.calls.clear();
            for (int i = 0; i < 100; ++i) { fallback.update(f.cursor, CursorSurfaceObservation::Unknown, true, f.settings); }
            check(fallback.active() && !fallback.status().empty(), "uncertainty remains visible");
            check(f.count("show-native") == 0 && f.count("hide-native") == 0 && f.count("publish") == 0,
                "uncertainty cannot reacquire native hiding");
            check(!f.cursor.recovery_pending() && !f.cursor.replacement_suspended(), "observation uncertainty does not fault input");
        });
        test("Start exit republishes before reacquiring and later reentry releases only owned hide", [] {
            Fixture f; f.active();
            StartCursorFallback fallback;
            fallback.update(f.cursor, CursorSurfaceObservation::Start, true, f.settings);
            f.calls.clear();
            fallback.update(f.cursor, CursorSurfaceObservation::Other, true, f.settings);
            check(f.calls == std::vector<std::string>{"publish", "initialize", "observe", "hide-native"}
                && f.cursor.owns_hide() && !fallback.active(), "custom cursor safely resumes");
            f.calls.clear();
            fallback.update(f.cursor, CursorSurfaceObservation::Start, true, f.settings);
            check(f.calls == std::vector<std::string>{"show-native", "conceal"}, "reentry releases reacquired ownership once");
        });
        test("Start restore failure retains ownership overlay and explicit recovery without poll retries", [] {
            Fixture f; f.active(); f.calls.clear(); f.visibility.fail_show = true;
            StartCursorFallback fallback;
            fallback.update(f.cursor, CursorSurfaceObservation::Start, true, f.settings);
            check(f.calls == std::vector<std::string>{"show-native"} && f.cursor.owns_hide()
                && f.overlay.visible && f.cursor.recovery_pending(), "retain last overlay and ownership on failed restore");
            const auto calls = f.calls;
            for (const auto observation : {CursorSurfaceObservation::Start, CursorSurfaceObservation::Unknown, CursorSurfaceObservation::Other}) {
                fallback.update(f.cursor, observation, true, f.settings, true);
                check(f.calls == calls && f.cursor.recovery_pending(), "no automatic restoration retry or fault bypass");
            }
        });
        test("surface hint preserves inactive and explicit-app presentation behavior elsewhere", [] {
            Fixture f;
            StartCursorFallback fallback;
            fallback.update(f.cursor, CursorSurfaceObservation::Other, false, f.settings, true);
            check(f.count("initialize") == 0 && f.count("publish") == 0, "inactive mapping cannot acquire cursor");
            f.visibility.state = NativeCursorState::Hidden;
            fallback.update(f.cursor, CursorSurfaceObservation::Unknown, true, f.settings, true);
            check(!fallback.active() && f.overlay.visible && !f.cursor.owns_hide()
                && f.count("hide-native") == 0 && f.count("show-native") == 0, "unknown outside Start keeps explicit-app semantics");
        });
        test("startup saved preference and neutral arming never initialize or hide", [] {
            Fixture f;
            check(f.calls.empty(), "constructors do not access native state");
            check(f.cursor.apply(), "load/Apply while disabled");
            for (int i = 0; i < 10; ++i) { f.cursor.update(false, f.settings); }
            check(f.count("initialize") == 0 && f.count("show-native") == 0 && f.count("hide-native") == 0, "inactive never touches global visibility");
        });
        test("inactive lifecycle never initializes or restores native state", [] {
            Fixture f;
            f.cursor.update(false, f.settings); check(!f.overlay.visible, "inactive has no overlay");
            check(f.cursor.pause() && f.cursor.apply() && f.cursor.shutdown(), "inactive lifecycle");
            check(!f.cursor.owns_hide() && f.count("initialize") == 0 && f.count("show-native") == 0
                && f.count("hide-native") == 0, "no show when not owned");
        });
        test("publish precedes initialize observe and hide with one owned acquisition", [] {
            Fixture f;
            f.active();
            check(f.calls == std::vector<std::string>{"publish", "initialize", "observe", "hide-native"}, "strict publish-before-hide order");
            check(f.cursor.owns_hide() && f.overlay.visible, "replacement active");
            f.active(); f.active();
            check(f.count("hide-native") == 1 && f.count("observe") == 1, "no repeated global hide calls");
        });
        test("failed first render never requests native hiding", [] {
            Fixture f; f.overlay.fail = true; f.active();
            check(!f.overlay.visible && !f.cursor.owns_hide() && f.count("initialize") == 0 && !f.cursor.error().empty(), "first render failure visible");
        });
        test("explicit enable waits for accepted controller activity", [] {
            Fixture f; check(f.cursor.enable(), "enable");
            f.cursor.update(false, f.settings);
            check(!f.overlay.visible && !f.cursor.owns_hide() && f.count("initialize") == 0, "enable alone cannot hide native");
        });
        test("accepted ownership loss restores before conceal and does not show again", [] {
            // Live's existing fake tests establish controller_active=false on physical takeover,
            // pause, stale/disconnect, excluded/protected contexts and display changes.
            Fixture f; f.active(); f.calls.clear();
            f.cursor.update(false, f.settings);
            check(f.calls == std::vector<std::string>{"show-native", "conceal"} && !f.cursor.owns_hide(), "restore before hide on inactive edge");
            f.cursor.update(false, f.settings); f.cursor.pause();
            check(f.count("show-native") == 1, "inactive polls do not override external visibility");
        });
        test("direct physical pause command restores immediately before next timer", [] {
            Fixture f; f.active(); f.calls.clear();
            check(f.cursor.pause(), "pause succeeds");
            check(f.calls == std::vector<std::string>{"show-native", "conceal"}, "immediate ordered pause");
        });
        test("settings apply restores before clearing renderer", [] {
            Fixture f; f.active(); f.calls.clear();
            check(f.cursor.apply(), "apply");
            check(f.calls == std::vector<std::string>{"show-native", "conceal", "reset"} && !f.cursor.owns_hide(), "apply restores");
        });
        test("explicit recovery retains only the native pointer", [] {
            Fixture f; f.active(); check(f.cursor.restore(), "restore");
            f.calls.clear(); f.active();
            check(!f.overlay.visible && !f.cursor.owns_hide() && f.count("publish") == 0 && f.count("hide-native") == 0,
                  "recovery must not fall back to duplicate overlay plus native pointer");
        });
        test("style switches keep one hide ownership and publish correct pixels request", [] {
            Fixture f; f.active();
            for (const auto& choice : cursor_styles) {
                f.settings.cursor_style = choice.style; f.settings.ring_diameter = 68; f.active();
                check(f.overlay.style == choice.style && f.overlay.size == 68 && f.cursor.owns_hide(), "selected style request");
            }
            check(f.count("hide-native") == 1 && f.count("show-native") == 0, "no visibility flicker on style changes");
        });
        test("Apply restores before renderer reset and waits for accepted controller use", [] {
            Fixture f; f.active(); f.calls.clear();
            check(f.cursor.apply(), "configuration boundary");
            check(f.calls == std::vector<std::string>{"show-native", "conceal", "reset"}, "safe Apply reset order");
            f.cursor.update(false, f.settings);
            check(f.count("hide-native") == 0, "Apply never reacquires alone");
            f.active(); check(f.cursor.owns_hide() && f.count("hide-native") == 1, "later controller use may acquire");
        });
        test("initialization failure is latched and never shows an unowned cursor", [] {
            Fixture f; f.visibility.fail_init = true; f.active(); f.active(); f.cursor.pause();
            check(f.cursor.replacement_suspended() && !f.cursor.owns_hide() && !f.cursor.error().empty(), "init error surfaced");
            check(!f.overlay.visible, "failed initialization must not leave a duplicate custom cursor");
            check(f.count("initialize") == 1 && f.count("hide-native") == 0 && f.count("show-native") == 0, "init failure no global mutation or poll retries");
        });
        test("already hidden and unknown observations do not take ownership or auto show", [] {
            for (const auto state : {NativeCursorState::Hidden, NativeCursorState::Unknown}) {
                Fixture f; f.visibility.state = state; f.active(); f.active(); f.cursor.pause(); f.cursor.shutdown();
                check(!f.cursor.owns_hide() && f.cursor.replacement_suspended() && !f.cursor.error().empty(), "observation error surfaced");
                check(!f.overlay.visible, "unknown/external hiding must not leave an unowned overlay");
                check(f.count("hide-native") == 0 && f.count("show-native") == 0, "external hiding untouched");
            }
        });
        test("explicit app escape presents only our overlay over externally hidden cursor", [] {
            Fixture f; f.visibility.state = NativeCursorState::Hidden;
            f.cursor.update(true, f.settings, true);
            check(f.overlay.visible && !f.cursor.owns_hide() && !f.cursor.replacement_suspended(),
                "explicit app escape should present a non-owning overlay");
            f.cursor.pause(); f.cursor.shutdown();
            check(!f.overlay.visible && f.count("show-native") == 0 && f.count("hide-native") == 0,
                "non-owning escape must never mutate another app's native visibility");
        });
        test("explicit app escape never bypasses unknown visibility or restore fault", [] {
            Fixture f; f.visibility.state = NativeCursorState::Unknown;
            f.cursor.update(true, f.settings, true);
            check(!f.overlay.visible && f.count("show-native") == 0 && f.count("hide-native") == 0,
                "unknown visibility cannot authorize overlay escape");
            Fixture g; g.active(); g.visibility.fail_show = true; g.cursor.pause();
            const auto calls = g.calls;
            g.cursor.update(true, g.settings, true);
            check(g.cursor.recovery_pending() && g.calls == calls, "escape bypassed owned restoration fault");
        });
        test("non-owning escape can later acquire normally visible cursor safely", [] {
            Fixture f; f.visibility.state = NativeCursorState::Hidden;
            f.cursor.update(true, f.settings, true);
            f.visibility.state = NativeCursorState::Visible;
            f.cursor.update(true, f.settings, true);
            check(f.cursor.owns_hide() && f.count("hide-native") == 1 && f.count("show-native") == 0,
                "normal visibility acquisition after non-owning presentation");
            f.cursor.pause();
            check(f.count("show-native") == 1, "only our later owned hiding may be restored");
        });
        test("failed hide does not claim replacement and does not blindly show", [] {
            Fixture f; f.visibility.fail_hide = true; f.active();
            check(!f.overlay.visible && !f.cursor.owns_hide() && f.cursor.replacement_suspended()
                && f.visibility.state == NativeCursorState::Visible && !f.cursor.error().empty(), "failed hide normal pointer remains");
            f.active(); f.cursor.pause();
            check(f.count("hide-native") == 1 && f.count("show-native") == 0, "failed acquisition not treated as ownership");
        });
        test("failed renderer restores native before dropping last published overlay", [] {
            Fixture f; f.active(); f.calls.clear(); f.overlay.fail = true; f.active();
            check(f.calls == std::vector<std::string>{"publish", "show-native", "conceal"} && !f.cursor.owns_hide()
                && !f.overlay.visible && !f.cursor.error().empty(), "renderer loss fallback");
        });
        test("renderer plus restore failure retains last visible image and ownership", [] {
            Fixture f; f.active(); f.calls.clear(); f.overlay.fail = true; f.visibility.fail_show = true; f.active();
            check(f.calls == std::vector<std::string>{"publish", "show-native"}, "do not conceal failed recovery image");
            check(f.overlay.visible && f.cursor.owns_hide() && f.cursor.recovery_pending() && f.cursor.replacement_suspended(), "failure ownership retained");
            const auto calls = f.calls;
            f.active(); f.cursor.update(false, f.settings); check(!f.cursor.enable() && !f.cursor.pause(), "cannot reset recovery away");
            check(f.calls == calls, "no unbounded retry or renderer reset");
        });
        test("failed restore on configuration enable or inactive transition retains recovery visual", [] {
            for (int transition = 0; transition < 4; ++transition) {
                Fixture f; f.active(); f.calls.clear(); f.visibility.fail_show = true;
                if (transition == 0) { f.cursor.update(false, f.settings); }
                if (transition == 1) { check(!f.cursor.apply(), "failed apply restoration"); }
                if (transition == 2) { check(!f.cursor.enable(), "failed enable restoration"); }
                if (transition == 3) { check(!f.cursor.pause(), "failed immediate pause"); }
                check(f.overlay.visible && f.cursor.owns_hide() && f.cursor.recovery_pending() && f.count("conceal") == 0, "retain on transition failure");
            }
        });
        test("explicit recovery restores failed ownership and suspends through unrelated Apply", [] {
            Fixture f; f.active(); f.visibility.fail_show = true; f.cursor.pause();
            f.visibility.fail_show = false;
            check(f.cursor.restore() && !f.cursor.owns_hide() && !f.cursor.recovery_pending() && !f.overlay.visible, "explicit recovery success");
            const auto hides = f.count("hide-native");
            f.active(); check(f.cursor.apply(), "unrelated settings apply"); f.active();
            check(f.count("hide-native") == hides && f.cursor.replacement_suspended() && !f.overlay.visible, "no instant rehide or duplicate cursor");
            check(f.cursor.enable(), "deliberate Desktop reenable"); f.cursor.update(false, f.settings);
            check(f.count("hide-native") == hides, "reenable Apply still waits for accepted use");
            f.active(); check(f.count("hide-native") == hides + 1 && f.cursor.owns_hide(), "explicit opt-in rearms");
        });
        test("user restore can repair earlier crash without current hide ownership", [] {
            Fixture f; f.visibility.state = NativeCursorState::Hidden;
            check(f.cursor.restore(), "forced recovery");
            check(f.calls == std::vector<std::string>{"initialize", "show-native", "conceal"}, "only user request overrides unowned state");
            f.active();             check(f.count("hide-native") == 0 && f.cursor.replacement_suspended() && !f.overlay.visible,
                  "recovery never activates replacement or duplicate overlays");
        });
        test("failed explicit recovery remains visible fault even without hide ownership", [] {
            Fixture f; f.visibility.fail_show = true;
            check(!f.cursor.restore() && !f.cursor.owns_hide() && f.cursor.recovery_pending(), "unowned force-show failure surfaced");
            f.active(); check(!f.cursor.enable() && !f.cursor.pause(true), "failure cannot be cleared by settings/cleanup");
            f.visibility.fail_show = false;
            check(f.cursor.restore() && !f.cursor.recovery_pending(), "explicit retry clears confirmed success only");
        });
        test("normal teardown restores before conceal and runtime release", [] {
            Fixture f; f.active(); f.calls.clear();
            check(f.cursor.shutdown(), "shutdown succeeds");
            check(f.calls == std::vector<std::string>{"show-native", "conceal", "shutdown"}, "teardown ordered");
        });
        test("failed teardown retains ownership for ordinary Close veto and explicit retry", [] {
            Fixture f; f.active(); f.calls.clear(); f.visibility.fail_show = true;
            check(!f.cursor.shutdown() && f.cursor.owns_hide() && f.overlay.visible && f.count("shutdown") == 0, "failed restore prevents disposal");
            f.visibility.fail_show = false;
            check(f.cursor.shutdown() && !f.cursor.owns_hide() && f.cursor.error().empty(), "bounded close retry restores");
        });
        test("runtime cleanup error is explicit without inventing hide ownership", [] {
            Fixture f; f.active(); f.visibility.fail_shutdown = true;
            check(!f.cursor.shutdown() && !f.cursor.owns_hide() && !f.overlay.visible && !f.cursor.error().empty(), "runtime failure not hidden");
            const auto shows = f.count("show-native");
            f.visibility.fail_shutdown = false; check(f.cursor.shutdown(), "runtime retry");
            check(f.count("show-native") == shows, "no second TRUE when no owned hide remains");
        });
        test("legacy ring entry point still renders the Ring style byte-for-byte", [] {
            for (const unsigned dpi : {96u, 120u, 144u, 192u, 288u}) {
                for (const int size : {20, 40, 67, 96}) {
                    check(cursor_pixels(dpi, size, CursorStyle::Ring) == ring_pixels(dpi, size), "baseline pixels identical");
                }
            }
        });
        test("accepted dot and bracket art is byte-for-byte unchanged by the redesign", [] {
            // Digests captured from the pre-redesign renderer. The ring and pointer were
            // deliberately re-drawn; the styles the user already liked must not shift a bit.
            struct Golden { CursorStyle style; unsigned dpi; int size; std::uint64_t hash; };
            static constexpr Golden goldens[] = {
                {CursorStyle::Dot, 96, 20, 0x2f9624713f792d4full},
                {CursorStyle::Brackets, 96, 20, 0xe2bf318f5d69d043ull},
                {CursorStyle::Dot, 96, 32, 0x1abc5912152d470full},
                {CursorStyle::Brackets, 96, 32, 0x69fde2422d1d0bc3ull},
                {CursorStyle::Dot, 96, 40, 0x74f251bbddcbe81full},
                {CursorStyle::Brackets, 96, 40, 0xd72a2b91ada0f7a3ull},
                {CursorStyle::Dot, 96, 67, 0xd5d0c610769cc17full},
                {CursorStyle::Brackets, 96, 67, 0x97ce1449be585733ull},
                {CursorStyle::Dot, 96, 96, 0x1b390881980153efull},
                {CursorStyle::Brackets, 96, 96, 0x07af78a63db5d343ull},
                {CursorStyle::Dot, 192, 20, 0xb86e58d0484adeefull},
                {CursorStyle::Brackets, 192, 20, 0xd67749854a6fb323ull},
                {CursorStyle::Dot, 192, 32, 0xb9b956c7ddcde3efull},
                {CursorStyle::Brackets, 192, 32, 0x71d6d31c05167be3ull},
                {CursorStyle::Dot, 192, 40, 0xe3d553425cf9055full},
                {CursorStyle::Brackets, 192, 40, 0x9c1354e1ad0e4e23ull},
                {CursorStyle::Dot, 192, 67, 0x82d084260c59edcfull},
                {CursorStyle::Brackets, 192, 67, 0x593156103c83c7a3ull},
                {CursorStyle::Dot, 192, 96, 0xd28e604d651c5f4full},
                {CursorStyle::Brackets, 192, 96, 0xc91a14fc12f683a3ull},
                {CursorStyle::Dot, 768, 20, 0x5fcd68e4e12c1a6full},
                {CursorStyle::Brackets, 768, 20, 0x636eeeb4a83559c3ull},
                {CursorStyle::Dot, 768, 32, 0x6366b754314fbedfull},
                {CursorStyle::Brackets, 768, 32, 0x553c30f8b68b4183ull},
                {CursorStyle::Dot, 768, 40, 0x1dae2affd216b4cfull},
                {CursorStyle::Brackets, 768, 40, 0xcac2a76f86fdd4c3ull},
                {CursorStyle::Dot, 768, 67, 0x0d83ca1fd31dc09full},
                {CursorStyle::Brackets, 768, 67, 0xbddad167b529dae3ull},
                {CursorStyle::Dot, 768, 96, 0x7e0b1121383af04full},
                {CursorStyle::Brackets, 768, 96, 0x0d371369d29408c3ull},
            };
            for (const auto& golden : goldens) {
                std::uint64_t hash = 1469598103934665603ull;
                const auto pixels = cursor_pixels(golden.dpi, golden.size, golden.style);
                check(!pixels.empty(), "golden configuration still renders");
                for (const auto pixel : pixels) {
                    for (int byte = 0; byte < 4; ++byte) { hash = (hash ^ ((pixel >> (byte * 8)) & 255)) * 1099511628211ull; }
                }
                if (hash != golden.hash) {
                    throw std::runtime_error("Accepted style " + std::to_string(static_cast<int>(golden.style)) + " changed at DPI "
                        + std::to_string(golden.dpi) + " size " + std::to_string(golden.size));
                }
            }
        });
        test("accepted modern art stays byte-for-byte unchanged", [] {
            // Captured from normal Ring/Pointer rendering before removing hand hints.
            struct Golden { CursorStyle style; unsigned dpi; int size; std::uint64_t hash; };
            static constexpr Golden goldens[] = {
                {CursorStyle::Ring, 96, 24, 17858284562186111867ull},
                {CursorStyle::Ring, 96, 28, 4745272069104432939ull},
                {CursorStyle::Ring, 96, 32, 93019988719157819ull},
                {CursorStyle::Ring, 144, 24, 9780516505355539435ull},
                {CursorStyle::Ring, 144, 28, 111588924280511867ull},
                {CursorStyle::Ring, 144, 32, 13568349854832998467ull},
                {CursorStyle::Ring, 192, 24, 11399723562119851387ull},
                {CursorStyle::Ring, 192, 28, 4627080670250875051ull},
                {CursorStyle::Ring, 192, 32, 15446108318879010819ull},
                {CursorStyle::Arrow, 96, 24, 1448205381622060317ull},
                {CursorStyle::Arrow, 96, 28, 611758405805694217ull},
                {CursorStyle::Arrow, 96, 32, 15989363204757239401ull},
                {CursorStyle::Arrow, 144, 24, 9134770826241982693ull},
                {CursorStyle::Arrow, 144, 28, 11386174709949346232ull},
                {CursorStyle::Arrow, 144, 32, 18424913696558060921ull},
                {CursorStyle::Arrow, 192, 24, 1167831456197004904ull},
                {CursorStyle::Arrow, 192, 28, 3844733664306130119ull},
                {CursorStyle::Arrow, 192, 32, 10887665621482185404ull},
            };
            for (const auto& golden : goldens) {
                std::uint64_t hash = 1469598103934665603ull;
                for (const auto pixel : cursor_pixels(golden.dpi, golden.size, golden.style)) {
                    for (int byte = 0; byte < 4; ++byte) { hash = (hash ^ ((pixel >> (byte * 8)) & 255)) * 1099511628211ull; }
                }
                if (hash != golden.hash) {
                    throw std::runtime_error("Accepted modern style " + std::to_string(static_cast<int>(golden.style))
                        + " changed at DPI " + std::to_string(golden.dpi) + " size " + std::to_string(golden.size));
                }
            }
        });
        test("four-style geometry hotspots DPI and negative origins", [] {
            for (const auto& choice : cursor_styles) {
                for (const unsigned dpi : {96u, 120u, 144u, 192u, 288u, 768u}) {
                    for (const int size : {20, 40, 67, 96}) {
                        const auto g = cursor_geometry({-3840, -1080}, dpi, size, choice.style);
                        check(g && g->side % 2 == 1 && g->left + g->hotspot_x == -3840 && g->top + g->hotspot_y == -1080, "accurate negative-origin hotspot");
                        // The pointer aims from just inside its leading corner; every other style is centred.
                        if (choice.style == CursorStyle::Arrow) {
                            check(g->hotspot_x == g->hotspot_y && g->hotspot_x >= 1 && g->hotspot_x * 4 < g->side, "pointer tip leads its own buffer");
                        } else { check(g->hotspot_x == g->side / 2 && g->hotspot_y == g->side / 2, "symmetric style center"); }
                    }
                }
            }
        });
        test("all styles reject invalid enum dimensions DPI and coordinate overflow", [] {
            for (const auto& choice : cursor_styles) {
                check(!cursor_geometry({}, 95, 40, choice.style) && !cursor_geometry({}, 769, 40, choice.style)
                    && !cursor_geometry({}, 96, 19, choice.style) && !cursor_geometry({}, 96, 97, choice.style)
                    && !cursor_geometry({INT_MIN, 0}, 96, 40, choice.style)
                    && !cursor_geometry({0, INT_MAX}, 96, 40, choice.style), "bounded coordinates");
                check(cursor_pixels(0, 40, choice.style).empty(), "invalid pixels");
            }
            check(!cursor_geometry({}, 96, 40, static_cast<CursorStyle>(99))
                && cursor_pixels(96, 40, static_cast<CursorStyle>(99)).empty(), "invalid enum");
        });
        test("four styles are bounded antialiased and premultiplied per channel", [] {
            for (const auto& choice : cursor_styles) {
                // Ring and pointer wear the redesigned palette; the dot and brackets the user
                // already accepted keep their original neutral ink.
                const bool modern = choice.style == CursorStyle::Ring || choice.style == CursorStyle::Arrow;
                const std::uint32_t lit_ink = modern ? 0xfff6f3ecu : 0xffffffffu;
                const std::uint32_t dark_ink = modern ? 0xff1c1e24u : 0xff000000u;
                for (const unsigned dpi : {96u, 120u, 144u, 192u}) {
                    for (const int size : {20, 24, 28, 32, 40, 96}) {
                        if (!modern && (size == 24 || size == 28 || size == 32)) { continue; } // Extra small-raster coverage is for the changed art.
                        const auto g = *cursor_geometry({}, dpi, size, choice.style);
                        const auto pixels = cursor_pixels(dpi, size, choice.style);
                        check(pixels.size() == static_cast<std::size_t>(g.side) * g.side, "exact pixel buffer size");
                        bool body = false, contour = false, antialias = false;
                        for (int y = 0; y < g.side; ++y) {
                            for (int x = 0; x < g.side; ++x) {
                                const auto pixel = at(pixels, g, x, y);
                                const auto alpha = pixel >> 24;
                                for (const int shift : {0, 8, 16}) {
                                    check(((pixel >> shift) & 255) <= alpha, "every channel stays within its own premultiplied alpha");
                                }
                                if (x == 0 || y == 0 || x == g.side - 1 || y == g.side - 1) { check(pixel == 0, "transparent border, no clipping"); }
                                body |= pixel == lit_ink; contour |= pixel == dark_ink; antialias |= alpha > 0 && alpha < 255;
                            }
                        }
                        if (!body || !contour || !antialias) {
                            throw std::runtime_error("Missing contrast/AA for style " + std::to_string(static_cast<int>(choice.style))
                                + " DPI " + std::to_string(dpi) + " size " + std::to_string(size));
                        }
                    }
                }
            }
        });
        test("redesigned ring and pointer wear a warm off-white body in a cool charcoal contour", [] {
            const auto luma = [](std::uint32_t p) {
                return (((p >> 16) & 255) * 2126 + ((p >> 8) & 255) * 7152 + (p & 255) * 722) / 10000;
            };
            for (const auto style : {CursorStyle::Ring, CursorStyle::Arrow}) {
                for (const unsigned dpi : {96u, 144u, 288u, 768u}) {
                    for (const int size : {20, 32, 40, 96}) {
                        std::uint32_t body = 0, contour = 0xffffffffu;
                        for (const auto pixel : cursor_pixels(dpi, size, style)) {
                            if ((pixel >> 24) != 255) { continue; }
                            if (luma(pixel) > luma(body)) { body = pixel; }
                            if (luma(pixel) < luma(contour)) { contour = pixel; }
                        }
                        check(body != 0 && contour != 0xffffffffu, "opaque body and contour both present");
                        const auto red = [](std::uint32_t p) { return (p >> 16) & 255; };
                        const auto green = [](std::uint32_t p) { return (p >> 8) & 255; };
                        const auto blue = [](std::uint32_t p) { return p & 255; };
                        check(red(body) > green(body) && green(body) > blue(body), "body leans warm, channels independent");
                        check(red(body) >= 230 && red(body) < 255, "body is off-white, not blown out to pure white");
                        check(blue(contour) > green(contour) && green(contour) > red(contour), "contour leans cool, channels independent");
                        check(blue(contour) <= 64 && red(contour) > 0, "contour is charcoal, not pure black");
                        check(luma(body) - luma(contour) > 180, "body and contour stay far apart in luminance");
                    }
                }
            }
        });
        test("ring keeps a clear centre inside a thicker stroke that fits its nominal box", [] {
            for (const unsigned dpi : {96u, 120u, 144u, 192u, 288u}) {
                for (const int size : {20, 32, 40, 67, 96}) {
                    const auto g = *cursor_geometry({}, dpi, size, CursorStyle::Ring);
                    const auto pixels = cursor_pixels(dpi, size, CursorStyle::Ring);
                    int hole = 0, stroke = 0, gap = 0;
                    for (int x = g.hotspot_x; x < g.side; ++x) {
                        const bool inked = at(pixels, g, x, g.hotspot_y) != 0;
                        if (!inked && stroke == 0) { ++hole; } else if (inked) { ++stroke; } else { ++gap; }
                    }
                    check(hole > 0 && stroke > 0 && gap > 0, "clear centre, drawn stroke, and margin before the edge");
                    check(stroke * 100 >= (hole + stroke) * 30, "stroke claims a substantial share of the drawn radius");
                    check((hole + stroke) * 2 <= size * static_cast<int>(dpi) / 96, "drawn circle sits inside its nominal size");
                    check(at(pixels, g, g.hotspot_x, g.hotspot_y) == 0, "the centre stays see-through");
                }
            }
        });
        test("pointer is a compact triangular head without a narrow stem and with exact tip hotspot", [] {
            for (const unsigned dpi : {96u, 120u, 144u, 192u, 288u}) {
                for (const int size : {20, 24, 28, 32, 40, 67, 96}) {
                    const auto g = *cursor_geometry({}, dpi, size, CursorStyle::Arrow);
                    const auto pixels = cursor_pixels(dpi, size, CursorStyle::Arrow);
                    check(at(pixels, g, g.hotspot_x, g.hotspot_y) != 0, "the aim point itself carries ink");
                    int ink = 0, widest = 0, rows = 0, columns = 0;
                    long long mass_x = 0, mass_y = 0;
                    for (int y = 0; y < g.side; ++y) {
                        int run = 0;
                        for (int x = 0; x < g.side; ++x) {
                            if (at(pixels, g, x, y) == 0) { continue; }
                            ++run; ++ink; mass_x += x; mass_y += y;
                        }
                        if (run > 0) { ++rows; }
                        widest = (std::max)(widest, run);
                    }
                    for (int x = 0; x < g.side; ++x) {
                        for (int y = 0; y < g.side; ++y) {
                            if (at(pixels, g, x, y) != 0) { ++columns; break; }
                        }
                    }
                    check(rows == columns, "balanced short head, not a tall stemmed arrow");
                    check(rows <= size * (dpi / 96.0) * .70 + 1, "head stays compact rather than inflated");
                    check(mass_x > static_cast<long long>(g.hotspot_x) * ink && mass_y > static_cast<long long>(g.hotspot_y) * ink, "body trails behind the tip");
                    check(ink * 5 > rows * widest * 2, "filled arrow head rather than a thin outline");
                    check(ink * 4 < rows * widest * 3, "still tapered, not a filled rectangle");
                    const double extent = size * (dpi / 96.0);
                    if (extent >= 180) {
                        const double translation = std::clamp(size * .035, .7, 1.75) * (dpi / 96.0) * .7071067811865475;
                        const auto sample = [&](double x, double y) {
                            return at(pixels, g, g.hotspot_x + static_cast<int>(std::lround(x * extent + translation)),
                                g.hotspot_y + static_cast<int>(std::lround(y * extent + translation)));
                        };
                        check(sample(.365, .365) == 0 && sample(.50, .19) != 0 && sample(.19, .50) != 0,
                            "shallow rear notch between two rounded shoulders, not a stem");
                    }
                }
            }
        });
        test("compact dot clear brackets and tip arrow remain genuinely distinct", [] {
            const auto dot = *cursor_geometry({}, 96, 40, CursorStyle::Dot);
            const auto ring = *cursor_geometry({}, 96, 40, CursorStyle::Ring);
            const auto brackets = cursor_pixels(96, 40, CursorStyle::Brackets);
            const auto arrow = *cursor_geometry({}, 96, 40, CursorStyle::Arrow);
            const auto arrow_pixels = cursor_pixels(96, 40, CursorStyle::Arrow);
            check(dot.side == 13 && ring.side == 43 && at(cursor_pixels(96, 40, CursorStyle::Dot), dot, dot.hotspot_x, dot.hotspot_y) == 0xffffffff,
                "10-DIP outlined dot at 40-DIP nominal size");
            check(arrow.side == 31 && arrow.hotspot_x == 3, "pointer buffer is tighter than the centred styles");
            for (int i = 0; i < ring.side; ++i) {
                check(at(brackets, ring, i, ring.hotspot_y) == 0 && at(brackets, ring, ring.hotspot_x, i) == 0, "brackets clear center and open axes");
            }
            check(at(arrow_pixels, arrow, arrow.hotspot_x, arrow.hotspot_y) != 0, "arrow actual tip has coverage at the hotspot");
            check(arrow_pixels != cursor_pixels(96, 40, CursorStyle::Ring) && brackets != cursor_pixels(96, 40, CursorStyle::Ring)
                && arrow_pixels != brackets, "not recolored versions of one shape");
        });
        test("all supported size and DPI geometries keep the exact hotspot and bounded guard", [] {
            for (const auto& choice : cursor_styles) {
                for (unsigned dpi = 96; dpi <= 768; ++dpi) {
                    for (int size = 20; size <= 96; ++size) {
                        const auto g = cursor_geometry({-7000, 450}, dpi, size, choice.style);
                        check(g && g->left + g->hotspot_x == -7000 && g->top + g->hotspot_y == 450
                            && g->hotspot_x > 0 && g->hotspot_y > 0 && g->hotspot_x < g->side - 1, "exact placement");
                    }
                }
            }
        });
        test("small polished pixels have real sub-sixteenth coverage and a smaller footprint", [] {
            for (const auto style : {CursorStyle::Ring, CursorStyle::Arrow}) {
                for (int size : {24, 28, 32}) {
                    const auto g = *cursor_geometry({}, 96, size, style);
                    const auto pixels = cursor_pixels(96, size, style);
                    bool fine_coverage = false;
                    int min_x = g.side, min_y = g.side, max_x = 0, max_y = 0;
                    for (int y = 0; y < g.side; ++y) {
                        for (int x = 0; x < g.side; ++x) {
                            const auto a = at(pixels, g, x, y) >> 24;
                            if (!a) { continue; }
                            bool old_level = false;
                            for (unsigned i = 0; i <= 16; ++i) { old_level |= a == (i * 255 + 8) / 16; }
                            fine_coverage |= !old_level;
                            min_x = (std::min)(min_x, x); max_x = (std::max)(max_x, x);
                            min_y = (std::min)(min_y, y); max_y = (std::max)(max_y, y);
                        }
                    }
                    check(fine_coverage, "actual small raster uses more precise alpha than the prior 4x4 edges");
                    check(max_y - min_y + 1 <= size * .84 + 1, "visible height smaller than old nominal inset");
                    if (style == CursorStyle::Arrow) {
                        check(max_x - min_x + 1 == max_y - min_y + 1 && max_x - min_x + 1 <= size * .70 + 1,
                            "small balanced rounded head with no long stem");
                    }
                }
            }
        });
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << count << " cases: " << error.what() << '\n';
        return 1;
    }
    std::cout << count << " cursor pixel / fake visibility cases passed.\n";
    return 0;
}
