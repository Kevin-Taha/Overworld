#include "Live.h"

#include <cmath>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace overworld;
using namespace overworld::live;

namespace {
void check(bool value, const char* description)
{
    if (!value) { throw std::runtime_error(description); }
}

void near(double a, double b, double tolerance = 0.001)
{
    check(std::abs(a - b) <= tolerance, "numeric result outside tolerance");
}

Point from_absolute(Point normalized, Rect desktop)
{
    check(valid(desktop), "invalid desktop for absolute-coordinate round trip");
    check(contains({0, 0, 65535, 65535}, normalized), "normalized coordinate outside input range");
    // Model the OS pixel-bucket conversion, not the requested physical point.
    const auto width = std::int64_t{desktop.right} - desktop.left + 1;
    const auto height = std::int64_t{desktop.bottom} - desktop.top + 1;
    return {static_cast<int>(desktop.left + normalized.x * width / 65536),
            static_cast<int>(desktop.top + normalized.y * height / 65536)};
}

class Fake final : public OutputBoundary {
public:
    Observation observation;
    std::deque<unsigned> returns;
    std::vector<Output> attempted;
    std::vector<Output> accepted;
    bool hit = true;
    Tick time = 1000;
    std::function<void()> during_hit;
    std::function<void(const Output&)> after_send;
    std::function<void(const Output*)> during_observe;
    std::wstring foreground = L"editor.exe";
    std::wstring mouse = L"explorer.exe";
    std::wstring keyboard = L"editor.exe";
    std::wstring capture;
    bool protected_mouse = false;
    bool protected_keyboard = false;
    std::vector<Output> validated;
    AppIdentity app{10, 100};
    void activation_changed(AppIdentity next) { app = next; observe_activation(next); }

    Fake()
    {
        observation.available = observation.allowed = observation.baseline_known = observation.cleanup_allowed = true;
        observation.mode = Mode::Pointer;
        observation.context = 1;
        observation.bounds = observation.desktop = {-1920, -1080, 1919, 1079};
        observation.expires = 2000;
    }
    Observation observe(unsigned, const Output* intent = nullptr) override
    {
        if (during_observe) { during_observe(intent); }
        if (intent) {
            validated.push_back(*intent);
            if (during_hit && intent->kind != OutputKind::PrimaryUp
                && intent->kind != OutputKind::SecondaryUp && intent->kind != OutputKind::EscapeUp) { during_hit(); }
        }
        auto result = observation;
        result.app = observe_activation(app);
        const bool key = intent && (intent->kind == OutputKind::EscapeDown || intent->kind == OutputKind::EscapeUp);
        const bool protected_recipient = protected_keyboard || (!key && protected_mouse);
        if (protected_recipient) {
            result.allowed = result.baseline_known = result.cleanup_allowed = false;
            result.block = BlockReason::Recipient;
        }
        if (executable_excluded(foreground, exclusions()) || executable_excluded(keyboard, exclusions())
            || (!key && (executable_excluded(mouse, exclusions()) || executable_excluded(capture, exclusions())))
            || (!key && !hit)) { result.allowed = false; result.block = hit ? BlockReason::Exclusion : BlockReason::Routing; }
        apply_auto_policy(result);
        return result;
    }
    Tick now() const override { return time; }
    unsigned send(const Output& output, Rect desktop) override
    {
        attempted.push_back(output);
        const unsigned count = returns.empty() ? 1 : returns.front();
        if (!returns.empty()) { returns.pop_front(); }
        if (count == 1) {
            accepted.push_back(output);
            if (output.kind == OutputKind::Move) {
                observation.cursor = from_absolute(absolute(output.point, desktop), desktop);
            }
            if (after_send) { after_send(output); }
        }
        return count;
    }
    unsigned count(OutputKind kind) const
    {
        unsigned result = 0;
        for (const auto& output : accepted) { if (output.kind == kind) { ++result; } }
        return result;
    }
};

struct Fixture {
    Fake fake;
    Live live{fake};
    Snapshot snapshot;
    Fixture()
    {
        snapshot.sequence = 1;
        snapshot.sampled_at = fake.time;
        for (auto& c : snapshot.slots) {
            c.connection = Connection::Connected;
            c.connection_epoch = 1;
        }
    }
    Controller& pad() { return snapshot.slots[0]; }
    void tick(Tick delta = 16)
    {
        fake.time += delta;
        snapshot.sampled_at = fake.time;
        ++snapshot.sequence;
        live.step(snapshot, fake.time);
    }
    void automatic_tick(bool ready = true, Tick delta = 16)
    {
        fake.time += delta;
        snapshot.sampled_at = fake.time;
        ++snapshot.sequence;
        live.automatic_step(snapshot, fake.time, ready);
    }
    void arm()
    {
        live.select(0, snapshot, fake.time);
        live.enable(fake.time);
        tick();
        check(live.armed(), "fresh neutral did not arm live");
        check(fake.accepted.empty(), "arming emitted output");
    }
    void drag()
    {
        arm();
        pad().buttons = a_button;
        tick();
        check(live.owned() == Primary, "drag should own accepted primary");
    }
    void neutral_then_press()
    {
        pad().buttons = 0;
        tick();
        check(live.armed(), "new neutral should rearm");
        pad().buttons = a_button;
        tick();
    }
};

void radial_response()
{
    near(response(6000, 6000, 0.27).x, 0);
    check(response(7000, 7000, 0.27).x > 0, "deadzone must be radial");
    near(response(32767, 0, 0.20).x, 1);
    const auto diagonal = response(-32768, -32768, 0.20);
    near(std::hypot(diagonal.x, diagonal.y), 1);
    const auto half = response(19660, 0, 0.20);
    near(half.x, 0.25);
    near(response(32767, 0, std::numeric_limits<double>::quiet_NaN()).x, 0);
    near(response(32767, 0, 1).x, 0);
}

Point integrate(Tick interval, bool precision)
{
    Pointer pointer;
    pointer.reset(1000);
    Controller pad;
    pad.left_x = 32767;
    pad.buttons = precision ? 0x0100 : 0;
    Point point{};
    for (Tick now = 1000 + interval; now <= 2000; now += interval) {
        point = pointer.step(pad, point, {-5000, -5000, 5000, 5000}, now, true).point;
    }
    return point;
}

void time_precision_fractions()
{
    near(integrate(10, false).x, 1100, 1);
    near(integrate(20, false).x, 1100, 1);
    near(integrate(10, true).x, 275, 1);
    check(integrate(10, true).y == 0, "unexpected vertical drift");
}

void scroll_fractions()
{
    Pointer pointer;
    pointer.reset(0);
    Controller pad;
    pad.right_y = 10000;
    int wheel = 0;
    for (Tick now = 10; now <= 1000; now += 10) {
        wheel += pointer.step(pad, {}, {-100, -100, 100, 100}, now, true).wheel;
    }
    check(wheel > 0 && wheel < 20, "small scroll fractions lost or excessive");
    pointer.reset(1000);
    pad.right_y = -32768;
    wheel = 0;
    for (Tick now = 1010; now <= 2000; now += 10) {
        wheel += pointer.step(pad, {}, {-100, -100, 100, 100}, now, true).wheel;
    }
    near(wheel, -960, 1);
    check(pointer.step(pad, {}, {-100, -100, 100, 100}, 2010, false).wheel == 0, "drag must suppress scroll");
}

void hysteresis_and_gap()
{
    Pointer pointer;
    pointer.reset(1000);
    Controller pad;
    pad.left_x = 7500; // Below enter threshold.
    Point point{};
    for (Tick now = 1010; now <= 2000; now += 10) {
        point = pointer.step(pad, point, {-1000, -1000, 1000, 1000}, now, true).point;
    }
    check(point == Point{}, "drift below enter deadzone");
    pad.left_x = 32767;
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 2080, true).point.x <= 36, "uncapped integration");
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 2400, true).point == Point{}, "gap teleported cursor");
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 2300, true).point == Point{}, "backwards time moved cursor");
}

void coordinates()
{
    const Rect desktop{-1920, -1080, 1919, 1079};
    check(from_absolute(absolute({-1920, -1080}, desktop), desktop) == Point{-1920, -1080}, "negative origin normalization");
    check(from_absolute(absolute({1919, 1079}, desktop), desktop) == Point{1919, 1079}, "last pixel normalization");
    check(from_absolute(absolute({9000, -9000}, desktop), desktop) == Point{1919, -1080}, "absolute clamp");
    const Rect single{-42, 7, -42, 7};
    check(from_absolute(absolute({-42, 7}, single), single) == Point{-42, 7}, "one-pixel display");
    check(!valid({0, 0, 70000, 10}) && !valid({}), "bad bounds accepted");
    near(absolute({0, 0}, desktop).x, 32776, 1);
    Pointer pointer;
    Controller pad;
    pad.left_x = 32767;
    pad.left_y = -32768;
    pointer.reset(1000);
    const int limit = (std::numeric_limits<int>::max)();
    check(pointer.step(pad, {limit, limit}, {limit - 10, limit - 10, limit, limit}, 1016, true).point
          == Point{limit, limit}, "safe coordinate arithmetic overflowed");
}

void absolute_pixel_round_trips()
{
    const Rect hd{0, 0, 1919, 1079};
    check(from_absolute(absolute({8, 8}, hd), hd) == Point{8, 8},
          "near-origin target moved into preceding pixel/non-client border");
    for (int count : {1, 2, 3, 17, 1280, 1920, 3440, 3840, 7680, 32767, 32768, 32769, 65535, 65536}) {
        for (int origin : {0, -1920, (std::numeric_limits<int>::min)(),
                           (std::numeric_limits<int>::max)() - count + 1}) {
            const Rect desktop{origin, origin, origin + (count - 1), origin + (count - 1)};
            for (int offset = 0; offset < count; ++offset) {
                const Point point{origin + offset, origin + (count - 1 - offset)};
                check(from_absolute(absolute(point, desktop), desktop) == point,
                      "absolute coordinate escaped its intended physical-pixel bucket");
            }
        }
    }
}

void disabled_observer_boundaries()
{
    Fixture f;
    const auto initial = f.live.status();
    f.live.invalidate(f.fake.time, L"physical takeover");
    check(!f.live.enabled() && f.live.status() == initial, "observer replaced disabled status");
    f.arm();
    f.live.pause(f.fake.time);
    const auto paused = f.live.status();
    f.live.invalidate(f.fake.time, L"foreground changed");
    check(!f.live.enabled() && f.live.status() == paused, "observer replaced explicit pause status");
    check(f.fake.accepted.empty(), "disabled observation emitted output");
}

void startup_and_explicit_selection()
{
    Fixture f;
    f.pad().buttons = a_button;
    f.live.enable(f.fake.time);
    f.tick();
    check(!f.live.enabled() && f.fake.accepted.empty(), "enable auto-selected a slot");
    f.live.select(4, f.snapshot, f.fake.time);
    f.live.enable(f.fake.time);
    check(!f.live.enabled(), "invalid selection accepted");
    f.live.select(0, f.snapshot, f.fake.time);
    f.live.enable(f.fake.time);
    f.tick();
    check(!f.live.armed(), "held controller armed");
    f.neutral_then_press();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "fresh A mapping");
}

void all_controls_neutral()
{
    Controller pad;
    pad.left_x = 6600;
    check(!pointer_neutral(pad), "live neutrality must be inside hysteresis exit");
    pad.left_x = 0;
    pad.right_x = 7600;
    check(!pointer_neutral(pad), "right stick neutrality");
    pad.right_x = 0;
    pad.right_trigger = 31;
    check(!pointer_neutral(pad), "trigger neutrality");
    pad.right_trigger = 0;
    pad.buttons = 0x8000;
    check(!pointer_neutral(pad), "reserved future button neutral gate");
    pad.buttons = 0x0400;
    check(pointer_neutral(pad), "Guide must be untouched");
}

void physical_baseline()
{
    Fixture f;
    f.fake.observation.baseline_known = false;
    f.live.select(0, f.snapshot, f.fake.time);
    f.live.enable(f.fake.time);
    f.tick();
    check(!f.live.armed(), "unknown physical baseline armed");
    f.fake.observation.baseline_known = true;
    f.fake.observation.physical_held = Other;
    f.tick();
    check(!f.live.armed(), "held modifier/key baseline armed");
    f.fake.observation.physical_held = Primary;
    f.tick();
    check(!f.live.armed(), "held mouse baseline armed");
    f.fake.observation.physical_held = 0;
    f.tick();
    check(f.live.armed(), "known released physical baseline did not arm");
}

void synthetic_baseline_classification()
{
    check(physical_holds(Primary, 0, Primary) == 0, "our accepted drag was mistaken for physical input");
    check(physical_holds(Secondary | Escape, 0, Secondary | Escape) == 0,
          "our accepted tap holds were mistaken for physical input");
    check(physical_holds(Primary | Other, 0, Primary) == Other, "owned drag hid physical modifier");
    check(physical_holds(Primary, Primary, Primary) == Primary, "genuine physical overlap was masked");
    check(physical_holds(0, Primary, Primary) == Primary, "raw overlap must survive stale asynchronous state");
    check(physical_holds(Primary | Escape, 0, 0) == (Primary | Escape), "pre-existing holds were ignored");
}

