#include "NavigationFilter.h"

namespace overworld {
std::wstring_view navigation_gate_text(NavigationGate reason) noexcept
{
    switch (reason) {
    case NavigationGate::Unpublished: return L"not published";
    case NavigationGate::Desktop: return L"eligible desktop";
    case NavigationGate::Keyboard: return L"eligible keyboard";
    case NavigationGate::Off: return L"option off/inspection";
    case NavigationGate::Settings: return L"Settings foreground";
    case NavigationGate::Prerequisites: return L"observer/config/recovery prerequisite";
    case NavigationGate::StaleController: return L"stale controller sample";
    case NavigationGate::NativeMode: return L"native-mode preference";
    case NavigationGate::DesktopBlocked: return L"desktop eligibility blocked";
    case NavigationGate::KeyboardBlocked: return L"keyboard target blocked";
    case NavigationGate::Suspended: return L"explicit suspension";
    }
    return L"unknown";
}
std::wstring_view navigation_pass_text(NavigationPass reason) noexcept
{
    switch (reason) {
    case NavigationPass::None: return L"none";
    case NavigationPass::FirstHold: return L"first unobserved hold";
    case NavigationPass::PassedHold: return L"repeat of passed hold";
    case NavigationPass::Disabled: return L"published policy disabled";
    case NavigationPass::Expired: return L"policy expired";
    case NavigationPass::Foreground: return L"foreground mismatch";
    case NavigationPass::Clock: return L"clock rollback";
    case NavigationPass::Unknown: return L"unknown foreground/policy";
    case NavigationPass::SourceAmbiguous: return L"source ambiguity quarantine";
    case NavigationPass::ScopeAbandoned: return L"abandoned hold awaiting release";
    }
    return L"unknown";
}
bool navigation_settings_blocked(std::uintptr_t settings, NavigationForeground foreground) noexcept
{
    return settings && (!foreground.known() || settings == foreground.window);
}

void NavigationPolicyMailbox::publish(NavigationPolicy policy) noexcept
{
    version.fetch_add(1);
    enabled.store(policy.enabled);
    window.store(policy.foreground.window); process.store(policy.foreground.process); thread.store(policy.foreground.thread);
    issued.store(policy.issued); scope.store(policy.scope);
    gate.store(policy.gate); block.store(policy.block);
    version.fetch_add(1);
}
NavigationPolicy NavigationPolicyMailbox::read() const noexcept
{
    const auto before = version.load();
    if (before & 1) { return {}; }
    NavigationPolicy result{{window.load(), process.load(), thread.load()}, issued.load(), scope.load(), enabled.load(),
        gate.load(), block.load()};
    if (before != version.load()) { result.enabled = false; }
    return result;
}
bool navigation_filter_requested(bool saved_option, bool inspection, bool configuration_available) noexcept
{
    return saved_option && !inspection && configuration_available;
}

bool navigation_policy_current(const NavigationPolicy& policy, NavigationForeground foreground, Tick now) noexcept
{
    return policy.enabled && policy.foreground.known() && policy.foreground == foreground
        && now >= policy.issued && now - policy.issued <= navigation_policy_age_ms;
}
bool navigation_desktop_eligible(const live::Observation& observation, bool desktop_selected, bool prerequisites) noexcept
{
    return prerequisites && desktop_selected && observation.available && observation.allowed
        && observation.baseline_known && observation.routing_stable && !observation.physical_held
        && observation.mode != live::Mode::Blocked
        && observation.block == live::BlockReason::None;
}
void NavigationFilter::quarantine(Entry& entry) noexcept
{
    if (entry.hold == Hold::Quarantined) { return; }
    ++counts_.quarantined;
    ++counts_.source_conflicts;
    if (entry.hold == Hold::Suppressed) { ++counts_.abandoned; }
    entry = {};
    entry.hold = Hold::Quarantined;
}
void NavigationFilter::abandon_scope(Entry& entry, bool foreground_changed) noexcept
{
    ++counts_.abandoned;
    if (foreground_changed) { ++counts_.foreground_abandons; }
    else { ++counts_.drain_abandons; }
    entry.hold = Hold::Abandoned;
    entry.drain_started.reset();
}
void NavigationFilter::record_pass(NavigationPass reason, const NavigationPolicy& policy, Tick now) noexcept
{
    ++counts_.passed[static_cast<unsigned>(reason)];
    counts_.last_pass = reason;
    counts_.last_gate = policy.gate;
    counts_.last_block = policy.block;
    counts_.last_age = now >= policy.issued ? now - policy.issued : 0;
    counts_.last_at = now;
}
void NavigationFilter::update(const NavigationPolicy& policy, NavigationForeground foreground, Tick now) noexcept
{
    const bool eligible = navigation_policy_current(policy, foreground, now);
    for (auto& entry : entries_) {
        if (entry.hold != Hold::Suppressed) { continue; }
        if (entry.foreground != foreground) {
            abandon_scope(entry, true); // Pass until a matching release, never suppress in a different foreground.
            continue;
        }
        if ((!eligible || policy.scope != entry.scope) && !entry.drain_started) { entry.drain_started = now; }
        if (entry.drain_started && (now < *entry.drain_started || now - *entry.drain_started >= navigation_drain_ms)) {
            abandon_scope(entry, false);
        }
    }
}
bool NavigationFilter::event(NavigationEvent event, const NavigationPolicy& policy,
    NavigationForeground foreground, Tick now) noexcept
{
    if (event.key < 0xc3 || event.key > 0xda) { return false; }
    update(policy, foreground, now);
    auto& entry = entries_[event.key - 0xc3];
    if (!event.injected || event.own_output) {
        quarantine(entry);
        if (event.down) { record_pass(NavigationPass::SourceAmbiguous, policy, now); }
        return false;
    }
    if (entry.hold == Hold::Quarantined) {
        if (event.down) { record_pass(NavigationPass::SourceAmbiguous, policy, now); }
        return false;
    }
    if ((entry.hold == Hold::Passed || entry.hold == Hold::Suppressed || entry.hold == Hold::Abandoned)
        && (entry.scan != event.scan || entry.tag != event.tag || entry.flags != event.source_flags)) {
        quarantine(entry);
        if (event.down) { record_pass(NavigationPass::SourceAmbiguous, policy, now); }
        return false;
    }
    if (!event.down) {
        const bool suppress = entry.hold == Hold::Suppressed;
        entry = {};
        entry.hold = Hold::Released;
        if (suppress) { ++counts_.suppressed_up; }
        return suppress;
    }
    if (entry.hold == Hold::Abandoned) {
        record_pass(NavigationPass::ScopeAbandoned, policy, now);
        return false;
    }
    if (entry.hold == Hold::Unknown) { ++counts_.first_holds; }
    const bool suppress = entry.hold == Hold::Suppressed
        || (entry.hold == Hold::Released && navigation_policy_current(policy, foreground, now));
    if (!suppress) {
        const auto reason = !policy.enabled ? NavigationPass::Disabled
            : !policy.foreground.known() || !foreground.known() ? NavigationPass::Unknown
            : policy.foreground != foreground ? NavigationPass::Foreground
            : now < policy.issued ? NavigationPass::Clock
            : now - policy.issued > navigation_policy_age_ms ? NavigationPass::Expired
            : entry.hold == Hold::Unknown ? NavigationPass::FirstHold : NavigationPass::PassedHold;
        record_pass(reason, policy, now);
    }
    if (entry.hold != Hold::Suppressed) {
        entry.foreground = foreground;
        entry.scope = policy.scope;
        entry.scan = event.scan;
        entry.tag = event.tag;
        entry.flags = event.source_flags;
    }
    entry.hold = suppress ? Hold::Suppressed : Hold::Passed;
    if (suppress) { ++counts_.suppressed_down; }
    return suppress;
}
unsigned NavigationFilter::pending() const noexcept
{
    unsigned count = 0;
    for (const auto& entry : entries_) { if (entry.hold == Hold::Suppressed) { ++count; } }
    return count;
}
void NavigationFilter::abandon() noexcept
{
    for (auto& entry : entries_) { if (entry.hold == Hold::Suppressed) { abandon_scope(entry, false); } }
}
} // namespace overworld
