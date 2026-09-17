#pragma once

#include "Live.h"
#include <atomic>

namespace gamepad {
constexpr Tick navigation_policy_age_ms = 100;
constexpr Tick navigation_drain_ms = 500;
class NavigationShutdown {
public:
    void begin() noexcept { started_ = true; }
    [[nodiscard]] bool can_enable(bool requested) const noexcept { return requested && !started_; }
private:
    bool started_ = false;
};
class NavigationStopRequest {
public:
    // Single UI writer. Snapshot the published count before exposing stop to the
    // worker/callback; a lagging count may overreport loss but cannot hide new loss.
    void request(std::uint64_t published_abandoned) noexcept
    {
        if (requested_.load()) { return; }
        abandoned_before_.store(published_abandoned);
        requested_.store(true);
    }
    [[nodiscard]] bool requested() const noexcept { return requested_.load(); }
    [[nodiscard]] std::uint64_t abandoned_before() const noexcept { return abandoned_before_.load(); }
private:
    std::atomic<bool> requested_{false};
    std::atomic<std::uint64_t> abandoned_before_{0};
};
struct NavigationStopResult {
    bool worker_exited = false;
    bool hook_removed = false;
    std::uint32_t runtime_error = 0;
    unsigned pending = 0;
    std::uint64_t abandoned_before = 0, abandoned_after = 0;
    [[nodiscard]] bool clean() const noexcept
    {
        return worker_exited && hook_removed && !runtime_error && !pending && abandoned_after == abandoned_before;
    }
};
struct NavigationForeground {
    std::uintptr_t window = 0;
    std::uint32_t process = 0;
    std::uint32_t thread = 0;
    bool operator==(const NavigationForeground&) const = default;
    [[nodiscard]] bool known() const { return window && process && thread; }
};
enum class NavigationGate : unsigned {
    Unpublished, Desktop, Keyboard, Off, Settings, Prerequisites, StaleController, NativeMode,
    DesktopBlocked, KeyboardBlocked, Suspended
};
enum class NavigationPass : unsigned {
    None, FirstHold, PassedHold, Disabled, Expired, Foreground, Clock, Unknown, SourceAmbiguous, ScopeAbandoned
};
[[nodiscard]] std::wstring_view navigation_gate_text(NavigationGate reason) noexcept;
[[nodiscard]] std::wstring_view navigation_pass_text(NavigationPass reason) noexcept;
[[nodiscard]] bool navigation_settings_blocked(std::uintptr_t settings, NavigationForeground foreground) noexcept;
struct NavigationPolicy {
    NavigationForeground foreground;
    Tick issued = 0; // Native producer/consumer both use monotonic_now(), never a different uptime clock.
    std::uint64_t scope = 0;
    bool enabled = false;
    NavigationGate gate = NavigationGate::Unpublished;
    live::BlockReason block = live::BlockReason::None;
};
struct NavigationPolicyMailbox {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::atomic<std::uint64_t> version{0}, issued{0}, scope{0};
    std::atomic<std::uintptr_t> window{0};
    std::atomic<std::uint32_t> process{0}, thread{0};
    std::atomic<bool> enabled{false};
    std::atomic<NavigationGate> gate{NavigationGate::Unpublished};
    std::atomic<live::BlockReason> block{live::BlockReason::None};
    void publish(NavigationPolicy policy) noexcept;
    [[nodiscard]] NavigationPolicy read() const noexcept;
};
[[nodiscard]] bool navigation_filter_requested(bool saved_option, bool inspection, bool configuration_available) noexcept;
[[nodiscard]] bool navigation_policy_current(const NavigationPolicy& policy, NavigationForeground foreground, Tick now) noexcept;
[[nodiscard]] bool navigation_desktop_eligible(const live::Observation& observation, bool desktop_selected,
    bool prerequisites) noexcept;

// Only reserved GAMEPAD payloads enter this ledger. No ordinary keyboard state is retained.
struct NavigationEvent {
    unsigned key = 0;
    bool down = false;
    bool injected = false;
    bool own_output = false;
    std::uint32_t scan = 0;
    std::uintptr_t tag = 0;
    unsigned source_flags = 0;
};
struct NavigationCounts {
    std::uint64_t suppressed_down = 0, suppressed_up = 0, first_holds = 0;
    std::uint64_t quarantined = 0, abandoned = 0;
    std::array<std::uint64_t, 10> passed{};
    std::uint64_t source_conflicts = 0, foreground_abandons = 0, drain_abandons = 0;
    NavigationPass last_pass = NavigationPass::None;
    NavigationGate last_gate = NavigationGate::Unpublished;
    live::BlockReason last_block = live::BlockReason::None;
    Tick last_age = 0, last_at = 0;
};
class NavigationFilter {
public:
    bool event(NavigationEvent event, const NavigationPolicy& policy, NavigationForeground foreground, Tick now) noexcept;
    void update(const NavigationPolicy& policy, NavigationForeground foreground, Tick now) noexcept;
    void abandon() noexcept;
    [[nodiscard]] unsigned pending() const noexcept;
    [[nodiscard]] const NavigationCounts& counts() const noexcept { return counts_; }
private:
    enum class Hold { Unknown, Released, Passed, Suppressed, Abandoned, Quarantined };
    struct Entry {
        Hold hold = Hold::Unknown;
        NavigationForeground foreground;
        std::uint64_t scope = 0;
        std::uint32_t scan = 0;
        std::uintptr_t tag = 0;
        unsigned flags = 0;
        std::optional<Tick> drain_started;
    };
    void quarantine(Entry& entry) noexcept;
    void abandon_scope(Entry& entry, bool foreground_changed) noexcept;
    void record_pass(NavigationPass reason, const NavigationPolicy& policy, Tick now) noexcept;
    std::array<Entry, 24> entries_{};
    NavigationCounts counts_;
};
} // namespace gamepad