void pre_enable_cached_sample()
{
    Fixture f;
    f.live.select(0, f.snapshot, f.fake.time);
    f.live.enable(f.fake.time);
    f.live.step(f.snapshot, f.fake.time);
    check(!f.live.armed() && f.fake.accepted.empty(), "cached pre-enable neutral armed");
    f.tick();
    check(f.live.armed(), "new post-enable neutral did not arm");
    f.live.pause(f.fake.time);
    f.live.enable(f.fake.time + 1);
    f.live.step(f.snapshot, f.fake.time + 1);
    check(!f.live.armed(), "cached pre-resume neutral armed");
}

void accepted_drag_pause()
{
    Fixture f;
    f.drag();
    f.tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "held A repeated");
    f.live.pause(f.fake.time);
    check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "pause failed owned cleanup");
    f.live.enable(f.fake.time);
    f.tick();
    check(!f.live.armed(), "paused A replayed");
    f.neutral_then_press();
    f.live.pause(f.fake.time);
    check(f.fake.count(OutputKind::PrimaryUp) == 2, "second cancellation did not reset cleanup budget");
}

void normal_release()
{
    Fixture f;
    f.drag();
    f.pad().buttons = 0;
    f.tick();
    check(!f.live.owned() && f.live.armed(), "ordinary release disarmed or retained ownership");
    f.tick();
    check(f.fake.count(OutputKind::PrimaryUp) == 1, "repeated up released unowned state");
}

void physical_takeover()
{
    Fixture f;
    f.drag();
    ++f.fake.observation.activity;
    f.tick();
    check(!f.live.owned() && !f.live.armed(), "physical takeover failed cleanup");
    f.tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "held A replay after physical takeover");
    f.neutral_then_press();
    check(f.fake.count(OutputKind::PrimaryDown) == 2, "fresh post-takeover A failed");
}

void physical_overlap_and_cleanup_budget()
{
    Fixture f;
    f.drag();
    f.fake.observation.physical_held = Primary;
    f.live.invalidate(f.fake.time, L"Physical takeover");
    check(f.live.owned() == Primary && f.live.faulted(), "physical overlap must retain faulted ledger");
    for (unsigned i = 0; i < 10; ++i) { f.tick(250); }
    check(f.fake.count(OutputKind::PrimaryUp) == 0, "released physical overlap");
    f.fake.observation.physical_held = 0;
    f.tick(250);
    check(f.live.owned() == Primary, "unbounded automatic cleanup retries");
    f.live.recover(f.fake.time);
    check(!f.live.owned() && !f.live.faulted() && !f.live.enabled(), "explicit safe recovery failed");
}

void context_and_geometry()
{
    Fixture f;
    f.drag();
    ++f.fake.observation.context;
    f.fake.observation.allowed = false;
    f.tick();
    check(!f.live.owned(), "target-blocking gate incorrectly blocked cleanup");
    f.fake.observation.allowed = true;
    f.tick();
    check(!f.live.armed(), "held A replayed in changed context");
    f.neutral_then_press();
    --f.fake.observation.bounds.right;
    f.tick();
    check(!f.live.owned() && !f.live.armed(), "geometry change failed disarm");
}

void stale_and_scheduling()
{
    Fixture f;
    f.drag();
    f.fake.time += 101;
    f.live.step(f.snapshot, f.fake.time);
    check(!f.live.owned() && !f.live.armed(), "stale sample did not cancel");
    f.pad().buttons = 0;
    f.tick();
    check(!f.live.armed(), "sample gap must be a boundary");
    f.tick();
    check(f.live.armed(), "fresh neutral after gap failed");
    --f.snapshot.sequence;
    f.live.step(f.snapshot, f.fake.time);
    check(!f.live.armed(), "out of order sample accepted");
}

void selection_and_reconnect()
{
    Fixture f;
    f.drag();
    ++f.pad().connection_epoch;
    f.tick();
    check(!f.live.enabled() && !f.live.owned(), "reconnect failed explicit disarm");
    f.live.enable(f.fake.time);
    check(!f.live.enabled(), "enable reconfirmed changed slot");
    f.live.select(1, f.snapshot, f.fake.time);
    f.live.enable(f.fake.time);
    f.tick();
    check(f.live.armed() && f.fake.count(OutputKind::PrimaryDown) == 1, "slot failover/replay");
    f.live.select(std::nullopt, f.snapshot, f.fake.time);
    check(!f.live.enabled(), "deselection did not disable");
}

void disconnect_error()
{
    for (const auto connection : {Connection::Disconnected, Connection::Error, Connection::Unknown}) {
        Fixture f;
        f.drag();
        f.pad().connection = connection;
        f.tick();
        check(!f.live.enabled() && !f.live.owned(), "connection loss failed cleanup");
    }
}

void tap_edges_and_mapping_isolation()
{
    Fixture f;
    f.arm();
    f.pad().buttons = 0x4000;
    f.tick();
    check(f.fake.count(OutputKind::SecondaryDown) == 1 && f.fake.count(OutputKind::SecondaryUp) == 1,
          "secondary tap not balanced");
    f.tick();
    check(f.fake.count(OutputKind::SecondaryDown) == 1, "held X repeated");
    f.pad().buttons = 0;
    f.tick();
    f.pad().buttons = 0x2000;
    f.tick();
    check(f.fake.count(OutputKind::EscapeDown) == 1 && f.fake.count(OutputKind::EscapeUp) == 1,
          "Escape tap not balanced");
    f.pad().buttons = 0;
    f.tick();
    f.pad().buttons = a_button | 0x6000;
    f.pad().right_y = 32767;
    f.tick();
    check(f.fake.count(OutputKind::SecondaryDown) == 1 && f.fake.count(OutputKind::EscapeDown) == 1
          && f.fake.count(OutputKind::Wheel) == 0, "drag leaked secondary/Escape/scroll mappings");
    f.pad().buttons = 0x6000;
    f.tick();
    check(f.fake.count(OutputKind::SecondaryDown) == 1 && f.fake.count(OutputKind::EscapeDown) == 1,
          "ignored edges replayed after drag");
}

void menu_mapping()
{
    Fixture f;
    f.fake.observation.mode = Mode::Menu;
    f.arm();
    f.pad().buttons = a_button | 0x4000;
    f.pad().right_y = 32767;
    f.tick();
    check(f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryDown) == 1
          && f.fake.count(OutputKind::PrimaryUp) == 0, "menu must use ordinary mouse down/hold/up");
    check(f.fake.count(OutputKind::Wheel) == 0 && f.fake.count(OutputKind::SecondaryDown) == 0,
          "primary hold leaked other mappings");
    f.fake.observation.mode = Mode::Pointer;
    ++f.fake.observation.routing;
    f.tick();
    check(f.live.armed() && f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryDown) == 1,
          "menu to pointer transition cancelled hold or replayed its edge");
    f.pad().buttons = 0;
    f.tick();
    check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "menu selection release lost");
}

void chord_and_guide()
{
    Fixture f;
    f.arm();
    f.pad().buttons = 0x0400;
    f.tick();
    check(f.fake.accepted.empty(), "Guide was mapped");
    f.pad().buttons = 0x0030 | a_button | 0x6000;
    f.tick();
    check(!f.live.enabled() && f.fake.accepted.empty(), "pause chord leaked another mapping");
}

void paused_context_chord_and_reconnect()
{
    Fixture f;
    f.arm();
    f.fake.observation.allowed = false;
    f.tick();
    f.pad().buttons = 0x0030 | a_button;
    f.tick();
    check(!f.live.enabled() && f.fake.accepted.empty(), "blocked-context chord failed pause-only behavior");
    f.pad().buttons = 0;
    f.live.enable(f.fake.time);
    ++f.pad().connection_epoch;
    f.tick();
    check(!f.live.enabled(), "blocked context hid observed reconnection");
    f.live.enable(f.fake.time);
    check(!f.live.enabled(), "reconnection not requiring selection reconfirmation");
}

void motion_dispatch_and_virtual_origin()
{
    Fixture f;
    f.fake.observation.cursor = {-1000, -500};
    f.arm();
    f.pad().left_x = 32767;
    f.pad().left_y = 32767;
    f.pad().right_y = 32767;
    f.tick();
    check(f.fake.count(OutputKind::Move) == 1 && f.fake.count(OutputKind::Wheel) == 1, "live math not wired to dispatcher");
    check(f.fake.observation.cursor.x > -1000 && f.fake.observation.cursor.y < -500, "live movement signs/origin");
    const auto before = f.fake.attempted.size();
    f.pad().left_x = f.pad().left_y = f.pad().right_y = 0;
    f.tick();
    f.tick();
    check(f.fake.attempted.size() == before, "neutral drift or replay");
}

void failed_down()
{
    Fixture f;
    f.arm();
    f.fake.returns = {0};
    f.pad().buttons = a_button;
    f.tick();
    check(!f.live.owned() && f.live.faulted(), "rejected down was incorrectly owned");
    f.tick();
    check(f.fake.attempted.size() == 1, "rejected action was replayed");
}

void partial_tap_and_failed_cleanup()
{
    Fixture f;
    f.arm();
    f.fake.returns = {1, 0, 0, 0, 0};
    f.pad().buttons = 0x4000;
    f.tick();
    check(f.live.owned() == Secondary && f.live.faulted(), "partly accepted tap lost down ledger");
    check(f.live.status().find(L"FAULT") != std::wstring_view::npos, "failed tap release did not show fault");
    for (unsigned i = 0; i < 10; ++i) { f.tick(250); }
    check(f.fake.attempted.size() == 5, "cleanup must be limited to three extra single-event attempts");
    check(f.fake.count(OutputKind::SecondaryDown) == 1, "partial tap down replayed");
    f.live.recover(f.fake.time);
    check(!f.live.owned() && !f.live.faulted(), "manual release retry failed");
}

void cleanup_desktop_loss()
{
    Fixture f;
    f.drag();
    f.fake.observation.allowed = false;
    f.fake.observation.cleanup_allowed = false;
    f.fake.observation.baseline_known = false;
    f.live.pause(f.fake.time);
    check(f.live.owned() == Primary, "unavailable desktop zero state treated as release evidence");
    f.live.enable(f.fake.time);
    check(!f.live.enabled(), "unresolved cleanup allowed enable");
    f.fake.observation.cleanup_allowed = f.fake.observation.baseline_known = true;
    f.live.recover(f.fake.time);
    check(!f.live.owned(), "cleanup wrongly requires target eligibility");
}

void final_recipient_and_context()
{
    Fixture f;
    f.arm();
    f.fake.hit = false;
    f.pad().buttons = a_button;
    f.tick();
    check(f.fake.attempted.empty() && !f.live.armed(), "invalid recipient received click");
    f.fake.hit = true;
    f.pad().buttons = 0;
    f.tick();
    f.fake.during_hit = [&] { ++f.fake.observation.context; };
    f.pad().buttons = a_button;
    f.tick();
    check(f.fake.attempted.empty(), "context changed in hit test was not caught");
}

void expired_during_validation()
{
    Fixture f;
    f.arm();
    f.fake.during_hit = [&] { f.fake.time += 101; };
    f.pad().buttons = a_button;
    f.tick();
    check(f.fake.attempted.empty(), "slow observer allowed stale output");
}

void unavailable_observers()
{
    Fixture f;
    f.live.select(0, f.snapshot, f.fake.time);
    f.fake.observation.available = false;
    f.live.enable(f.fake.time);
    check(!f.live.enabled(), "missing required observer allowed enable");
}

void impossible_acceptance()
{
    Fake fake;
    Dispatcher dispatcher(fake);
    fake.returns = {2};
    check(dispatcher.emit({OutputKind::PrimaryDown}, fake.observation) == Emission::Blocked, "invalid acceptance succeeded");
    check(dispatcher.uncertain() && dispatcher.faulted() && !dispatcher.owned(),
          "uncertain output falsely counted as accepted ownership");
    dispatcher.recover(fake.time);
    check(dispatcher.faulted(), "impossible acceptance silently recovered");
}

void cross_window_drag_and_activation()
{
    Fixture f;
    f.arm();
    f.fake.after_send = [&](const Output& output) {
        if (output.kind == OutputKind::PrimaryDown) {
            f.fake.foreground = L"explorer.exe";
            f.fake.keyboard = L"explorer.exe";
            f.fake.capture = L"explorer.exe";
            ++f.fake.observation.routing;
        }
    };
    f.pad().buttons = a_button;
    f.pad().left_x = 32767;
    f.tick();
    for (unsigned i = 0; i < 4; ++i) {
        ++f.fake.observation.routing; // Activation/capture, then title drag/resize geometry.
        f.tick();
    }
    check(f.live.armed() && f.live.owned() == Primary, "ordinary activation/drag geometry disarmed");
    check(f.fake.count(OutputKind::PrimaryDown) == 1 && f.fake.count(OutputKind::PrimaryUp) == 0,
          "activation cancelled/replayed the accepted primary hold");
    check(f.fake.count(OutputKind::Move) == 5, "cross-window motion stopped");
    f.pad().buttons = 0;
    f.tick();
    check(f.live.armed() && !f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1,
          "cross-window drag did not finish with exactly one release");
}

