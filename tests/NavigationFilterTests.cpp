#include "NavigationFilter.h"
#include "HostShutdown.h"

#include <iostream>
#include <stdexcept>

using namespace overworld;
namespace {
unsigned checks = 0;
void check(bool value, const char* message)
{
    ++checks;
    if (!value) { throw std::runtime_error(message); }
}
constexpr NavigationForeground original{10, 100, 1000};
constexpr NavigationForeground different{20, 200, 2000};
NavigationPolicy policy(Tick now = 1000, bool enabled = true, std::uint64_t scope = 1)
{
    return {original, now, scope, enabled};
}
NavigationEvent key(unsigned value, bool down, bool injected = true, bool own = false)
{
    return {value, down, injected, own, 0, 123, 0};
}
}
int main()
{
    try {
        {
            HostShutdown host;
            NavigationShutdown navigation;
            const bool saved_option = true;
            check(host.allows_work() && navigation.can_enable(saved_option), "normal saved opt-in is initially usable");
            check(host.begin(), "first close commits");
            navigation.begin();
            unsigned mapped_steps = 0, hook_starts = 0, settings_applies = 0;
            // Fake the timer and Settings commands dispatched by a modal close warning.
            for (unsigned i = 0; i < 100; ++i) {
                if (host.allows_work()) { ++mapped_steps; ++settings_applies; }
                if (navigation.can_enable(saved_option)) { ++hook_starts; }
            }
            check(!mapped_steps && !hook_starts && !settings_applies,
                "modal close pumping must not restart hooks, mapping or settings despite saved option");
            check(!host.begin(), "nested WM_CLOSE must not start a second close");
            host.finish();
            host.veto();
            check(!host.allows_work() && !navigation.can_enable(saved_option), "completed shutdown cannot be revived");
        }
        {
            HostShutdown host;
            NavigationShutdown navigation;
            check(host.begin(), "cursor-recovery veto fixture");
            check(!host.allows_work(), "no work while cursor warning pumps messages");
            host.veto(); // The native filter's final shutdown has not been called on this path.
            check(host.allows_work() && navigation.can_enable(true), "veto permits explicit recovery, not terminal filter shutdown");
            check(host.begin(), "a later explicit close can retry");
            navigation.begin();
            host.finish();
            check(!host.allows_work() && !navigation.can_enable(true), "successful retry stays closed");
        }
        {
            for (const bool source_conflict : {false, true}) {
                NavigationFilter ledger;
                ledger.event(key(0xc3, false), policy(), original, 1000);
                check(ledger.event(key(0xc3, true), policy(), original, 1000), "post-stop race owns a hold");
                std::atomic<std::uint64_t> published{ledger.counts().abandoned};
                NavigationStopRequest stop;
                stop.request(published.load());
                check(stop.requested() && stop.abandoned_before() == 0, "stop publication must already expose its baseline");
                // Callback dispatch between stop publication and the next worker-loop top.
                if (source_conflict) {
                    auto mixed = key(0xc3, true); ++mixed.tag;
                    ledger.event(mixed, policy(1001, false), original, 1001);
                } else {
                    ledger.event(key(0xc3, true), policy(1001, false), different, 1001);
                }
                published.store(ledger.counts().abandoned);
                check(ledger.pending() == 0 && published.load() == 1, "callback must reproduce abandonment before loop observes stop");
                stop.request(published.load()); // Repeated disable/shutdown cannot move the baseline.
                check(stop.abandoned_before() == 0, "repeated stop must preserve the first published baseline");
                check(!NavigationStopResult{true, true, 0, ledger.pending(),
                    stop.abandoned_before(), published.load()}.clean(),
                    "clean unhook cannot conceal a post-stop callback abandonment");
            }
            NavigationStopRequest historical;
            historical.request(7);
            historical.request(9);
            check(historical.abandoned_before() == 7, "immutable baseline across repeated shutdown requests");
            check(NavigationStopResult{true, true, 0, 0, historical.abandoned_before(), 7}.clean(),
                "fully published historical losses alone still allow clean stop");
            check(!NavigationStopResult{true, true, 0, 0, historical.abandoned_before(), 8}.clean(),
                "in-flight unpublished pre-stop loss is conservatively reported, not silently included");
            check(!NavigationStopResult{true, true, 0, 1, historical.abandoned_before(), 7}.clean(),
                "pending work still fails stop despite historical-only counts");
            check(!NavigationStopResult{true, true, 5, 0, historical.abandoned_before(), 7}.clean(),
                "actual stop error remains a failure");
        }
        {
            NavigationStopResult stopped{true, true, 0, 0, 7, 7};
            check(stopped.clean(), "historical abandonment alone must not turn successful stop/unhook into failure");
            auto failed = stopped; failed.runtime_error = 5;
            check(!failed.clean(), "actual runtime/unhook failure must remain visible");
            failed = stopped; failed.worker_exited = false;
            check(!failed.clean(), "shutdown wait timeout cannot be called a clean exit");
            failed = stopped; failed.hook_removed = false;
            check(!failed.clean(), "failed/unknown unhook must remain a shutdown failure");
            failed = stopped; failed.pending = 1;
            check(!failed.clean(), "pending suppressed work cannot be hidden by old counters");
            failed = stopped; ++failed.abandoned_after;
            check(!failed.clean(), "new abandonment during this stop must still be reported");
            NavigationFilter ledger;
            ledger.event(key(0xc3, false), policy(), original, 1000);
            ledger.event(key(0xc3, true), policy(), original, 1000);
            ledger.update(policy(), different, 1001);
            const auto history = ledger.counts().abandoned;
            ledger.abandon();
            check(NavigationStopResult{true, true, 0, ledger.pending(), history, ledger.counts().abandoned}.clean()
                && ledger.counts().abandoned == 1, "clean stop must retain actual historical evidence rather than zero it");
        }
        for (unsigned value = 0; value < 256; ++value) {
            for (unsigned flags = 0; flags < 4; ++flags) {
                NavigationFilter f;
                const bool injected = (flags & 1) != 0, own = (flags & 2) != 0;
                const bool eligible = value >= 0xc3 && value <= 0xda && injected && !own;
                check(!f.event(key(value, false, injected, own), policy(1000, false), original, 1000), "up before any down must pass");
                check(f.event(key(value, true, injected, own), policy(), original, 1000) == eligible, "only injected untagged GAMEPAD downs suppress");
                check(f.event(key(value, true, injected, own), policy(1001, false), original, 1001) == eligible, "repeat follows original down");
                check(f.event(key(value, false, injected, own), policy(1002, false), original, 1002) == eligible, "disable drains only its suppressed up");
                check(f.pending() == 0, "balanced up leaves no suppressed hold");
                if (value < 0xc3 || value > 0xda) {
                    check(f.counts().first_holds == 0 && f.counts().suppressed_down == 0 && f.counts().quarantined == 0,
                        "ordinary/NAVIGATION/VK_PACKET input never enters diagnostics or ledger");
                }
            }
        }
        for (unsigned value = 0xc3; value <= 0xda; ++value) {
            NavigationFilter f;
            check(!f.event(key(value, true), policy(), original, 1000), "unknown startup hold must pass");
            check(!f.event(key(value, true), policy(), original, 1000), "unknown hold repeat must pass");
            check(!f.event(key(value, false), policy(), original, 1000), "never swallow up for a passed down");
            check(f.counts().first_holds == 1, "one uncertainty diagnostic per startup hold");
            check(f.event(key(value, true), policy(), original, 1000), "observed release arms next hold");
            check(f.event(key(value, true), policy(), original, 1000), "suppressed repeat remains owned");
            check(f.event(key(value, false), policy(), original, 1000), "matching up swallowed");
            check(f.pending() == 0 && f.counts().suppressed_down == 2 && f.counts().suppressed_up == 1, "repeat and up counters exact");
        }
        for (unsigned part = 0; part < 5; ++part) {
            NavigationFilter f;
            f.event(key(0xc3, false), policy(), original, 1000);
            check(f.event(key(0xc3, true), policy(), original, 1000), "mixed-source precondition");
            auto mixed = key(0xc3, true);
            if (part == 0) { mixed.injected = false; }
            if (part == 1) { mixed.own_output = true; }
            if (part == 2) { ++mixed.scan; }
            if (part == 3) { ++mixed.tag; }
            if (part == 4) { ++mixed.source_flags; }
            check(!f.event(mixed, policy(), original, 1000), "observable same-key source ambiguity must fail open");
            check(!f.event(key(0xc3, false), policy(), original, 1000), "ambiguous up cannot be swallowed");
            check(!f.event(key(0xc3, true), policy(), original, 1000), "quarantine persists until fresh runtime");
            check(f.pending() == 0 && f.counts().quarantined == 1 && f.counts().abandoned == 1,
                "ambiguous owned hold is diagnosed rather than invented pairing");
        }
        {
            NavigationFilter f;
            f.event(key(0xc3, false), policy(), original, 1000);
            check(f.event(key(0xc3, true), policy(), original, 1000), "scope precondition");
            check(!f.event(key(0xc3, true), policy(), different, 1001), "foreground change must not suppress in another app");
            check(!f.event(key(0xc3, false), policy(), different, 1002), "foreground-switch up fails open");
            check(f.counts().abandoned == 1, "scope excursion diagnosed");
        }
        {
            NavigationFilter f;
            f.event(key(0xc3, false), policy(), original, 1000);
            check(!f.event(key(0xc3, true), policy(), original, 1101), "stale policy cannot suppress new hold");
            check(!f.event(key(0xc3, true), policy(1102), original, 1102), "refresh cannot convert passed hold to suppressed");
            check(!f.event(key(0xc3, false), policy(1103), original, 1103), "stale-start hold up passes");
            check(f.event(key(0xc3, true), policy(1104), original, 1104), "next fresh hold can suppress");
            f.update(policy(1105, false), original, 1105);
            check(f.event(key(0xc3, true), policy(1604, true, 2), original, 1604), "existing source drains boundedly despite quick reenable");
            check(!f.event(key(0xc3, false), policy(1605, true, 2), original, 1605), "500ms drain expiry must fail open");
            check(f.counts().abandoned == 1 && f.pending() == 0, "drain deadline cannot be renewed by policy refresh");
        }
        {
            NavigationFilter f;
            f.event(key(0xc3, false), policy(), original, 1000);
            check(f.event(key(0xc3, true), policy(), original, 1000), "stop precondition");
            f.abandon();
            check(!f.event(key(0xc3, false), policy(1001, false), original, 1001) && f.pending() == 0,
                "forced bounded shutdown abandons rather than injecting output or swallowing later up");
        }
        {
            check(navigation_policy_current(policy(), original, 1100), "100ms lease boundary inclusive");
            check(!navigation_policy_current(policy(), original, 1101), "expired lease rejected");
            check(!navigation_policy_current(policy(), original, 999), "clock rollback rejected");
            for (const NavigationForeground bad : {NavigationForeground{}, NavigationForeground{10, 101, 1000},
                NavigationForeground{10, 100, 1001}, different}) {
                check(!navigation_policy_current(policy(), bad, 1000), "full foreground identity required");
            }
        }
        {
            NavigationFilter f;
            f.event(key(0xc3, false), policy(), original, 1000);
            check(f.event(key(0xc3, true), policy(), original, 1000), "scope recovery precondition");
            check(!f.event(key(0xc3, false), policy(1001), different, 1001),
                "an abandoned up must pass in the new foreground");
            check(f.event(key(0xc3, true), policy(1002), original, 1002),
                "a completed scope-only interruption must not permanently disable this key");
            check(f.counts().quarantined == 0 && f.counts().foreground_abandons == 1,
                "scope abandonment must be distinguished from ambiguous-source quarantine");
        }
        {
            NavigationFilter f;
            f.event(key(0xc4, false), policy(), original, 1000);
            check(f.event(key(0xc4, true), policy(), original, 1000), "drain recovery precondition");
            f.update(policy(1001, false), original, 1001);
            f.update(policy(1501, false), original, 1501);
            check(!f.event(key(0xc4, true), policy(1502), original, 1502),
                "abandoned repeat must still pass even after renewed eligibility");
            check(!f.event(key(0xc4, false), policy(1503), original, 1503),
                "abandoned matching up must pass");
            check(f.event(key(0xc4, true), policy(1504), original, 1504),
                "matching release must allow a new safe hold after bounded drain");
            check(f.counts().drain_abandons == 1 && f.counts().quarantined == 0,
                "deadline alone cannot poison the key for the entire session");
        }
        {
            NavigationFilter f;
            f.event(key(0xc3, false), policy(), original, 1000);
            f.event(key(0xc3, true), policy(), original, 1000);
            f.update(policy(1001), different, 1001);
            auto different_source = key(0xc3, false); ++different_source.tag;
            check(!f.event(different_source, policy(1002), different, 1002),
                "different-source up cannot rearm scope-abandoned hold");
            check(!f.event(key(0xc3, true), policy(1003), original, 1003)
                && f.counts().source_conflicts == 1 && f.counts().quarantined == 1,
                "actual source ambiguity must remain run-long quarantine");
        }
        {
            check(navigation_settings_blocked(original.window, original),
                "foreground Settings must keep its native navigation");
            check(!navigation_settings_blocked(different.window, original),
                "modeless Settings behind the verified target must not disable filtering globally");
            check(!navigation_settings_blocked(0, original), "destroyed Settings cannot block filtering");
            check(navigation_settings_blocked(different.window, {}), "unknown target must remain blocked");
        }
        {
            NavigationFilter f;
            auto blocked = policy(1000, false);
            blocked.gate = NavigationGate::Settings;
            f.event(key(0xc3, false), blocked, original, 1000);
            f.event(key(0xc3, true), blocked, original, 1042);
            check(f.counts().last_pass == NavigationPass::Disabled && f.counts().last_gate == NavigationGate::Settings
                && f.counts().last_age == 42, "missed GAMEPAD down must retain actual gate and publication age");
            f.update(policy(1050), different, 1050);
            f.event(key(0x41, true), policy(1051), different, 1051);
            check(f.counts().last_at == 1042 && f.counts().last_gate == NavigationGate::Settings,
                "timer updates, opening Details and ordinary keys must not erase retained missed-down evidence");
            f.event(key(0xc3, false), policy(1052), original, 1052);
            auto expired = policy(1053); expired.gate = NavigationGate::Desktop;
            f.event(key(0xc3, true), expired, original, 1154);
            check(f.counts().last_pass == NavigationPass::Expired
                && f.counts().passed[static_cast<unsigned>(NavigationPass::Expired)] == 1,
                "expired-policy failure needs its own counter");
            f.event(key(0xc3, false), policy(1155), original, 1155);
            f.event(key(0xc3, true), policy(1156), different, 1156);
            check(f.counts().last_pass == NavigationPass::Foreground
                && f.counts().passed[static_cast<unsigned>(NavigationPass::Disabled)] == 1,
                "foreground mismatch must not be mistaken for option-off");
        }
        {
            // Reproduce the observed publisher-ahead/coarse-consumer clock mismatch.
            // The future guard is correct; both native sites must instead share one clock.
            NavigationPolicyMailbox mailbox;
            mailbox.publish(policy(1007));
            NavigationFilter mixed;
            mixed.event(key(0xc3, false), mailbox.read(), original, 1000);
            check(!mixed.event(key(0xc3, true), mailbox.read(), original, 1000),
                "a timestamp from an ahead clock must still fail the future guard");
            check(mixed.counts().last_pass == NavigationPass::Clock,
                "mixed clock domains must reproduce the captured clock-rollback category");
            NavigationFilter shared;
            const auto captured = mailbox.read();
            const Tick after_snapshot = 1007;
            shared.event(key(0xc3, false), captured, original, after_snapshot);
            check(shared.event(key(0xc3, true), captured, original, after_snapshot),
                "shared clock after the policy snapshot accepts the immediately eligible hold");
            check(shared.event(key(0xc3, false), captured, original, after_snapshot + 1),
                "shared clock preserves matching up suppression");
            check(navigation_policy_current(captured, original, 1107),
                "pre-validation issuance retains the inclusive 100 ms lease");
            check(!navigation_policy_current(captured, original, 1108),
                "slow validation must not renew or extend the original lease");
        }
        {
            NavigationPolicyMailbox mailbox;
            mailbox.publish(policy(2000));
            const Tick before_publication = 2000;
            mailbox.publish(policy(2001));
            check(!navigation_policy_current(mailbox.read(), original, before_publication),
                "sampling time before reading a concurrently newer policy creates a false future");
            const auto captured = mailbox.read();
            mailbox.publish(policy(2002));
            const Tick after_snapshot = 2002;
            check(navigation_policy_current(captured, original, after_snapshot),
                "explicit snapshot-before-time ordering is safe during concurrent publication");
            NavigationFilter f;
            f.event(key(0xc3, false), captured, original, after_snapshot);
            check(f.event(key(0xc3, true), captured, original, after_snapshot), "drain test owns a hold");
            const auto off = policy(2003, false);
            f.update(off, original, 2003);
            f.update(off, original, 2502);
            check(f.pending() == 1, "shared-clock drain remains bounded and active at 499 ms");
            f.update(off, original, 2503);
            check(f.pending() == 0 && f.counts().drain_abandons == 1,
                "shared-clock drain expires at exactly 500 ms");
        }
        {
            NavigationPolicyMailbox mailbox;
            check(!mailbox.read().enabled, "unpublished policy must fail open");
            mailbox.publish(policy());
            check(navigation_policy_current(mailbox.read(), original, 1000), "published snapshot works");
            const auto stable = mailbox.version.load();
            mailbox.version.store(stable + 1);
            check(!mailbox.read().enabled, "interrupted publication must return immediately with no authorization");
            mailbox.version.store(stable + 2);
            mailbox.publish(policy(1010));
            check(mailbox.read().scope == 1 && mailbox.read().issued == 1010, "freshness-only update preserves logical scope");
            auto blocked = policy(1011, false);
            blocked.gate = NavigationGate::DesktopBlocked; blocked.block = overworld::live::BlockReason::Exclusion;
            mailbox.publish(blocked);
            check(mailbox.read().gate == NavigationGate::DesktopBlocked
                && mailbox.read().block == overworld::live::BlockReason::Exclusion,
                "published reason metadata must travel with its authorization snapshot");
            for (unsigned combination = 0; combination < 8; ++combination) {
                const bool saved = (combination & 1) != 0, inspection = (combination & 2) != 0, configured = (combination & 4) != 0;
                check(navigation_filter_requested(saved, inspection, configured) == (saved && !inspection && configured),
                    "inspection/configuration must prohibit hook lifecycle even for saved opt-in");
            }
        }
        {
            using namespace overworld::live;
            Observation o;
            o.available = o.allowed = o.baseline_known = o.routing_stable = true;
            o.mode = Mode::Pointer;
            check(navigation_desktop_eligible(o, true, true), "eligible neutral desktop must not require controller_active");
            check(!navigation_desktop_eligible(o, false, true), "manual native passes new holds");
            check(!navigation_desktop_eligible(o, true, false), "inspection/settings/recovery prerequisite denies");
            for (const auto reason : {BlockReason::Auto, BlockReason::Exclusion, BlockReason::Recipient,
                BlockReason::DesktopSession, BlockReason::AutoUnavailable, BlockReason::AutoNotPresent,
                BlockReason::Routing, BlockReason::Physical, BlockReason::Settling, BlockReason::TextOutput}) {
                auto blocked = o; blocked.block = reason;
                check(!navigation_desktop_eligible(blocked, true, true), "structured block reason cannot authorize suppression");
            }
            auto held = o; held.physical_held = Other;
            check(!navigation_desktop_eligible(held, true, true), "physical overlap passes new holds");
            auto unknown = o; unknown.baseline_known = false;
            check(!navigation_desktop_eligible(unknown, true, true), "unknown physical baseline passes new holds");
            auto stale = o; stale.routing_stable = false;
            check(!navigation_desktop_eligible(stale, true, true), "unstable route passes new holds");
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS " << checks << " navigation filter checks; native hook adapter not linked or executed\n";
}
