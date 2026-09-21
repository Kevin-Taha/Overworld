#include "Live.h"
#include "NavigationFilter.h"
#include "SelfCaption.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace overworld;
using namespace overworld::live;
namespace {
void check(bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } }
constexpr NavigationForeground original{10, 100, 1000}, changed{20, 200, 2000};
struct Fake final : OutputBoundary {
    Observation observation;
    Tick time = 4400;
    bool physical_a = false, retrigger = false, native_release_lag = false;
    unsigned accepted_count = 1;
    unsigned native_passed_down = 0;
    std::vector<Output> attempts, accepted;
    std::function<void(const Output*)> during_observe;
    NavigationFilter filter;
    NavigationPolicy policy{original, 4444, 1, true, NavigationGate::Desktop};
    NavigationForeground foreground = original;
    Fake()
    {
        observation.available = observation.allowed = observation.baseline_known = observation.cleanup_allowed = true;
        observation.mode = Mode::Pointer;
        observation.app = {{10, 100}, 1};
        observation.routing = 354; observation.context = 1;
        observation.bounds = observation.desktop = {0, 0, 1919, 1079};
        observation.cursor = {800, 300};
        observation.expires = 10000;
    }
    Observation observe(unsigned, const Output* intent = nullptr) override
    {
        if (during_observe) { during_observe(intent); }
        return observation;
    }
    Tick now() const override { return time; }
    bool native(bool down, Tick at)
    {
        const bool suppressed = filter.event({0xc3, down, true, false, 0, 123, 0}, policy, foreground, at);
        if (down && !suppressed) { ++native_passed_down; }
        return suppressed;
    }
    unsigned send(const Output& output, Rect) override
    {
        attempts.push_back(output);
        if (accepted_count != 1) { return accepted_count; }
        accepted.push_back(output);
        if (output.kind == OutputKind::Move) {
            observation.cursor = output.point;
        }
        if (output.kind == OutputKind::PrimaryDown && retrigger) {
            foreground = changed;
            observation.app = {{20, 200}, 2};
            observation.routing = 357;
            if (physical_a || native_release_lag) {
                check(!native(false, time + 1), "foreground transition abandons suppressed up rather than swallowing in new foreground");
                check(!native(true, time + 3), "retrigger sees old policy and passes in different foreground");
            }
        }
        return 1;
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
    Live live;
    Snapshot snapshot;
    explicit Fixture(bool release_tap = true) : live(fake, release_tap)
    {
        snapshot.sequence = 1; snapshot.sampled_at = fake.time;
        for (auto& pad : snapshot.slots) { pad.connection = Connection::Connected; pad.connection_epoch = 1; }
    }
    Controller& pad() { return snapshot.slots[1]; }
    void at(Tick time, bool automatic = false, bool prerequisites = true)
    {
        fake.time = time; snapshot.sampled_at = time; ++snapshot.sequence;
        fake.physical_a = (pad().buttons & a_button) != 0;
        if (automatic) { live.automatic_step(snapshot, time, prerequisites); }
        else { live.step(snapshot, time); }
    }
    void arm()
    {
        live.select(1, snapshot, fake.time); live.enable(fake.time); at(4440);
        check(live.armed(), "fixture neutral arms selected slot");
    }
    void press()
    {
        arm(); pad().buttons = a_button; at(4456);
        check(fake.attempts.empty(), "stationary trial press has no immediate mouse output");
    }
};
}
int main()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& body) {
        try {
            body(); ++passed; std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    try {
        test("recorded foreground retrigger fails press-to-click but not released tap under declared bridge assumption", [] {
            for (const bool trial : {false, true}) {
                Fixture f{trial}; f.arm();
                f.fake.filter.event({0xc3, false, true, false, 0, 123, 0}, f.fake.policy, original, 4444);
                check(f.fake.native(true, 4483), "initial native A suppressed, not first hold");
                f.fake.policy.issued = 4491;
                f.fake.during_observe = [&](const Output* output) {
                    if (output && output->kind == OutputKind::PrimaryDown && f.fake.time < 4537) { f.fake.time = 4537; }
                };
                f.pad().buttons = a_button; f.fake.retrigger = true; f.at(4513);
                if (!trial) {
                    check(f.fake.count(OutputKind::PrimaryDown) == 1 && f.fake.native_passed_down == 1
                        && f.fake.filter.counts().foreground_abandons == 1 && f.fake.time == 4537,
                        "old path reproduces 4513 sample, 4537 mouse-down and 4538/4540 retrigger");
                    f.fake.policy = {changed, 4567, 2, true, NavigationGate::Desktop};
                    check(!f.fake.native(false, 4582), "passed retrigger's up stays passed after policy catches up");
                    f.fake.time = 4613;
                    f.live.invalidate(4613, L"Modeled hard context change during native activation");
                    check(f.fake.count(OutputKind::PrimaryUp) == 1 && !f.live.owned(),
                        "cleanup can release mapped down before sampled A release without bypassing ownership");
                    f.pad().buttons = 0; f.at(4651);
                } else {
                    check(f.fake.attempts.empty() && f.fake.native_passed_down == 0,
                        "no focus-changing mapped down while sampled A remains held");
                    f.at(4537); f.at(4582); f.at(4613);
                    f.fake.policy.issued = 4630;
                    check(f.fake.native(false, 4640), "declared model: bridge release precedes released XInput sample");
                    f.pad().buttons = 0; f.at(4651);
                    check(f.fake.count(OutputKind::PrimaryDown) == 1 && f.live.owned() == Primary
                        && f.fake.native_passed_down == 0, "commit after release removes modeled held-A retrigger trigger");
                    f.at(4667);
                    check(f.fake.count(OutputKind::PrimaryUp) == 1 && !f.live.owned(), "normal trusted owned-up follows");
                }
            }
        });
        test("late native bridge up remains explicitly uncovered and never grants cross-foreground suppression", [] {
            Fixture f; f.press();
            f.fake.filter.event({0xc3, false, true, false, 0, 123, 0}, f.fake.policy, original, 4450);
            check(f.fake.native(true, 4456), "native hold was suppressed");
            f.fake.retrigger = f.fake.native_release_lag = true;
            f.pad().buttons = 0; f.at(4488);
            check(!f.fake.physical_a && f.fake.count(OutputKind::PrimaryDown) == 1 && f.fake.native_passed_down == 1,
                "sampled physical release is not proof native bridge has processed release");
            check(f.fake.filter.counts().foreground_abandons == 1,
                "candidate leaves unknown-foreground and native source safety unchanged");
        });
        test("stationary held A promotes exactly at 250 ms and remains a drag", [] {
            Fixture f; f.press();
            for (Tick at = 4472; at < 4705; at += 16) { f.at(at); }
            f.at(4705);
            check(f.fake.attempts.empty(), "249 ms must remain pending");
            f.at(4706);
            check(f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryDown) == 1, "250 ms starts normal held drag");
            f.pad().left_x = 32767; f.at(4722);
            check(f.fake.count(OutputKind::Move) == 1 && f.live.owned() == Primary, "hold plus stick continues dragging");
            f.pad().left_x = 0; f.pad().buttons = 0; f.at(4738);
            check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "release ends drag normally");
        });
        test("resolved routing wait cannot expire a fresh validated 96-ms tap", [] {
            for (const bool automatic : {false, true}) {
                Fixture f; f.arm();
                f.fake.observation.routing_stable = false; f.at(4456, automatic);
                check(f.live.armed() && f.fake.attempts.empty(), "brief routing wait retains arming without output");
                f.fake.observation.routing_stable = true;
                f.pad().buttons = a_button; f.at(4472, automatic);
                for (Tick at = 4488; at <= 4552; at += 16) { f.at(at, automatic); }
                f.pad().buttons = 0; f.at(4568, automatic);
                check(f.live.armed() && f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryDown) == 1,
                    "a resolved wait must not cancel the 96-ms tap at the old wait's 112-ms age");
                f.at(4584, automatic);
                check(!f.live.owned() && !f.live.faulted() && f.fake.count(OutputKind::PrimaryUp) == 1,
                    "recovered tap retains normal accepted-up cleanup");
            }
        });
        test("routing recovery preserves the original 249/250-ms drag threshold", [] {
            Fixture f; f.arm();
            f.fake.observation.routing_stable = false; f.at(4456);
            f.fake.observation.routing_stable = true;
            f.pad().buttons = a_button; f.at(4472);
            for (Tick at = 4488; at < 4721; at += 16) { f.at(at); }
            f.at(4721);
            check(f.live.armed() && f.fake.attempts.empty(), "resolved routing wait must leave 249-ms hold pending");
            f.at(4722);
            check(f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryDown) == 1,
                "pending validation must not renew the original 250-ms gesture threshold");
            f.pad().buttons = 0; f.at(4738);
            check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "promoted drag ends normally");
        });
        test("expired unresolved routing wait cannot be reset by a newly stable A press", [] {
            Fixture f; f.arm();
            f.fake.observation.routing_stable = false;
            for (Tick at = 4456; at <= 4536; at += 16) { f.at(at); }
            f.fake.observation.routing_stable = true;
            f.pad().buttons = a_button; f.at(4556);
            check(!f.live.armed() && f.fake.attempts.empty(), "100-ms unresolved wait expires before pending-tap entry");
            f.pad().buttons = 0; f.at(4572); f.at(4588);
            check(f.fake.attempts.empty(), "expired gesture cannot replay after rearming");
        });
        test("deferred drag release then repress retains existing ownership without a second down", [] {
            for (const bool trial : {false, true}) {
                for (const bool automatic : {false, true}) {
                    Fixture f{trial}; f.arm();
                    f.pad().buttons = a_button; f.pad().left_x = 32767; f.at(4456, automatic);
                    check(f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryDown) == 1
                        && f.fake.count(OutputKind::Move) == 1, "drag begins with one accepted down and motion");
                    f.pad().buttons = 0; f.fake.observation.routing_stable = false; f.at(4472, automatic);
                    check(f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryUp) == 0,
                        "unstable release retains original down");
                    f.fake.observation.routing_stable = true;
                    f.pad().buttons = a_button; f.at(4488, automatic);
                    check(f.live.armed() && !f.live.faulted() && f.live.owned() == Primary
                        && f.fake.count(OutputKind::PrimaryDown) == 1 && f.fake.count(OutputKind::PrimaryUp) == 0
                        && f.fake.count(OutputKind::Move) == 2,
                        "repress while occupied must continue drag motion, not dispatch a redundant down and cleanup");
                    f.pad().left_x = 0; f.pad().buttons = 0; f.at(4504, automatic);
                    check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1,
                        "later stable release balances the original drag once");
                }
            }
        });
        test("deferred drag release completes normally without repress", [] {
            Fixture f; f.arm();
            f.pad().buttons = a_button; f.pad().left_x = 32767; f.at(4456);
            f.pad().buttons = 0; f.pad().left_x = 0; f.fake.observation.routing_stable = false; f.at(4472);
            f.fake.observation.routing_stable = true; f.at(4488);
            check(f.live.armed() && !f.live.owned() && f.fake.count(OutputKind::PrimaryDown) == 1
                && f.fake.count(OutputKind::PrimaryUp) == 1, "stable up releases once and never creates a new tap");
        });
        test("repress after a deferred tap up finishes that accepted tap before starting a new gesture", [] {
            Fixture f; f.press(); f.pad().buttons = 0; f.at(4472);
            check(f.live.owned() == Primary, "first released tap owns its down");
            f.fake.observation.routing_stable = false; f.at(4488);
            f.fake.observation.routing_stable = true; f.pad().buttons = a_button; f.at(4504);
            check(f.live.armed() && !f.live.owned() && f.fake.count(OutputKind::PrimaryDown) == 1
                && f.fake.count(OutputKind::PrimaryUp) == 1, "tap's scheduled up completes before new stationary press");
            f.pad().buttons = 0; f.at(4520); f.at(4536);
            check(!f.live.owned() && f.fake.count(OutputKind::PrimaryDown) == 2 && f.fake.count(OutputKind::PrimaryUp) == 2,
                "each tap commits and balances exactly once");
        });
        test("movement starts drag down before its first motion without losing original anchor", [] {
            Fixture f; f.press();
            f.pad().left_x = 32767; f.at(4472);
            check(f.fake.accepted.size() == 2 && f.fake.accepted[0].kind == OutputKind::PrimaryDown
                && f.fake.accepted[1].kind == OutputKind::Move && f.live.owned() == Primary, "down precedes first drag motion");
            f.pad().buttons = 0; f.pad().left_x = 0; f.at(4488);
            check(!f.live.owned(), "drag up balanced");
        });
        test("pending tap uses automatic acquisition without being discarded on neutral release", [] {
            Fixture f;
            f.at(4440, true);
            f.pad().buttons = a_button; f.at(4456, true);
            check(f.live.selected_slot() == 1u && f.fake.attempts.empty(), "fresh slot1 A starts pending gesture automatically");
            f.pad().buttons = 0; f.at(4472, true);
            check(f.fake.count(OutputKind::PrimaryDown) == 1 && f.live.owned() == Primary, "neutral release commits without reacquiring another controller");
            f.at(4488, true);
            check(f.fake.count(OutputKind::PrimaryUp) == 1 && !f.live.owned(), "automatic tap up delivered");
        });
        test("changed original targets, unsafe gates and competing gestures cancel without replay", [] {
            for (unsigned changed_case = 0; changed_case < 18; ++changed_case) {
                Fixture f; f.press();
                auto& o = f.fake.observation;
                switch (changed_case) {
                case 0: ++o.context; break;
                case 1: ++o.activity; break;
                case 2: ++o.routing; break;
                case 3: ++o.app.generation; break;
                case 4: ++o.app.identity.process; break;
                case 5: ++o.cursor.x; break;
                case 6: o.routing_stable = false; break;
                case 7: o.allowed = false; o.block = BlockReason::Exclusion; break;
                case 8: o.available = false; break;
                case 9: o.physical_held = Other; break;
                case 10: o.mode = Mode::Blocked; break;
                case 11: ++o.bounds.right; break;
                case 12: o.baseline_known = false; break;
                case 13: f.snapshot.slots[0].buttons = a_button; break;
                case 14: f.pad().right_y = 32767; break;
                case 15: f.pad().buttons |= 0x2000; break;
                case 16: ++f.pad().connection_epoch; break;
                case 17: o.app = {}; break;
                }
                f.at(4472);
                f.pad().buttons = 0; f.pad().right_y = 0; f.snapshot.slots[0].buttons = 0;
                o = Fake{}.observation;
                f.at(4488); f.at(4504);
                check(f.fake.attempts.empty(), "cancelled held gesture never becomes a delayed click at a restored or new target");
            }
        });
        test("pause keyboard native physical prerequisites and text-fault boundaries cancel pending tap", [] {
            for (unsigned boundary = 0; boundary < 8; ++boundary) {
                Fixture f; f.press();
                switch (boundary) {
                case 0: f.live.pause(4457); break;
                case 1: f.live.physical_takeover(4457); break;
                case 2: f.live.hold_for_keyboard(true, 4457); break;
                case 3: f.live.suspend_automatic(4457); break;
                case 4: f.live.observe_text_fault(1, 4457); break;
                case 5: f.at(4472, true, false); break;
                case 6: f.live.invalidate(4457, L"Display changed"); break;
                case 7: check(f.live.prefer_slot(0, 4457).empty(), "preference fixture"); break;
                }
                f.pad().buttons = 0; f.at(4488);
                check(f.fake.attempts.empty(), "input boundary cannot later commit pending tap");
            }
        });
        test("stale gap duplicate and release-with-motion never manufacture a tap", [] {
            Fixture f; f.press();
            f.pad().buttons = 0; f.at(4557);
            check(f.fake.attempts.empty(), "101ms sample gap cancels");
            Fixture g; g.press();
            g.pad().buttons = 0; g.snapshot.sampled_at = 4472; g.fake.time = 4472;
            g.live.step(g.snapshot, 4472);
            check(g.fake.attempts.empty(), "changed sample timestamp at duplicate sequence fails closed");
            Fixture h; h.press();
            h.pad().buttons = 0; h.pad().left_x = 32767; h.at(4488);
            check(h.fake.attempts.empty(), "release with new pointer movement cancels instead of relocating tap");
        });
        test("fresh release commit stays pinned through both existing dispatcher validation reads", [] {
            for (unsigned on_read = 1; on_read <= 2; ++on_read) {
                Fixture f; f.press(); unsigned reads = 0;
                f.fake.during_observe = [&](const Output* output) {
                    if (output && output->kind == OutputKind::PrimaryDown && ++reads == on_read) { ++f.fake.observation.routing; }
                };
                f.pad().buttons = 0; f.at(4488);
                check(f.fake.attempts.empty() && !f.live.owned(), "target change during dispatch cannot be reinterpreted as a new target");
                f.fake.during_observe = {}; f.at(4504);
                check(f.fake.attempts.empty(), "cancelled commit never replays");
            }
        });
        test("deferred or expired release commit is consumed rather than replayed", [] {
            for (const bool expires : {false, true}) {
                Fixture f; f.press();
                f.fake.during_observe = [&](const Output* output) {
                    if (!output || output->kind != OutputKind::PrimaryDown) { return; }
                    if (expires) { f.fake.time = 4590; }
                    else { f.fake.observation.routing_stable = false; }
                };
                f.pad().buttons = 0; f.at(4488);
                check(f.fake.attempts.empty(), "stale or unstable dispatch cannot send tap");
                f.fake.during_observe = {}; f.fake.observation.routing_stable = true;
                f.at(4591); f.at(4607);
                check(f.fake.attempts.empty(), "regaining eligibility cannot replay consumed commit");
            }
        });
        test("failed or uncertain down is never retried and accepted up keeps cleanup restrictions", [] {
            for (const unsigned result : {0u, 2u}) {
                Fixture f; f.press(); f.fake.accepted_count = result;
                f.pad().buttons = 0; f.at(4488); f.at(4504); f.at(4520);
                check(f.fake.attempts.size() == 1 && f.live.faulted() && !f.live.owned(), "failed/uncertain commit retains original no-replay fault");
            }
            Fixture f; f.press(); f.pad().buttons = 0; f.at(4488);
            f.fake.observation.cleanup_allowed = false; f.at(4504);
            check(f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryUp) == 0, "untrusted cleanup retains ownership");
            f.fake.observation.cleanup_allowed = true; f.at(4520);
            check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "trusted route releases only accepted tap down");
        });
        test("promoted drag retains physical-overlap cleanup and no second down", [] {
            Fixture f; f.press(); f.pad().left_x = 32767; f.at(4472);
            check(f.live.owned() == Primary, "drag owned");
            f.fake.observation.physical_held = Primary;
            f.live.physical_takeover(4473);
            check(f.live.owned() == Primary && f.fake.count(OutputKind::PrimaryUp) == 0,
                "physical hold overlap blocks owned release rather than interfering");
            f.pad().buttons = 0; f.pad().left_x = 0; f.at(4488);
            check(f.fake.count(OutputKind::PrimaryDown) == 1, "blocked drag cannot become a tap");
            f.fake.observation.physical_held = 0; f.fake.time = 4500; f.live.recover(4500);
            check(!f.live.owned() && f.fake.count(OutputKind::PrimaryUp) == 1, "explicit recovery balances accepted drag only");
        });
        test("tap leaves owned down alive for queued own-caption messages until next sample", [] {
            Fixture f; f.press(); f.pad().buttons = 0; f.at(4488);
            SelfCaption caption;
            CaptionContext context{10, 10, 11, 1, 1, CaptionButton::Minimize, true,
                (f.live.owned() & Primary) != 0, false};
            check(caption.down(context, true) && caption.pending(), "queued down can use owned ledger despite released physical A");
            f.at(4504); context.primary_owned = (f.live.owned() & Primary) != 0;
            check(caption.up(context, true) && caption.take(context).has_value(), "processed matching up authorizes original caption action");
        });
        std::cout << passed << " release-tap model/regression groups passed; " << failed
            << " failed; no native input or hook used\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
    return failed ? 1 : 0;
}