void menus_submenus_without_neutral()
{
    Fixture f;
    f.arm();
    f.fake.after_send = [&](const Output& output) {
        if (output.kind == OutputKind::SecondaryUp) {
            f.fake.observation.mode = Mode::Menu;
            f.fake.capture = L"editor.exe";
            ++f.fake.observation.routing;
        } else if (output.kind == OutputKind::EscapeUp) {
            f.fake.observation.mode = Mode::Pointer;
            f.fake.capture.clear();
            ++f.fake.observation.routing;
        }
    };
    f.pad().buttons = 0x4000;
    f.pad().left_x = 32767;
    f.tick();
    for (unsigned i = 0; i < 5; ++i) {
        ++f.fake.observation.routing; // Hover opens/closes additional standard popup HWNDs.
        f.tick();
    }
    check(f.live.armed() && f.fake.count(OutputKind::Move) == 6, "menu/submenu stranded motion pending neutral");
    check(f.fake.count(OutputKind::SecondaryDown) == 1, "held X reopened a menu");
    f.pad().buttons = 0x2000; // Never center the stick during this interaction.
    f.tick();
    f.tick();
    check(f.live.armed() && f.fake.count(OutputKind::EscapeDown) == 1, "menu dismissal rearmed/replayed Escape");
    f.pad().buttons = a_button;
    f.tick();
    f.pad().buttons = 0;
    f.tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1 && f.fake.count(OutputKind::PrimaryUp) == 1,
          "post-menu click required neutral or lost its release");
}

void safe_routing_during_final_validation()
{
    Fixture f;
    f.arm();
    unsigned reads = 0;
    f.fake.during_observe = [&](const Output* intent) {
        if (intent && ++reads == 2) { ++f.fake.observation.routing; }
    };
    f.pad().buttons = a_button;
    f.tick();
    check(f.live.armed() && f.fake.accepted.empty(), "benign final revision injected stale event or disarmed");
    f.fake.during_observe = {};
    f.tick();
    check(f.fake.accepted.empty(), "deferred primary edge was replayed while held");
    f.pad().buttons = 0;
    f.tick();
    f.pad().buttons = a_button;
    f.tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "fresh edge after deferred routing failed");
}

void transient_routing_with_owned_drag()
{
    Fixture f;
    f.drag();
    f.fake.observation.routing_stable = false;
    f.pad().buttons = 0;
    f.tick();
    check(f.live.armed() && f.live.owned() == Primary && !f.live.faulted(), "transient routing cancelled drag");
    f.fake.observation.routing_stable = true;
    ++f.fake.observation.routing;
    f.tick();
    check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "deferred primary release was lost");
}

void routing_deferral_preserves_stick_hysteresis()
{
    for (bool configured : {false, true}) {
        for (bool dragging : {false, true}) {
            Fixture f;
            PointerSettings settings;
            if (configured) { settings.left_enter = settings.right_enter = 0.50; }
            check(f.live.set_pointer_settings(settings, f.fake.time).empty(), "valid settings rejected");
            f.arm();
            f.pad().buttons = dragging ? a_button : 0;
            f.pad().left_x = f.pad().right_y = 32767;
            f.tick();
            const auto left_band = static_cast<std::int16_t>(
                (settings.left_exit + settings.left_enter) * 0.5 * 32767);
            const auto right_band = static_cast<std::int16_t>(
                (settings.right_exit + settings.right_enter) * 0.5 * 32767);
            f.pad().left_x = left_band;
            f.pad().right_y = right_band;
            f.tick();
            const auto position = f.fake.observation.cursor;
            const auto wheels = f.fake.count(OutputKind::Wheel);
            const auto sent = f.fake.accepted.size();
            f.fake.observation.routing_stable = false;
            f.tick();
            check(f.fake.accepted.size() == sent && f.live.armed(), "transient route emitted or disarmed");
            f.fake.observation.routing_stable = true;
            for (unsigned i = 0; i < 240; ++i) { f.tick(); }
            check(f.fake.observation.cursor.x > position.x, "eligible route reset active pointer hysteresis");
            check(dragging ? f.live.owned() == Primary : f.fake.count(OutputKind::Wheel) > wheels,
                  "eligible route lost drag ownership or active scroll hysteresis");
            check(f.fake.count(OutputKind::PrimaryDown) == (dragging ? 1u : 0u),
                  "route replayed the held activation edge");

            f.fake.observation.routing_stable = false;
            f.pad().left_x = f.pad().right_y = 0;
            f.tick();
            f.fake.observation.routing_stable = true;
            f.pad().left_x = left_band;
            f.pad().right_y = right_band;
            const auto after_center = f.fake.accepted.size();
            for (unsigned i = 0; i < 240; ++i) { f.tick(); }
            check(f.fake.accepted.size() == after_center,
                  "centering during deferral failed to require re-entry threshold");
        }
    }
}

void deferred_tap_release()
{
    for (const auto button : {0x4000, 0x2000}) {
        Fixture f;
        f.arm();
        f.fake.after_send = [&](const Output& output) {
            if (output.kind == OutputKind::SecondaryDown || output.kind == OutputKind::EscapeDown) {
                f.fake.observation.routing_stable = false;
            }
        };
        f.pad().buttons = static_cast<std::uint16_t>(button);
        f.tick();
        check(f.live.owned() && f.live.armed() && !f.live.faulted(), "tap deferral lost accepted down");
        f.fake.observation.routing_stable = true;
        f.tick();
        check(!f.live.owned() && f.fake.accepted.size() == 2 && !f.live.faulted(),
              "tap's deferred up was not completed without replaying its down");
    }
}

void final_move_destination_and_protected_transition()
{
    Fixture f;
    f.arm();
    f.pad().left_x = 32767;
    f.fake.during_observe = [&](const Output* intent) {
        if (intent && intent->kind == OutputKind::Move && intent->point.x > 0) { f.fake.protected_mouse = true; }
    };
    f.tick();
    check(f.fake.accepted.empty() && !f.live.armed(), "motion destination bypassed protected-recipient check");
    f.fake.during_observe = {};
    f.fake.protected_mouse = false;
    f.tick();
    check(!f.live.armed(), "unsafe transition resumed with held stick");
    f.pad().left_x = 0;
    f.tick();
    check(f.live.armed(), "unsafe transition did not recover on new neutral");
}

void exclusion_validation()
{
    check(validate_exclusions({}).empty(), "empty explicit exclusion list rejected");
    const ExecutableExclusions names{L"Game.exe", L"My Game (DX12).EXE", L"\u00c4pp.exe"};
    check(validate_exclusions(names).empty(), "valid executable basenames rejected");
    check(executable_excluded(L"gAME.ExE", names), "exclusions are not case-insensitive");
    check(executable_excluded(L"\u00e4pp.EXE", names), "ordinal Unicode casing lost");
    check(!executable_excluded(L"GameHelper.exe", names), "exclusions used substring/wildcard matching");
    for (const auto name : {L"", L"exe", L".exe", L"Game.dll", L"*.exe", L"foo?.exe", L"C:\\Game.exe",
                            L"dir/Game.exe", L" Game.exe", L"Game.exe ", L"Game\n.exe", L"Game:.exe"}) {
        check(!validate_exclusions({name}).empty(), "invalid exclusion was accepted");
    }
    check(!validate_exclusions(ExecutableExclusions(129, L"a.exe")).empty(), "exclusion count was unbounded");
    check(!validate_exclusions({std::wstring(260, L'a') + L".exe"}).empty(), "executable name was unbounded");
}

void exclusion_apply_and_recipients()
{
    Fixture f;
    f.drag();
    check(f.live.set_exclusions({L"Game.exe"}, f.fake.time).empty(), "valid exclusion apply failed");
    check(!f.live.enabled() && !f.live.owned() && !f.live.controller_active(), "exclusion apply retained live hold");
    check(!f.live.set_exclusions({L"*.exe"}, f.fake.time).empty()
          && f.live.exclusions() == ExecutableExclusions{L"Game.exe"}, "invalid list replaced last valid exclusions");
    f.pad().buttons = 0;
    f.live.enable(f.fake.time);
    f.tick();
    f.fake.foreground = L"gAME.EXE";
    f.pad().left_x = 32767;
    f.tick();
    check(!f.live.armed() && !f.live.controller_active(), "excluded foreground allowed controller ownership");
    f.fake.foreground = L"editor.exe";
    f.pad().left_x = 0;
    f.tick();
    f.fake.mouse = L"GAME.exe";
    f.pad().buttons = a_button;
    f.tick();
    check(!f.live.armed() && f.fake.count(OutputKind::PrimaryDown) == 1, "excluded inactive point recipient got click");
    f.fake.mouse = L"explorer.exe";
    f.pad().buttons = 0;
    f.tick();
    f.fake.keyboard = L"game.exe";
    f.pad().buttons = 0x2000;
    f.tick();
    check(f.fake.count(OutputKind::EscapeDown) == 0, "excluded actual key recipient got Escape");
    f.fake.keyboard = L"editor.exe";
    f.pad().buttons = 0;
    f.tick();
    f.pad().buttons = a_button;
    f.tick();
    f.fake.capture = L"game.exe";
    f.tick();
    check(!f.live.armed() && !f.live.owned(), "excluded capture failed disarm/owned cleanup");
}

void exclusions_checked_after_motion()
{
    Fixture f;
    check(f.live.set_exclusions({L"game.exe"}, f.fake.time).empty(), "exclusion apply failed");
    f.arm();
    f.fake.after_send = [&](const Output& output) {
        if (output.kind == OutputKind::Move) { f.fake.mouse = L"game.exe"; ++f.fake.observation.routing; }
    };
    f.pad().left_x = 32767;
    f.pad().buttons = a_button;
    f.tick();
    check(f.fake.count(OutputKind::Move) == 1 && f.fake.count(OutputKind::PrimaryDown) == 0
          && !f.live.controller_active(), "post-motion click used pre-move recipient eligibility");
}

void protected_drag_cleanup_fault()
{
    Fixture f;
    f.drag();
    f.fake.protected_mouse = true;
    f.tick();
    check(f.live.owned() == Primary && !f.live.controller_active()
          && f.fake.count(OutputKind::PrimaryUp) == 0, "protected recipient received a release");
    f.tick(250); f.tick(250);
    check(f.live.faulted() && f.live.owned() == Primary && !f.live.controller_active(),
          "protected recipient falsely accepted cleanup or retained visible ownership");
    f.fake.protected_mouse = false;
    f.live.recover(f.fake.time);
    check(!f.live.enabled() && !f.live.owned() && !f.live.faulted(), "safe explicit cleanup failed");
}

void default_settings_equivalence()
{
    const PointerSettings defaults;
    check(defaults.speed == 1100 && defaults.precision == 0.25 && defaults.scroll == 960
          && defaults.left_exit == 0.20 && defaults.left_enter == 0.24
          && defaults.right_exit == 0.23 && defaults.right_enter == 0.27
          && defaults.profile == MotionProfile::Smooth, "praised defaults changed");
    check(validate_pointer_settings(defaults).empty(), "defaults invalid");
    Pointer implicit, explicit_defaults;
    implicit.reset(1000);
    explicit_defaults.reset(1000);
    Point a{}, b{};
    Controller pad;
    for (Tick now = 1016; now < 5000; now += 16) {
        pad.left_x = static_cast<std::int16_t>((now * 57) % 65536 - 32768);
        pad.left_y = static_cast<std::int16_t>((now * 79) % 65536 - 32768);
        pad.right_y = static_cast<std::int16_t>((now * 31) % 65536 - 32768);
        pad.buttons = now % 3 ? 0 : 0x0100;
        const auto first = implicit.step(pad, a, {-5000, -5000, 5000, 5000}, now, true);
        const auto second = explicit_defaults.step(pad, b, {-5000, -5000, 5000, 5000}, now, true, defaults);
        check(first.point == second.point && first.wheel == second.wheel, "default helper and settings math differ");
        a = first.point;
        b = second.point;
    }
}

void settings_validation_and_atomic_apply()
{
    Fixture f;
    f.drag();
    const auto original = f.live.pointer_settings();
    const auto reason = f.live.status();
    double PointerSettings::* fields[] = {&PointerSettings::speed, &PointerSettings::precision,
        &PointerSettings::scroll, &PointerSettings::left_exit, &PointerSettings::left_enter,
        &PointerSettings::right_exit, &PointerSettings::right_enter};
    // Nonfinite rejection for every field, preserving active state as well as settings.
    for (auto member : fields) {
        for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity()}) {
            auto invalid = original;
            invalid.profile = static_cast<MotionProfile>(99);
            check(!f.live.set_pointer_settings(invalid, f.fake.time).empty() && f.live.pointer_settings() == original
                && f.live.enabled() && f.live.owned() == Primary, "invalid profile changed live state");
            invalid = original;
            invalid.*member = value;
            check(!f.live.set_pointer_settings(invalid, f.fake.time).empty(), "nonfinite setting accepted");
            check(f.live.pointer_settings() == original && f.live.enabled()
                  && f.live.owned() == Primary && f.live.controller_active(),
                  "invalid setting changed settings or live ownership");
        }
        for (const double value : {-1.0, 10000.0}) {
            auto invalid = original;
            invalid.*member = value;
            check(!f.live.set_pointer_settings(invalid, f.fake.time).empty(), "out-of-range setting accepted");
        }
    }
    auto invalid = original;
    invalid.left_enter = invalid.left_exit;
    check(!f.live.set_pointer_settings(invalid, f.fake.time).empty(), "equal left deadzones accepted");
    invalid = original;
    invalid.right_exit = invalid.right_enter + 0.01;
    check(!f.live.set_pointer_settings(invalid, f.fake.time).empty(), "inverted right deadzones accepted");
    auto settings = original;
    settings.speed = 2200;
    settings.precision = 0.5;
    settings.left_exit = 0.30;
    settings.left_enter = 0.34;
    check(f.live.status() == reason, "validation changed status");
    check(f.live.set_pointer_settings(settings, f.fake.time).empty(), "valid settings apply rejected");
    check(!f.live.enabled() && !f.live.owned() && !f.live.controller_active(), "settings apply failed pause/cleanup");
    check(f.live.pointer_settings() == settings, "valid settings not retained");
    f.live.enable(f.fake.time);
    f.tick();
    check(!f.live.armed(), "settings re-enable replayed held A");
    f.pad().buttons = 0;
    f.pad().left_x = 9000;
    f.tick();
    check(f.live.armed(), "custom neutral threshold not used");
    f.pad().left_x = 32767;
    f.tick();
    check(f.fake.observation.cursor.x == 35, "configured speed not used by live pointer");
    check(f.live.set_pointer_settings({}, f.fake.time).empty()
          && f.live.pointer_settings() == original, "restore defaults failed");
}

void controller_ownership_visibility()
{
    Fixture f;
    check(!f.live.controller_active(), "startup claims controller use");
    f.arm();
    f.tick();
    check(!f.live.controller_active(), "eligibility/neutral claims controller use");
    f.pad().buttons = 0x0100;
    f.tick();
    check(!f.live.controller_active(), "unmapped precision-only input claims use");
    f.pad().left_x = 32767;
    f.tick();
    check(f.live.controller_active(), "accepted motion did not claim use");
    f.pad().buttons = 0;
    f.pad().left_x = 0;
    ++f.fake.observation.routing;
    f.tick();
    check(f.live.controller_active(), "ordinary idle/focus hid controller use");
    ++f.fake.observation.activity;
    f.tick();
    check(!f.live.controller_active(), "physical takeover did not hide controller use");
    f.tick();
    check(f.live.armed() && !f.live.controller_active(), "neutral recovery alone showed controller use");
    f.pad().right_y = 32767;
    f.tick();
    check(f.live.controller_active(), "accepted scroll did not claim use");
    f.live.pause(f.fake.time);
    check(!f.live.controller_active(), "pause/exit retained controller use");
}

void settings_cleanup_failure_and_limits()
{
    check(validate_pointer_settings({100, 0.05, 120, 0, 0.01, 0, 0.01}).empty(), "valid lower limits rejected");
    check(validate_pointer_settings({5000, 1, 4800, 0.90, 0.95, 0.90, 0.95}).empty(), "valid upper limits rejected");
    Fixture f;
    f.drag();
    f.fake.observation.physical_held = Primary;
    auto settings = f.live.pointer_settings();
    settings.speed = 1500;
    check(f.live.set_pointer_settings(settings, f.fake.time).empty(), "valid setting not accepted");
    check(!f.live.enabled() && !f.live.controller_active() && f.live.owned() == Primary && f.live.faulted(),
          "settings apply hid failed overlapping cleanup");
    f.live.enable(f.fake.time);
    check(!f.live.enabled(), "settings bypassed cleanup fault");
}

void final_exclusion_and_takeover()
{
    for (bool physical : {false, true}) {
        Fixture f;
        check(f.live.set_exclusions({L"game.exe"}, f.fake.time).empty(), "exclusion apply failed");
        f.arm();
        unsigned reads = 0;
        f.fake.during_observe = [&](const Output* intent) {
            if (intent && ++reads == 2) {
                if (physical) { ++f.fake.observation.activity; }
                else { f.fake.mouse = L"game.exe"; }
            }
        };
        f.pad().buttons = a_button;
        f.tick();
        check(f.fake.accepted.empty() && !f.live.armed() && !f.live.controller_active(),
              "final exclusion/physical change escaped per-event validation");
    }
}

void configured_pointer_math()
{
    const PointerSettings settings{2000, 0.5, 1200, 0.30, 0.35, 0.40, 0.45};
    Pointer pointer;
    pointer.reset(1000);
    Controller pad;
    pad.left_x = pad.right_y = 32767;
    pad.buttons = 0x0100;
    Point position{};
    int wheel = 0;
    for (Tick now = 1010; now <= 2000; now += 10) {
        const auto motion = pointer.step(pad, position, {-5000, -5000, 5000, 5000}, now, true, settings);
        position = motion.point;
        wheel += motion.wheel;
    }
    near(position.x, 1000, 1);
    near(wheel, 1200, 1);
    pad = {};
    pad.left_x = 11000; // Above exit, below enter: only previously active sticks move.
    pad.right_y = 14000;
    position = {};
    wheel = 0;
    for (Tick now = 2010; now <= 3000; now += 10) {
        const auto motion = pointer.step(pad, position, {-5000, -5000, 5000, 5000}, now, true, settings);
        position = motion.point;
        wheel += motion.wheel;
    }
    check(position.x > 0 && wheel > 0, "custom active hysteresis stopped above exit");
    pointer.reset(3000);
    for (Tick now = 3010; now <= 4000; now += 10) {
        const auto motion = pointer.step(pad, {}, {-5000, -5000, 5000, 5000}, now, true, settings);
        check(motion.point == Point{} && motion.wheel == 0, "custom entry deadzones ignored");
    }
}

void expired_neutral_observation()
{
    Fixture f;
    f.live.select(0, f.snapshot, f.fake.time);
    f.live.enable(f.fake.time);
    f.fake.during_observe = [&](const Output*) { f.fake.time += 101; };
    f.tick();
    check(!f.live.armed() && !f.live.controller_active() && f.fake.accepted.empty(),
          "expired neutral observation armed or claimed ownership");
}

void persistent_unstable_release_is_bounded()
{
    Fixture f;
    f.drag();
    f.pad().buttons = 0;
    f.fake.observation.routing_stable = false;
    for (unsigned i = 0; i < 9; ++i) { f.tick(); }
    check(!f.live.armed() && !f.live.faulted() && f.live.owned() == Primary, "initial deferred cleanup should await recovery");
    f.tick(250); f.tick(250);
    check(!f.live.armed() && !f.live.controller_active() && f.live.owned() == Primary && f.live.faulted(),
          "unstable routing retained an unbounded unfaulted hold");
    f.fake.observation.routing_stable = true;
    f.live.recover(f.fake.time);
    check(!f.live.owned() && !f.live.enabled() && !f.live.faulted(), "bounded routing failure could not recover");
}

std::int16_t band_axis(double active, const PointerSettings& settings)
{
    return static_cast<std::int16_t>(std::lround((settings.left_enter + active * (1 - settings.left_enter)) * 32767));
}

void smooth_original_math()
{
    for (const auto& settings : {PointerSettings{}, PointerSettings{1234.567, .31, 817, .1, .6, .8, .9}}) {
        Pointer pointer; pointer.reset(1000);
        bool left_active = false, right_active = false;
        double fx = 0, fy = 0, fw = 0;
        Point position{};
        Tick last = 1000;
        // Independent pre-checkpoint math, including exit rescaling, fractions and scroll.
        const auto original = [](std::int16_t x, std::int16_t y, double enter, double leave, bool& active) {
            const double radius = std::hypot(static_cast<double>(x), static_cast<double>(y)) / 32767.0;
            active = active ? radius > leave : radius > enter;
            if (!active) { return Vector{}; }
            const double strength = ((std::min)(radius, 1.0) - leave) / (1.0 - leave);
            const double scale = strength * strength / (radius * 32767.0);
            return Vector{x * scale, y * scale};
        };
        for (int i = 0; i < 1000; ++i) {
            Controller pad;
            pad.left_x = static_cast<std::int16_t>((i * 541) % 65536 - 32768);
            pad.left_y = static_cast<std::int16_t>((i * 317) % 65536 - 32768);
            pad.right_x = static_cast<std::int16_t>((i * 97) % 65536 - 32768);
            pad.right_y = static_cast<std::int16_t>((i * 211) % 65536 - 32768);
            if (i % 9 == 0) { pad = {}; }
            pad.buttons = i % 3 ? 0 : 0x0100;
            const Tick now = last + static_cast<Tick>(i % 43);
            const auto left = original(pad.left_x, pad.left_y, settings.left_enter, settings.left_exit, left_active);
            const auto right = original(pad.right_x, pad.right_y, settings.right_enter, settings.right_exit, right_active);
            const double dt = static_cast<double>((std::min)(now - last, integration_cap_ms)) / 1000.0;
            const double speed = settings.speed * (pad.buttons ? settings.precision : 1.0);
            if (!left_active) { fx = fy = 0; }
            fx += left.x * speed * dt; fy -= left.y * speed * dt;
            const Point expected{position.x + static_cast<int>(fx), position.y + static_cast<int>(fy)};
            fx -= static_cast<int>(fx); fy -= static_cast<int>(fy);
            const bool scroll = i % 4 != 0;
            if (!scroll || !right_active) { fw = 0; }
            if (scroll) { fw += right.y * settings.scroll * dt; }
            const int wheel = static_cast<int>(fw); fw -= wheel;
            const auto actual = pointer.step(pad, position, {-30000, -30000, 30000, 30000}, now, scroll, settings);
            check(actual.point == expected && actual.wheel == wheel, "Smooth differs from original math");
            position = actual.point; last = now;
        }
    }
}

void speed_band_plateaus()
{
    for (const auto zones : {std::pair{.20, .24}, std::pair{.0, .95}, std::pair{.90, .95}}) {
        PointerSettings settings; settings.profile = MotionProfile::SpeedBands;
        settings.left_exit = zones.first; settings.left_enter = zones.second;
        check(validate_pointer_settings(settings).empty(), "legal unusual deadzones");
        for (const auto sample : {std::pair{.01, .10}, std::pair{.15, .10}, std::pair{.34, .10},
                                 std::pair{.36, .35}, std::pair{.50, .35}, std::pair{.71, .35}, std::pair{.89, .35},
                                 std::pair{.91, 1.0}, std::pair{.98, 1.0}, std::pair{1.0, 1.0}}) {
            for (bool precision : {false, true}) {
                for (Tick interval : {10ull, 20ull}) {
                    Pointer pointer; pointer.reset(0);
                    Controller pad; pad.left_x = band_axis(sample.first, settings);
                    pad.buttons = precision ? 0x0100 : 0;
                    Point position{};
                    for (Tick now = interval; now <= 1000; now += interval) {
                        position = pointer.step(pad, position, {-5000, -5000, 5000, 5000}, now, true, settings).point;
                    }
                    near(position.x, settings.speed * sample.second * (precision ? settings.precision : 1), 1);
                    check(position.y == 0, "axial plateau drift");
                }
            }
        }
    }
}

void speed_band_hysteresis()
{
    PointerSettings settings; settings.profile = MotionProfile::SpeedBands; settings.speed = 1000;
    Pointer pointer; pointer.reset(0); Tick now = 0;
    Controller pad;
    const auto expect = [&](double active, int dx) {
        pad.left_x = band_axis(active, settings);
        now += 20;
        const auto motion = pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, now, true, settings);
        check(motion.point == Point{dx, 0}, "boundary hysteresis changed fixed velocity");
    };
    expect(.349, 2); expect(.351, 7);
    for (int i = 0; i < 25; ++i) { expect(.349, 7); expect(.351, 7); }
    expect(.301, 7); expect(.299, 2); expect(.349, 2);
    expect(.899, 7); expect(.901, 20);
    for (int i = 0; i < 25; ++i) { expect(.899, 20); expect(.901, 20); }
    expect(.851, 20); expect(.849, 7); expect(.899, 7); expect(.10, 2);
}

void configurable_and_merged_band_rates()
{
    for (const auto rates : {std::array{.35, .35, 1.0}, std::array{.10, .60, .60},
        std::array{.12, .40, .75}, std::array{.25, .25, .25}, std::array{.01, .01, .01}}) {
        for (bool precision : {false, true}) {
            PointerSettings settings; settings.profile = MotionProfile::SpeedBands;
            settings.speed = 1700; settings.band_rates = rates;
            check(validate_pointer_settings(settings).empty(), "valid merged/custom band rates rejected");
            Pointer pointer; pointer.reset(0); Tick now = 0;
            for (const auto sample : {std::pair{.15, 0u}, std::pair{.34, 0u}, std::pair{.36, 1u},
                std::pair{.89, 1u}, std::pair{.95, 2u}, std::pair{.86, 2u}, std::pair{.84, 1u}, std::pair{.29, 0u}}) {
                Controller pad; pad.left_x = band_axis(sample.first, settings); pad.buttons = precision ? 0x0100 : 0;
                Point position{};
                for (int tick = 0; tick < 50; ++tick) {
                    now += 20;
                    position = pointer.step(pad, position, {-5000, -5000, 5000, 5000}, now, true, settings).point;
                }
                near(position.x, settings.speed * rates[sample.second] * (precision ? settings.precision : 1), 1.1);
                check(position.y == 0, "custom axial rates drifted");
            }
            auto smooth = settings; smooth.profile = MotionProfile::Smooth;
            auto reference = smooth; reference.band_rates = PointerSettings{}.band_rates;
            Pointer a, b; a.reset(0); b.reset(0);
            Controller pad; pad.left_x = 25000; pad.right_y = 22000;
            check(a.step(pad, {}, {-5000, -5000, 5000, 5000}, 20, true, smooth).point
                == b.step(pad, {}, {-5000, -5000, 5000, 5000}, 20, true, reference).point,
                "band rates changed Smooth response");
        }
    }
}

void band_rate_validation_and_live_apply()
{
    Fixture f; f.drag();
    const auto before = f.live.pointer_settings();
    for (std::size_t band = 0; band < before.band_rates.size(); ++band) {
        for (double invalid : {0.0, -1.0, 1.01, std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
            auto settings = before; settings.band_rates[band] = invalid;
            check(!f.live.set_pointer_settings(settings, f.fake.time).empty()
                && f.live.pointer_settings() == before && f.live.owned() == Primary && f.live.armed(),
                "invalid rate changed live state");
        }
    }
    for (const auto rates : {std::array{.40, .30, 1.0}, std::array{.10, .90, .80}}) {
        auto settings = before; settings.band_rates = rates;
        check(!f.live.set_pointer_settings(settings, f.fake.time).empty(), "descending band speeds accepted");
    }
    auto settings = before; settings.band_rates = {.35, .35, .90}; settings.profile = MotionProfile::SpeedBands;
    check(f.live.set_pointer_settings(settings, f.fake.time).empty() && f.live.pointer_settings() == settings,
          "valid rates not applied");
    check(!f.live.owned() && !f.live.enabled(), "band Apply did not pause and release");
    f.live.enable(f.fake.time); f.tick();
    check(!f.live.armed(), "rate change replayed held A");
    f.pad().buttons = 0; f.tick(); f.pad().left_x = band_axis(.15, settings);
    const auto origin = f.fake.observation.cursor;
    f.tick(20);
    near(f.fake.observation.cursor.x - origin.x, settings.speed * .35 * .020, 1);
}

void speed_band_direction_scroll()
{
    PointerSettings settings; settings.profile = MotionProfile::SpeedBands;
    for (double active : {.15, .5, .95}) {
        for (double angle : {0.0, .45, 1.5, 2.3, 3.7, 4.8, 5.5}) {
            const double radius = settings.left_enter + active * (1 - settings.left_enter);
            Controller pad;
            pad.left_x = static_cast<std::int16_t>(std::lround(std::cos(angle) * radius * 32767));
            pad.left_y = static_cast<std::int16_t>(std::lround(std::sin(angle) * radius * 32767));
            pad.right_y = 24000;
            Pointer bands, smooth; bands.reset(0); smooth.reset(0);
            auto old = settings; old.profile = MotionProfile::Smooth;
            Point position{}; int wheel = 0, original_wheel = 0;
            for (Tick now = 10; now <= 1000; now += 10) {
                const auto motion = bands.step(pad, position, {-5000, -5000, 5000, 5000}, now, true, settings);
                position = motion.point; wheel += motion.wheel;
                original_wheel += smooth.step(pad, {}, {-5000, -5000, 5000, 5000}, now, true, old).wheel;
            }
            const double rate = active < .35 ? .1 : active < .9 ? .35 : 1;
            near(position.x, std::cos(angle) * settings.speed * rate, 1.1);
            near(position.y, -std::sin(angle) * settings.speed * rate, 1.1);
            near(std::hypot(position.x, position.y), settings.speed * rate, 1.5);
            check(wheel == original_wheel, "speed profile changed scroll");
        }
    }
    Pointer pointer; pointer.reset(0);
    Controller pad; pad.left_x = pad.left_y = -32768;
    const auto m = pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 32, true, settings);
    check(m.point.x < 0 && m.point.y > 0 && std::hypot(m.point.x, m.point.y) <= settings.speed * .032,
        "negative extreme diagonal exceeded cap or changed signs");
}

void speed_band_reset_gap_deferral()
{
    PointerSettings settings; settings.profile = MotionProfile::SpeedBands; settings.speed = 1000;
    Pointer pointer; pointer.reset(0);
    Controller pad; pad.left_x = 32767;
    pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 20, true, settings);
    pad.left_x = band_axis(.88, settings);
    pointer.skip_interval(pad, 40, settings);
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 60, true, settings).point.x == 20, "deferral lost high-band hysteresis");
    pointer.reset(60);
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 80, true, settings).point.x == 7, "reset retained old high band");
    pad.left_x = 32767;
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 170, true, settings).point.x == 32, "dt not capped");
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 271, true, settings).point == Point{}, "gap moved pointer");
    pad.left_x = band_axis(.88, settings);
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 291, true, settings).point.x == 7, "gap failed to reset high band");
    pad.left_x = 0; pointer.skip_interval(pad, 311, settings);
    pad.left_x = static_cast<std::int16_t>(.22 * 32767);
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 331, true, settings).point == Point{}, "centering during deferral ignored");
    pad.left_x = 32767; pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 351, true, settings);
    pad.left_x = static_cast<std::int16_t>(.22 * 32767);
    pointer.skip_interval(pad, 371, settings);
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 391, true, settings).point.x == 2, "entry/exit hysteresis stranded bands");
    check(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, 390, true, settings).point == Point{}, "backwards time");
}

void speed_band_live_boundaries()
{
    for (int transition = 0; transition < 5; ++transition) {
        Fixture f; PointerSettings settings; settings.profile = MotionProfile::SpeedBands; settings.speed = 1000;
        check(f.live.set_pointer_settings(settings, f.fake.time).empty(), "profile apply");
        f.arm(); f.pad().left_x = 32767; f.pad().buttons = a_button; f.tick(20);
        check(f.live.owned() == Primary && f.live.controller_active(), "band drag fixture");
        if (transition == 0) { f.live.pause(f.fake.time); }
        if (transition == 1) { ++f.fake.observation.activity; f.tick(); }
        if (transition == 2) { f.fake.observation.mode = Mode::Blocked; f.fake.observation.allowed = false; f.tick(); }
        if (transition == 3) { check(f.live.set_pointer_settings(settings, f.fake.time).empty(), "settings reset"); }
        if (transition == 4) { f.fake.time += 101; f.live.step(f.snapshot, f.fake.time); }
        check(!f.live.controller_active() && !f.live.armed() && !f.live.owned(), "boundary failed to pause/release");
        f.fake.observation.mode = Mode::Pointer; f.fake.observation.allowed = true;
        if (!f.live.enabled()) { f.live.enable(f.fake.time); }
        f.pad() = {}; f.pad().connection = Connection::Connected; f.pad().connection_epoch = 1; f.tick();
        if (transition == 4) { f.tick(); } // Stale input still requires a new baseline after the gap.
        check(f.live.armed(), "neutral rearm");
        f.pad().left_x = band_axis(.88, settings);
        const auto before = f.fake.observation.cursor; f.tick(20);
        check(f.fake.observation.cursor.x == before.x + 7 && f.fake.count(OutputKind::PrimaryDown) == 1,
            "boundary retained high band or replayed click");
    }
}

void speed_band_live_routing()
{
    Fixture f; PointerSettings settings; settings.profile = MotionProfile::SpeedBands; settings.speed = 1000;
    check(f.live.set_pointer_settings(settings, f.fake.time).empty(), "profile apply"); f.arm();
    f.pad().left_x = 32767; f.pad().buttons = a_button; f.tick(20);
    f.pad().left_x = band_axis(.88, settings); f.fake.observation.routing_stable = false;
    const auto before = f.fake.observation.cursor; f.tick(20);
    check(f.fake.observation.cursor == before && f.live.owned() == Primary, "deferred motion or lost hold");
    f.fake.observation.routing_stable = true; ++f.fake.observation.routing; f.fake.observation.mode = Mode::Menu; f.tick(20);
    check(f.fake.observation.cursor.x == before.x + 20 && f.fake.count(OutputKind::PrimaryDown) == 1, "routing changed band or replayed click");
    f.pad().buttons = 0; f.tick(20);
    check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "normal release lost");
}
} // namespace

void automatic_start_and_first_intent()
{
    Fixture f;
    f.automatic_tick();
    check(!f.live.selected_slot() && f.fake.accepted.empty(), "neutral startup should not select or emit");
    f.snapshot.slots[1].buttons = a_button;
    f.automatic_tick();
    check(f.live.selected_slot() == 1u && f.live.armed() && f.live.controller_active(),
          "first deliberate controller should activate without a UI action");
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "first A after observed neutral must not be lost");
    f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "held auto-activation repeated click");
    f.snapshot.slots[1].buttons = 0;
    f.automatic_tick();
    check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "normal automatic release");
}

void automatic_held_start_and_readiness()
{
    for (bool initial_ready : {false, true}) {
        Fixture f;
        f.pad().buttons = a_button;
        f.automatic_tick(initial_ready);
        f.automatic_tick();
        check(f.fake.accepted.empty(), "startup or unavailable-mode held button replayed");
        f.pad().buttons = 0;
        f.automatic_tick();
        f.pad().buttons = a_button;
        f.automatic_tick();
        check(f.fake.count(OutputKind::PrimaryDown) == 1, "neutral and fresh input failed to activate");
        f.automatic_tick(false);
        check(!f.live.owned() && !f.live.controller_active(), "lost prerequisites did not release/hide");
    }
}

void automatic_game_and_physical_handoff()
{
    for (bool physical : {false, true}) {
        Fixture f;
        f.automatic_tick();
        f.pad().buttons = a_button;
        f.automatic_tick();
        if (physical) {
            ++f.fake.observation.activity;
            f.fake.observation.physical_held = Other;
        } else {
            f.fake.observation.allowed = false;
            ++f.fake.observation.context;
        }
        f.automatic_tick();
        check(!f.live.owned() && !f.live.controller_active() && f.fake.count(OutputKind::PrimaryUp) == 1,
              "game/physical boundary retained output");
        f.fake.observation.allowed = true;
        f.fake.observation.physical_held = 0;
        f.automatic_tick();
        check(f.fake.count(OutputKind::PrimaryDown) == 1, "held input leaked after automatic resume");
        f.pad().buttons = 0;
        f.automatic_tick();
        f.pad().buttons = a_button;
        f.automatic_tick();
        check(f.fake.count(OutputKind::PrimaryDown) == 2, "fresh input did not resume automatically");
    }
}

void automatic_pause_is_sticky()
{
    Fixture f;
    f.automatic_tick();
    f.pad().buttons = a_button;
    f.automatic_tick();
    f.live.suspend_automatic(f.fake.time);
    check(f.live.automatically_paused() && !f.live.owned(), "manual pause not latched/cleaned");
    f.pad().buttons = 0; f.automatic_tick();
    check(f.live.set_pointer_settings({}, f.fake.time).empty(), "settings");
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "settings or automatic loop undid emergency pause");
    f.live.resume_automatic(f.fake.time);
    f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "resume replayed held A");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 2, "explicit resume failed after fresh neutral");
    f.pad().buttons = 0x0030 | a_button;
    f.automatic_tick();
    check(f.live.automatically_paused() && !f.live.owned(), "controller pause chord did not latch");
}

void manual_chord_toggles_once_and_never_replays_input()
{
    Fixture f; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    f.pad().buttons = 0x0030 | a_button | 0x6000; f.automatic_tick();
    check(f.live.automatically_paused() && !f.live.owned() && !f.live.controller_active(),
          "native mode retained a drag or custom cursor ownership");
    check(f.fake.count(OutputKind::PrimaryUp) == 1 && f.fake.count(OutputKind::SecondaryDown) == 0
        && f.fake.count(OutputKind::EscapeDown) == 0, "switching chord leaked simultaneous actions");
    for (int i = 0; i < 90; ++i) { f.automatic_tick(); }
    check(f.live.automatically_paused(), "holding the chord toggled repeatedly");
    f.pad().buttons = 0x0010; f.automatic_tick();
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused(), "partial release rearmed the toggle");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = 0x0030 | a_button; f.automatic_tick();
    check(!f.live.automatically_paused() && !f.live.enabled() && !f.live.controller_active(),
          "second press did not select desktop mode without immediate injection");
    f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "held A replayed on the return to desktop");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 2, "desktop did not resume after fresh neutral input");
}

void manual_toggle_does_not_override_games_or_prerequisites()
{
    Fixture f; f.automatic_tick();
    f.fake.observation.allowed = false;
    f.fake.observation.block = BlockReason::Auto;
    f.fake.observation.automatic = AutoReason::Confinement;
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(!f.live.automatically_paused() && f.live.auto_override_active()
        && f.live.desktop_requests() == 1, "one chord from Auto-native did not request desktop");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "safe override did not allow fresh desktop input");
    f.fake.activation_changed({20, 200});
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1 && !f.live.auto_override_active(), "override leaked into next app");
    f.automatic_tick(false);
    f.pad().buttons = 0; f.automatic_tick(false);
    f.pad().buttons = 0x0030; f.automatic_tick(false);
    check(!f.live.auto_override_active() && f.fake.count(OutputKind::PrimaryDown) == 1, "prerequisites permitted override");
}

void mode_chord_filters_stale_startup_and_connection_changes()
{
    Fixture f;
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(!f.live.automatically_paused(), "startup-held chord toggled without a release baseline");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused(), "fresh chord did not toggle after startup release");
    f.pad().buttons = 0;
    f.fake.time += 101;
    f.live.automatic_step(f.snapshot, f.fake.time, true);
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused(), "stale release armed a toggle");
    f.pad().buttons = 0; f.automatic_tick();
    ++f.pad().connection_epoch;
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused(), "held chord across reconnect toggled");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(!f.live.automatically_paused(), "fresh post-reconnect toggle failed");
    const auto sequence = f.snapshot.sequence;
    --f.snapshot.sequence;
    f.live.automatic_step(f.snapshot, f.fake.time, true);
    f.snapshot.sequence = sequence; f.automatic_tick();
    check(!f.live.automatically_paused(), "out-of-order chord replayed");
}

void multiple_pads_toggle_once_and_faults_remain_visible()
{
    Fixture f;
    check(f.live.prefer_slot(0, f.fake.time).empty(), "pin fixture");
    f.automatic_tick();
    f.snapshot.slots[1].buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused(), "nonpreferred controller could not switch modes");
    f.snapshot.slots[0].buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused(), "second held/duplicate controller double-toggled");
    f.snapshot.slots[0].buttons = f.snapshot.slots[1].buttons = 0; f.automatic_tick();
    f.snapshot.slots[0].buttons = f.snapshot.slots[1].buttons = 0x0030; f.automatic_tick();
    check(!f.live.automatically_paused(), "simultaneous controllers toggled twice");
    f.snapshot.slots[0].buttons = f.snapshot.slots[1].buttons = 0; f.automatic_tick();
    f.fake.returns = {0}; f.pad().buttons = a_button; f.automatic_tick();
    check(f.live.faulted(), "fault fixture");
    f.pad().buttons = 0x0030; f.automatic_tick();
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(f.live.faulted() && f.live.effective_mode() == EffectiveMode::Blocked
        && !f.live.auto_override_active() && f.fake.attempted.size() == 1,
          "toggle cleared output fault or retried a failed action");
}

void app_override_identity_and_hard_precedence()
{
    for (const auto automatic : {AutoReason::ExclusiveD3D, AutoReason::Presentation, AutoReason::Confinement}) {
        Fixture f;
        f.fake.observation.allowed = false; f.fake.observation.block = BlockReason::Auto;
        f.fake.observation.automatic = automatic;
        f.automatic_tick(); f.pad().buttons = 0x0030; f.automatic_tick();
        check(f.live.auto_override_active() && f.live.effective_mode() == EffectiveMode::Desktop, "recognized Auto override failed");
        f.fake.activation_changed({10, 100}); // Same verified owner despite a modal foreground change.
        f.fake.observation.routing++;
        f.pad().buttons = 0; f.automatic_tick();
        check(f.live.auto_override_active(), "same-root modal routing lost override");
        f.fake.activation_changed({11, 100});
        check(!f.live.auto_override_active(), "different root in same PID retained override");
        f.fake.activation_changed({10, 100});
        check(!f.live.auto_override_active(), "departure/re-entry revived prior override");
    }
    for (const auto block : {BlockReason::Recipient, BlockReason::Exclusion, BlockReason::DesktopSession,
        BlockReason::Routing, BlockReason::AutoUnavailable, BlockReason::AutoNotPresent}) {
        Fixture f; f.fake.observation.allowed = false; f.fake.observation.block = block;
        f.fake.observation.automatic = AutoReason::Confinement;
        f.automatic_tick(); f.pad().buttons = 0x0030; f.automatic_tick();
        check(!f.live.auto_override_active() && f.live.effective_mode() == EffectiveMode::Blocked
            && f.fake.accepted.empty(), "hard reason was bypassed as Auto");
    }
}

void override_request_rechecks_identity_and_physical_input()
{
    Fixture f; f.fake.observation.allowed = false; f.fake.observation.block = BlockReason::Auto;
    f.fake.observation.automatic = AutoReason::Confinement;
    auto expected = f.fake.observe(0);
    f.fake.observation.physical_held = Other;
    check(!f.fake.request_desktop(expected, f.fake.time + 100), "physical input bypassed final override check");
    f.fake.observation.physical_held = 0;
    expected = f.fake.observe(0);
    f.fake.activation_changed({99, 100});
    check(!f.fake.request_desktop(expected, f.fake.time + 100), "changed activation accepted stale override");
    f.fake.app = {};
    check(!f.live.request_desktop(f.fake.time), "unknown owner identity accepted");
}

void shell_and_reserved_key_classification_are_narrow()
{
    check(!confinement_candidate(100, 100, 200), "shell-process file window used global clip as game evidence");
    check(confinement_candidate(101, 100, 200), "unrelated/separate process received a shell allowlist");
    check(!confinement_candidate(200, 100, 200), "own overlay used as confinement evidence");
    for (unsigned key = 0xc3; key <= 0xda; ++key) {
        check(!physical_async_key(key), "reserved gamepad async key treated as physical keyboard");
        ReleasedKeyState input;
        check(input.observe(key, true, false, 1000) == KeyState::Clear, "reserved async state caused takeover");
        check(input.observe(key, true, true, 1000) == KeyState::Physical, "real raw reserved-key report was masked");
    }
    for (unsigned key = 0x88; key <= 0x8f; ++key) {
        check(!physical_async_key(key), "controller navigation was mistaken for physical keyboard input");
        check(input_category(key) == InputCategory::ReservedNavigation, "navigation must have a content-free category");
    }
    for (const unsigned key : {1u, 2u, 8u, 13u, 16u, 17u, 27u, 65u, 0xe7u}) {
        check(physical_async_key(key), "ordinary mouse/key/modifier/VK_PACKET was masked");
    }
}

void physical_ownership_stays_stable_until_controller_use()
{
    Fixture f; f.automatic_tick();
    f.live.physical_takeover(f.fake.time);
    for (unsigned i = 0; i < 8; ++i) {
        f.fake.observation.physical_held = Other; f.automatic_tick();
        check(f.live.effective_mode() == EffectiveMode::Physical && !f.live.controller_active(),
            "ordinary typing must not become a fault");
        f.fake.observation.physical_held = 0; f.automatic_tick();
        check(f.live.effective_mode() == EffectiveMode::Physical && f.fake.accepted.empty(),
            "key release must not oscillate ownership or emit controller input");
    }
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.live.effective_mode() == EffectiveMode::Desktop && f.live.controller_active()
        && f.fake.count(OutputKind::PrimaryDown) == 1, "fresh controller use must reclaim desktop normally");
    f.pad().buttons = 0; f.automatic_tick();
    f.live.physical_takeover(f.fake.time);
    f.fake.observation.allowed = false; f.fake.observation.block = BlockReason::Recipient;
    f.automatic_tick();
    check(f.live.effective_mode() == EffectiveMode::Blocked, "physical owner state must not hide hard blocks");
    f.fake.observation.allowed = true; f.fake.observation.block = BlockReason::None;
    f.automatic_tick();
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused(), "mode chord from physical ownership must still select native mode");
}

void physical_diagnostics_do_not_encode_ordinary_keys()
{
    const std::wstring expected = L"input=keyboard; raw=1; async=1; up-settling=0";
    for (unsigned key = 0x41; key <= 0x5a; ++key) {
        const auto output = input_diagnostic_text(input_category(key), true, true, false);
        check(output == expected, "ordinary letter identity leaked into serialized input diagnostics");
        check(output.find(L"VK=") == std::wstring::npos && output.find(std::to_wstring(key)) == std::wstring::npos,
            "diagnostic output must not contain numeric ordinary key identity");
    }
    check(input_category(0x10) == InputCategory::Modifier && input_category(0xa5) == InputCategory::Modifier,
        "modifiers should use a shared category, not individual key identity");
    check(input_category(1) == InputCategory::Mouse && input_category(0xc3) == InputCategory::ReservedGamepad,
        "mouse and reserved gamepad categories");
    check(input_diagnostic_text(input_category(0x41, true), false, true, true)
        == L"input=owned; raw=0; async=1; up-settling=1", "owned output must not reveal its key payload");
}

void accepted_up_settles_without_masking_physical_input()
{
    ReleasedKeyState input;
    check(input.observe(1, true, false, 1000) == KeyState::Physical, "unowned asynchronous down ignored");
    input.accepted_up(1, 1000);
    check(input.observe(1, true, false, 1010) == KeyState::Settling, "accepted-up lag misclassified as physical");
    check(input.observe(2, true, false, 1010) == KeyState::Physical, "settling mask leaked to another key");
    check(input.observe(1, false, false, 1020) == KeyState::Clear, "settled up did not clear");
    check(input.observe(1, true, false, 1021) == KeyState::Physical, "later real hold got stale immunity");
    input.accepted_up(1, 2000);
    check(input.observe(1, true, true, 2001) == KeyState::Physical, "raw overlap masked during settle");
    input.accepted_up(1, 3000);
    check(input.observe(1, true, false, 3100) == KeyState::Physical, "settling was not bounded to 100 ms");
}

void settling_defers_new_edges_without_replay()
{
    Fixture f; f.arm();
    f.fake.observation.block = BlockReason::Settling;
    f.fake.observation.routing_stable = false;
    f.pad().buttons = a_button; f.tick();
    check(f.live.armed() && f.fake.accepted.empty(), "settling must defer without immediate fault or new output");
    f.fake.observation.routing_stable = true;
    f.fake.observation.block = BlockReason::None;
    f.tick();
    check(f.fake.accepted.empty(), "settling-time button edge replayed");
    f.pad().buttons = 0; f.tick();
    f.pad().buttons = a_button; f.tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "fresh post-settling input failed");
}

void mode_toggle_keeps_pending_releases_bounded()
{
    Fixture f; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    f.fake.observation.cleanup_allowed = false;
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused() && f.live.owned() == Primary && !f.live.faulted(),
          "mode switch lost deferred cleanup ownership");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = 0x0030; f.automatic_tick();
    check(f.live.automatically_paused() && !f.live.enabled(), "toggle resumed while a release was pending");
    f.automatic_tick(true, 250); f.automatic_tick(true, 250);
    check(f.live.faulted() && f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryUp) == 0,
          "mode switches replenished the cleanup budget or emitted an unsafe up");
}

void automatic_ambiguity_and_preference()
{
    Fixture f;
    f.automatic_tick();
    f.pad().buttons = f.snapshot.slots[1].buttons = a_button;
    f.automatic_tick();
    check(f.fake.accepted.empty() && !f.live.selected_slot(), "ambiguous devices selected arbitrarily");
    check(!f.live.prefer_slot(4, f.fake.time).empty(), "invalid preference accepted");
    check(f.live.prefer_slot(1, f.fake.time).empty(), "valid preference rejected");
    f.automatic_tick();
    check(f.fake.accepted.empty(), "preference replayed a held button");
    f.snapshot.slots[1].buttons = 0; f.automatic_tick();
    f.snapshot.slots[1].buttons = a_button; f.automatic_tick();
    check(f.live.selected_slot() == 1u && f.fake.count(OutputKind::PrimaryDown) == 1,
          "preferred controller did not win over unpreferred active pad");
    check(f.live.prefer_slot({}, f.fake.time).empty(), "return to Auto");
    f.pad().buttons = f.snapshot.slots[1].buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.live.selected_slot() == 0u, "Auto preference did not resume discovery");
}

void automatic_reconnect_and_idle_switch()
{
    Fixture f;
    f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    f.snapshot.slots[1].buttons = a_button; f.automatic_tick();
    check(f.live.selected_slot() == 0u && f.fake.count(OutputKind::PrimaryDown) == 1,
          "another controller stole an active drag");
    f.pad().buttons = f.snapshot.slots[1].buttons = 0; f.automatic_tick();
    f.snapshot.slots[1].buttons = a_button; f.automatic_tick();
    check(f.live.selected_slot() == 1u && f.fake.count(OutputKind::PrimaryDown) == 2,
          "fresh idle-device handoff failed");
    f.snapshot.slots[1].connection = Connection::Disconnected;
    ++f.snapshot.slots[1].connection_epoch; f.automatic_tick();
    check(!f.live.owned(), "disconnect retained automatic hold");
    f.snapshot.slots[1].connection = Connection::Connected;
    ++f.snapshot.slots[1].connection_epoch; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 2, "reconnect replayed held button");
    f.snapshot.slots[1].buttons = 0; f.automatic_tick();
    f.snapshot.slots[1].buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 3, "reconnect needs no manual confirmation after fresh neutral");
}

void automatic_freshness_and_failure()
{
    Fixture f;
    f.automatic_tick();
    f.pad().buttons = a_button;
    f.automatic_tick(true, 101);
    check(f.fake.accepted.empty(), "expired neutral history allowed auto acquisition");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button;
    f.fake.returns = {0};
    f.automatic_tick();
    check(f.live.faulted(), "fake native rejection not faulted");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.attempted.size() == 1, "automatic loop retried rejected down");
    f.live.recover(f.fake.time);
    f.automatic_tick();
    check(f.fake.accepted.empty(), "recovery skipped neutral requirement");
}

void notification_policy_is_not_generic_fullscreen_detection()
{
    for (const auto state : {NotificationState::Busy, NotificationState::Available,
                            NotificationState::QuietTime, NotificationState::StoreApp}) {
        check(notification_pause_reason(state).empty(), "generic fullscreen/browser state was treated as a game");
    }
    for (const auto state : {NotificationState::NotPresent, NotificationState::ExclusiveD3D,
                            NotificationState::Presentation, static_cast<NotificationState>(999)}) {
        check(!notification_pause_reason(state).empty(), "documented exclusive/protected state not blocked");
    }
}

void fullscreen_pointer_confinement_is_not_plain_fullscreen()
{
    const Rect screen{0, 0, 1919, 1079};
    check(fullscreen_pointer_confined(screen, screen, {1, 1, 1918, 1078}, screen),
          "recorded Dragonwilds fullscreen/confinement shape not recognized");
    check(!fullscreen_pointer_confined(screen, screen, screen, screen),
          "fullscreen browser with unrestricted pointer classified as a game");
    check(!fullscreen_pointer_confined({10, 10, 1199, 799}, screen, {100, 100, 200, 200}, screen),
          "windowed editor capture classified as fullscreen");
    check(!fullscreen_pointer_confined({-8, -8, 1927, 1039}, screen, {1, 1, 1918, 1000}, screen),
          "maximized work-area window classified as fullscreen");
    const Rect desktop{-1920, 0, 1919, 1079}, monitor{-1920, 0, -1, 1079};
    check(fullscreen_pointer_confined(monitor, monitor, monitor, desktop), "per-monitor confinement missed");
    check(!fullscreen_pointer_confined(monitor, monitor, screen, desktop), "another monitor's clip treated as foreground confinement");
    check(!fullscreen_pointer_confined(monitor, monitor, desktop, desktop), "virtual-desktop default clip classified");
    check(!fullscreen_pointer_confined({}, screen, {1, 1, 50, 50}, screen), "invalid window classified");
}

void auto_game_handoff_retains_only_current_activation()
{
    GameHandoff handoff;
    check(handoff.update(true, 11, NotificationState::Busy, false).empty(), "BUSY alone still misclassifies");
    check(!handoff.update(true, 11, NotificationState::Busy, true).empty(), "confinement did not augment BUSY");
    check(!handoff.update(true, 11, NotificationState::Available, false).empty(),
          "in-game menu releasing pointer confinement resumed desktop emulation");
    handoff.foreground_changed(22);
    check(handoff.update(true, 22, NotificationState::Busy, false).empty(), "game latch leaked into another app");
    check(handoff.update(true, 11, NotificationState::Busy, false).empty(), "old HWND permanently classified as a game");
    check(!handoff.update(true, 11, NotificationState::Busy, true).empty(), "fresh game activation not recognized");
    check(handoff.update(false, 11, NotificationState::ExclusiveD3D, true).empty(), "Auto off still forces game detection");
    check(handoff.update(true, 11, NotificationState::Available, false).empty(), "Auto off retained stale confinement");
    check(!handoff.update(true, 11, NotificationState::ExclusiveD3D, false).empty(), "existing fullscreen signal lost");
    handoff.reset();
    check(handoff.update(true, 11, NotificationState::Available, false).empty(), "reset retained app evidence");
}

void auto_game_rule_releases_drag_without_changing_manual_or_safety_policy()
{
    Fixture f; GameHandoff handoff;
    f.automatic_tick(); f.pad().buttons = a_button; f.automatic_tick();
    f.fake.observation.reason = handoff.update(true, 11, NotificationState::Busy, true);
    f.fake.observation.allowed = f.fake.observation.reason.empty();
    f.automatic_tick();
    check(!f.live.owned() && !f.live.controller_active() && f.fake.count(OutputKind::PrimaryUp) == 1,
          "inferred game did not yield/release");
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 1, "game input was remapped to a new click");
    f.fake.observation.reason = handoff.update(false, 11, NotificationState::Busy, true);
    f.fake.observation.allowed = f.fake.observation.reason.empty();
    f.pad().buttons = 0; f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 2, "manual desktop mode unavailable with Auto off");
    f.fake.protected_mouse = true; f.automatic_tick();
    check(!f.live.controller_active(), "Auto off overrode protected recipient handling");
}

void automatic_active_poll_reuses_live_validation()
{
    Fixture f;
    f.automatic_tick();
    f.pad().buttons = a_button; f.automatic_tick();
    unsigned observations = 0;
    f.fake.during_observe = [&](const Output*) { ++observations; };
    f.automatic_tick();
    check(observations == 1 && f.live.owned() == Primary,
          "automatic held loop duplicated native observation or released the drag");
    f.snapshot.slots[1].buttons = a_button; f.automatic_tick();
    f.pad().buttons = 0; f.automatic_tick();
    check(f.live.selected_slot() == 0u && f.fake.count(OutputKind::PrimaryDown) == 1,
          "another pad's earlier held press replayed when the drag ended");
}

void automatic_gradual_intent_after_neutral()
{
    Fixture f;
    f.automatic_tick();
    f.pad().left_x = 7000; // Between stop and start: a deliberate slow stick ramp.
    f.automatic_tick();
    f.pad().left_x = 11000;
    f.automatic_tick();
    check(f.live.selected_slot() == 0u && f.live.armed(), "slow stick ramp lost recent neutral history");
    for (unsigned tick = 0; tick < 4; ++tick) { f.automatic_tick(); }
    check(f.fake.count(OutputKind::Move) > 0, "slow stick ramp never resumed movement");
}

void top_speed_requires_near_full_stick()
{
    PointerSettings settings; settings.profile = MotionProfile::SpeedBands; settings.speed = 1000;
    Pointer pointer; pointer.reset(0);
    Controller pad; Tick now = 0;
    const auto expect = [&](double raw, int distance) {
        pad.left_x = static_cast<std::int16_t>(std::lround(raw * 32767));
        now += 20;
        near(pointer.step(pad, {}, {-1000, -1000, 1000, 1000}, now, true, settings).point.x, distance, 1);
    };
    expect(.80, 7); expect(.90, 7); expect(.923, 7); expect(.925, 20);
    expect(.90, 20); expect(.887, 20); expect(.885, 7);
}

void explicit_admin_policy_respects_integrity_ceiling()
{
    constexpr unsigned low = 0x1000, medium = 0x2000, high = 0x3000, system = 0x4000;
    for (const auto host : {medium, high}) {
        check(process_integrity_allowed(host, low, false) && process_integrity_allowed(host, medium, false),
              "normal desktop recipients rejected");
        check(!process_integrity_allowed(host, system, false) && !process_integrity_allowed(host, 0, false),
              "system or unknown integrity accepted");
        check(!process_integrity_allowed(host, medium, true), "UIAccess process accepted");
    }
    check(!process_integrity_allowed(medium, high, false), "unelevated host can target admin windows");
    check(process_integrity_allowed(high, high, false), "explicit admin launch is rejected");
    for (const auto host : {0u, low, system}) {
        check(!process_integrity_allowed(host, medium, false), "unsupported host token accepted");
    }
}

void empty_mouse_reports_do_not_take_over()
{
    check(!mouse_report_activity(false, 0, 0, 0), "idle mouse report kicks out controller");
    check(mouse_report_activity(false, 1, 0, 0) && mouse_report_activity(false, 0, -1, 0),
          "real relative mouse movement ignored");
    check(mouse_report_activity(true, 0, 0, 0), "absolute motion to origin ignored");
    for (unsigned bit = 1; bit <= 0x800; bit <<= 1) {
        check(mouse_report_activity(false, 0, 0, bit), "mouse button or wheel activity ignored");
    }
}

void fresh_snapshot_after_ui_delay_preserves_drag()
{
    Fixture f;
    f.drag();
    f.pad().left_x = 32767;
    f.automatic_tick(true, 350); // The reader continued; the UI was in a move/size loop.
    check(f.live.armed() && f.live.controller_active() && f.live.owned() == Primary,
          "fresh input after a UI delay dropped an accepted drag");
    check(f.fake.count(OutputKind::Move) == 0 && f.fake.count(OutputKind::PrimaryUp) == 0,
          "UI delay replayed deferred motion or cancelled the drag");
    f.automatic_tick();
    check(f.fake.count(OutputKind::Move) == 1, "drag did not continue on the next fresh tick");
    f.pad().buttons = 0; f.automatic_tick(true, 350);
    check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1 && f.live.armed(),
          "release at the end of a UI delay was not balanced");
}

void ui_delay_consumes_edges_but_keeps_safety_gates()
{
    Fixture f; f.arm();
    f.pad().buttons = a_button | 0x6000;
    f.tick(350);
    check(f.live.armed() && f.fake.accepted.empty(), "UI delay replayed newly observed held buttons");
    f.pad().buttons = 0; f.tick();
    f.pad().buttons = a_button; f.tick();
    check(f.live.owned() == Primary, "fresh click after UI delay failed");
    ++f.fake.observation.activity;
    f.tick(350);
    check(!f.live.armed() && !f.live.owned(), "UI-delay recovery masked physical takeover");
    f.neutral_then_press();
    ++f.pad().connection_epoch; f.tick(350);
    check(!f.live.armed() && !f.live.owned(), "UI-delay recovery masked reconnect");
}

void transient_release_observation_does_not_latch_fault()
{
    Fixture f;
    f.drag();
    f.pad().buttons = 0;
    f.fake.during_observe = [&](const Output* intent) {
        if (intent && intent->kind == OutputKind::PrimaryUp) {
            f.fake.observation.cleanup_allowed = false;
            f.fake.observation.baseline_known = false;
        }
    };
    f.tick();
    check(f.live.owned() == Primary && !f.live.faulted(), "transient click-release routing latched a permanent fault");
    check(f.fake.count(OutputKind::PrimaryUp) == 0, "release emitted without a trusted context");
    f.fake.during_observe = {};
    f.fake.observation.cleanup_allowed = f.fake.observation.baseline_known = true;
    f.tick();
    check(!f.live.owned() && !f.live.faulted(), "release did not finish when context recovered");
    const auto moves = f.fake.count(OutputKind::Move);
    f.pad().left_x = 32767; f.tick();
    check(f.fake.count(OutputKind::Move) > moves, "blank-area click left pointer movement stopped");
}

void transient_context_cleanup_recovers_without_manual_reset()
{
    Fixture f;
    f.drag();
    f.fake.observation.allowed = f.fake.observation.cleanup_allowed = f.fake.observation.baseline_known = false;
    f.tick();
    check(f.live.owned() == Primary && !f.live.faulted(), "momentary route loss became a sticky send failure");
    const auto moves = f.fake.count(OutputKind::Move);
    f.pad().left_x = 32767;
    f.automatic_tick();
    check(f.live.owned() == Primary && !f.live.armed() && f.fake.count(OutputKind::Move) == moves,
          "automatic input escaped the pending-release gate");
    f.pad().left_x = 0;
    f.fake.observation.allowed = f.fake.observation.cleanup_allowed = f.fake.observation.baseline_known = true;
    f.pad().buttons = 0;
    f.tick(250);
    check(!f.live.owned() && !f.live.faulted(), "deferred cleanup did not recover automatically");
    f.tick();
    f.pad().buttons = a_button; f.tick();
    check(f.fake.count(OutputKind::PrimaryDown) == 2, "transient route recovery required manual reset");
}

Observation pending_route(Observation observation)
{
    observation.allowed = observation.baseline_known = observation.cleanup_allowed = false;
    observation.routing_stable = false;
    observation.routing_pending = true;
    observation.block = BlockReason::Routing;
    observation.mode = Mode::Blocked;
    return observation;
}

void transient_query_gap_preserves_owned_click()
{
    Fixture f;
    f.drag();
    const auto known = f.fake.observation;
    f.fake.observation = pending_route(known);
    f.pad().buttons = 0;
    f.automatic_tick();
    check(f.live.armed() && f.live.controller_active() && f.live.owned() == Primary && !f.live.faulted(),
          "one missing routing snapshot cancelled the accepted click");
    check(f.live.effective_mode() == EffectiveMode::Desktop && f.live.blocked_reason() == BlockReason::Routing,
          "bounded wait reported either a fault or unconditional eligibility");
    check(f.fake.accepted.size() == 1, "unknown routing emitted output");
    f.fake.observation = known;
    f.automatic_tick();
    check(f.live.armed() && !f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1,
          "fresh revalidated routing failed to complete the original click");
    f.pad().left_x = 32767;
    f.automatic_tick();
    check(f.fake.count(OutputKind::Move) == 1, "query-gap recovery required a new neutral gesture");
}

void query_gap_never_acquires_or_replays_press()
{
    Fixture startup;
    startup.automatic_tick();
    startup.fake.observation = pending_route(startup.fake.observation);
    startup.pad().buttons = a_button;
    startup.automatic_tick();
    check(!startup.live.armed() && startup.fake.accepted.empty(), "unknown route acquired a controller");

    for (unsigned query = 0; query < 3; ++query) {
        Fixture f;
        f.arm();
        const auto known = f.fake.observation;
        unsigned queries = 0;
        f.fake.during_observe = [&](const Output*) {
            if (queries++ == query) { f.fake.observation = pending_route(known); }
        };
        f.pad().buttons = a_button;
        f.tick();
        check(f.live.armed() && f.fake.accepted.empty(), "pending first/final query emitted or disarmed");
        check(f.live.blocked_reason() == BlockReason::Routing, "mid-event query gap advertised eligibility");
        f.fake.during_observe = {};
        f.fake.observation = known;
        f.tick();
        check(f.fake.accepted.empty(), "query-gap button edge replayed after recovery");
        f.pad().buttons = 0; f.tick();
        f.pad().buttons = a_button; f.tick();
        check(f.fake.count(OutputKind::PrimaryDown) == 1, "fresh post-query-gap press failed");
    }
}

void query_gap_deadline_and_cleanup_remain_bounded()
{
    for (const bool restored : {false, true}) {
        Fixture f;
        f.drag();
        const auto known = f.fake.observation;
        f.fake.observation = pending_route(known);
        f.pad().buttons = 0;
        f.tick();
        f.tick(99);
        check(f.live.armed() && f.live.owned() == Primary, "query gap timed out before 100 ms");
        if (restored) { f.fake.observation = known; }
        f.tick(1);
        check(!f.live.armed() && !f.live.controller_active(), "query gap resumed at/after its 100 ms deadline");
        check(f.fake.count(OutputKind::PrimaryUp) == (restored ? 1u : 0u),
              "cleanup skipped revalidation or sent into unknown routing");
        if (!restored) {
            f.tick(250); f.tick(250);
            check(f.live.faulted() && f.live.owned() == Primary, "query gap replenished cleanup budget");
            f.tick(250);
            check(f.fake.accepted.size() == 1, "exhausted cleanup produced extra output");
        }
    }
}

void query_gap_cannot_hide_real_safety_changes()
{
    for (unsigned change = 0; change < 8; ++change) {
        Fixture f;
        f.drag();
        f.fake.observation = pending_route(f.fake.observation);
        switch (change) {
        case 0: ++f.fake.observation.context; break;
        case 1: ++f.fake.observation.activity; break;
        case 2: --f.fake.observation.desktop.left; break;
        case 3: f.fake.observation.physical_held = Other; break;
        case 4: f.fake.observation.block = BlockReason::Recipient; break;
        case 5: f.fake.observation.block = BlockReason::DesktopSession; break;
        case 6: f.fake.observation.block = BlockReason::Exclusion; break;
        case 7: f.fake.observation.available = false; break;
        }
        f.tick();
        check(!f.live.armed() && !f.live.controller_active(), "query pending hid a real safety boundary");
        check(f.fake.accepted.size() == 1 && f.live.owned() == Primary,
              "unsafe query-gap cleanup lost ownership or injected a release");
    }
    Fixture stale;
    stale.drag();
    stale.fake.observation = pending_route(stale.fake.observation);
    stale.fake.during_observe = [&](const Output*) { stale.fake.time += 101; };
    stale.tick();
    check(!stale.live.armed(), "pending routing bypassed the controller freshness deadline");

    Fixture idle;
    idle.arm();
    idle.pad().left_x = 32767; idle.tick();
    idle.pad().left_x = 0;
    const auto known = idle.fake.observation;
    unsigned reads = 0;
    idle.fake.during_observe = [&](const Output*) {
        idle.fake.observation = reads++ == 0 ? pending_route(known) : known;
        if (reads == 1) { idle.fake.observation.physical_held = Other; }
    };
    idle.automatic_tick();
    check(!idle.live.armed() && !idle.live.controller_active(),
          "automatic pre-observation physical hold was masked by a subsequent clear query");
}

void query_disappearance_classification_is_narrow()
{
    // Win32 ERROR_INVALID_WINDOW_HANDLE; missing identity can explain only the
    // no-error/invalid-handle/invalid-parameter forms, never permission failure.
    check(routing_query_disappeared(1400, false), "invalid-window result not classified as disappearing routing");
    for (const unsigned error : {0u, 6u, 87u}) {
        check(routing_query_disappeared(error, true), "known missing identity did not defer");
        check(!routing_query_disappeared(error, false), "valid identity masked an unknown query failure");
    }
    for (const unsigned error : {5u, 8u, 1460u, 9999u}) {
        check(!routing_query_disappeared(error, false) && !routing_query_disappeared(error, true),
              "access denied/infrastructure/unknown query error was softened");
    }
}

void routing_wait_deadline_survives_dispatch_queries()
{
    for (const Tick elapsed : {0ull, 1ull, 2ull}) {
        for (const unsigned slow_query : {1u, 2u}) {
            Fixture f;
            f.arm();
            const auto known = f.fake.observation;
            f.fake.observation = pending_route(known);
            f.tick();
            f.fake.observation = known;
            unsigned queries = 0;
            f.fake.during_observe = [&](const Output* intent) {
                if (intent && ++queries == slow_query) { f.fake.time += elapsed; }
            };
            f.pad().buttons = a_button;
            f.tick(99);
            if (!elapsed) {
                check(f.live.armed() && f.fake.count(OutputKind::PrimaryDown) == 1,
                      "routing recovery before its deadline was blocked");
                continue;
            }
            check(f.fake.accepted.empty(), "routing wait expired during dispatch but emitted a new down");
            check(!f.live.armed() && !f.live.controller_active(), "expired routing dispatch did not require neutral");
            f.fake.during_observe = {};
            f.tick();
            check(f.fake.accepted.empty(), "expired routing dispatch replayed held A");
        }
    }
}

void cleanup_up_cannot_clear_expired_routing_wait()
{
    for (const bool motion : {false, true}) {
        for (const Tick elapsed : {1ull, 2ull}) {
            Fixture f;
            f.drag();
            const auto known = f.fake.observation;
            f.fake.observation = pending_route(known);
            f.tick();
            f.fake.observation = known;
            f.pad().buttons = 0;
            f.pad().left_x = motion ? 32767 : 0;
            f.fake.during_observe = [&](const Output* intent) {
                if (intent && intent->kind == OutputKind::PrimaryUp) { f.fake.time += elapsed; }
            };
            f.tick(99);
            check(f.fake.count(OutputKind::PrimaryUp) == 1 && !f.live.owned(),
                  "routing expiry prevented independently authorized cleanup");
            check(f.fake.accepted.size() == 2 && !f.live.armed() && !f.live.controller_active(),
                  "cleanup revived expired routing or allowed same-frame movement");
            f.fake.during_observe = {};
            f.pad().buttons = a_button; f.tick();
            check(f.fake.count(OutputKind::PrimaryDown) == 1, "post-cleanup expiry did not require a new neutral");
        }
    }
    Fixture f;
    f.drag();
    const auto known = f.fake.observation;
    f.fake.observation = pending_route(known);
    f.tick();
    f.fake.observation = known;
    f.pad().buttons = 0;
    f.pad().left_x = 32767;
    f.fake.during_observe = [&](const Output* intent) {
        if (intent && intent->kind == OutputKind::Move) { f.fake.time += 2; }
    };
    f.tick(99);
    check(f.fake.count(OutputKind::PrimaryUp) == 1 && !f.live.owned(),
          "cleanup before the routing deadline was blocked");
    check(f.fake.count(OutputKind::Move) == 0 && !f.live.armed(),
          "cleanup at 99 ms erased the deadline for later movement dispatch");
}

int main()
{
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"stable physical ownership and controller handoff", physical_ownership_stays_stable_until_controller_use},
        {"activation-scoped override and hard precedence", app_override_identity_and_hard_precedence},
        {"override final identity and physical checks", override_request_rechecks_identity_and_physical_input},
        {"shell-only confinement and reserved async keys", shell_and_reserved_key_classification_are_narrow},
        {"categorical private physical diagnostics", physical_diagnostics_do_not_encode_ordinary_keys},
        {"accepted-up observation settling", accepted_up_settles_without_masking_physical_input},
        {"settling consumes deferred edges", settling_defers_new_edges_without_replay},
        {"fullscreen confined-pointer predicate", fullscreen_pointer_confinement_is_not_plain_fullscreen},
        {"automatic game evidence lifetime and off setting", auto_game_handoff_retains_only_current_activation},
        {"automatic game rule yields drag and retains safety", auto_game_rule_releases_drag_without_changing_manual_or_safety_policy},
        {"manual mode toggle edge and no replay", manual_chord_toggles_once_and_never_replays_input},
        {"manual mode toggle respects games and prerequisites", manual_toggle_does_not_override_games_or_prerequisites},
        {"mode toggle startup stale and reconnect", mode_chord_filters_stale_startup_and_connection_changes},
        {"mode toggle multiple pads and faults", multiple_pads_toggle_once_and_faults_remain_visible},
        {"mode toggle pending cleanup budget", mode_toggle_keeps_pending_releases_bounded},
        {"custom and merged band velocities", configurable_and_merged_band_rates},
        {"band rate validation and safe Apply", band_rate_validation_and_live_apply},
        {"top speed near full stick", top_speed_requires_near_full_stick},
        {"explicit admin integrity ceiling", explicit_admin_policy_respects_integrity_ceiling},
        {"empty mouse reports retain control", empty_mouse_reports_do_not_take_over},
        {"fresh snapshot after UI delay preserves drag", fresh_snapshot_after_ui_delay_preserves_drag},
        {"UI delay consumes edges and retains safety", ui_delay_consumes_edges_but_keeps_safety_gates},
        {"automatic gradual intent", automatic_gradual_intent_after_neutral},
        {"transient click release", transient_release_observation_does_not_latch_fault},
        {"transient context cleanup recovery", transient_context_cleanup_recovers_without_manual_reset},
        {"transient query gap click continuity", transient_query_gap_preserves_owned_click},
        {"query gap no acquisition or replay", query_gap_never_acquires_or_replays_press},
        {"query gap deadline and cleanup budget", query_gap_deadline_and_cleanup_remain_bounded},
        {"query gap real safety boundaries", query_gap_cannot_hide_real_safety_changes},
        {"query disappearance error classification", query_disappearance_classification_is_narrow},
        {"routing wait deadline through dispatch", routing_wait_deadline_survives_dispatch_queries},
        {"cleanup does not revive expired routing", cleanup_up_cannot_clear_expired_routing_wait},
        {"automatic first intent", automatic_start_and_first_intent},
        {"automatic held startup/readiness", automatic_held_start_and_readiness},
        {"automatic gaming/physical handoff", automatic_game_and_physical_handoff},
        {"sticky emergency pause", automatic_pause_is_sticky},
        {"automatic ambiguity/preference", automatic_ambiguity_and_preference},
        {"automatic reconnect/idle switch", automatic_reconnect_and_idle_switch},
        {"automatic freshness/failure", automatic_freshness_and_failure},
        {"limited fullscreen attention policy", notification_policy_is_not_generic_fullscreen_detection},
        {"automatic reuses live validation", automatic_active_poll_reuses_live_validation},
        {"radial response", radial_response}, {"time/precision/fractions", time_precision_fractions},
        {"scroll fractions", scroll_fractions}, {"hysteresis/gap", hysteresis_and_gap},
        {"coordinates", coordinates}, {"absolute pixel round trips", absolute_pixel_round_trips},
        {"disabled observer boundaries", disabled_observer_boundaries},
        {"startup/selection", startup_and_explicit_selection},
        {"all controls neutral", all_controls_neutral}, {"physical baseline", physical_baseline},
        {"synthetic baseline classification", synthetic_baseline_classification},
        {"pre-enable cached sample", pre_enable_cached_sample},
        {"accepted drag/pause", accepted_drag_pause}, {"normal release", normal_release},
        {"physical takeover", physical_takeover}, {"physical overlap/retry budget", physical_overlap_and_cleanup_budget},
        {"context/geometry", context_and_geometry}, {"stale/scheduling", stale_and_scheduling},
        {"selection/reconnect", selection_and_reconnect}, {"disconnect/error", disconnect_error},
        {"tap edges/mapping isolation", tap_edges_and_mapping_isolation}, {"menu mapping", menu_mapping},
        {"chord/Guide", chord_and_guide}, {"failed down", failed_down},
        {"paused context chord/reconnect", paused_context_chord_and_reconnect},
        {"live motion/negative origin", motion_dispatch_and_virtual_origin},
        {"partial tap/failed cleanup", partial_tap_and_failed_cleanup}, {"cleanup desktop loss", cleanup_desktop_loss},
        {"final recipient/context", final_recipient_and_context}, {"validation deadline", expired_during_validation},
        {"observer availability", unavailable_observers}, {"impossible acceptance", impossible_acceptance},
        {"cross-window drag/activation", cross_window_drag_and_activation},
        {"menus/submenus without neutral", menus_submenus_without_neutral},
        {"safe final routing revision", safe_routing_during_final_validation},
        {"transient routing/owned drag", transient_routing_with_owned_drag},
        {"deferred stick hysteresis", routing_deferral_preserves_stick_hysteresis},
        {"deferred tap release", deferred_tap_release},
        {"motion destination/protected transition", final_move_destination_and_protected_transition},
        {"exclusion validation", exclusion_validation}, {"exclusion apply/recipients", exclusion_apply_and_recipients},
        {"post-motion exclusions", exclusions_checked_after_motion},
        {"protected drag cleanup fault", protected_drag_cleanup_fault},
        {"default settings equivalence", default_settings_equivalence},
        {"settings validation/atomic apply", settings_validation_and_atomic_apply},
        {"controller ownership visibility", controller_ownership_visibility},
        {"settings cleanup failure/limits", settings_cleanup_failure_and_limits},
        {"final exclusion/takeover", final_exclusion_and_takeover},
        {"configured pointer math", configured_pointer_math}, {"expired neutral observation", expired_neutral_observation},
        {"bounded unstable release", persistent_unstable_release_is_bounded},
        {"Smooth original math equivalence", smooth_original_math},
        {"speed band plateaus and user deadzones", speed_band_plateaus},
        {"speed band hysteresis", speed_band_hysteresis},
        {"speed band direction diagonal cap and unchanged scroll", speed_band_direction_scroll},
        {"speed band resets gaps and skipped intervals", speed_band_reset_gap_deferral},
        {"speed band live pause takeover settings and stale boundaries", speed_band_live_boundaries},
        {"speed band live routing preserves drag and hysteresis", speed_band_live_routing}
    };
    unsigned failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failed << "/" << std::size(tests) << " live cases passed (FAKE output only)\n";
    return failed ? 1 : 0;
}
