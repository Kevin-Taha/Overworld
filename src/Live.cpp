#include "Live.h"

#include <cmath>

#include <Windows.h>
#include <utility>

namespace gamepad::live {
bool known_app(AppIdentity app) { return app.root_owner && app.process; }
bool heuristic_auto(AutoReason reason)
{
    return reason == AutoReason::ExclusiveD3D || reason == AutoReason::Presentation || reason == AutoReason::Confinement;
}
std::wstring_view block_reason_text(BlockReason reason)
{
    switch (reason) {
    case BlockReason::None: return L"Eligible";
    case BlockReason::Infrastructure: return L"Input observers or prerequisites unavailable";
    case BlockReason::DesktopSession: return L"Normal unlocked desktop/session required";
    case BlockReason::Routing: return L"Recipient routing is unknown or changing";
    case BlockReason::Recipient: return L"Recipient session, integrity or UIAccess unsupported";
    case BlockReason::Exclusion: return L"Explicit executable exclusion blocks desktop control";
    case BlockReason::Surface: return L"Recipient hidden, minimized or disabled";
    case BlockReason::Geometry: return L"Desktop geometry or cursor unsupported";
    case BlockReason::SwappedButtons: return L"Swapped mouse buttons unsupported";
    case BlockReason::Physical: return L"Physical mouse or keyboard has control";
    case BlockReason::Settling: return L"Accepted key-up is waiting for asynchronous state to settle";
    case BlockReason::Auto: return L"Auto is yielding; Menu+View requests desktop for this app";
    case BlockReason::AutoUnavailable: return L"Auto observation unavailable; desktop override not granted";
    case BlockReason::AutoNotPresent: return L"Windows reports no active interactive desktop";
    case BlockReason::TextOutput: return L"Text output failed; a new desktop request is required";
    }
    return L"Unknown eligibility reason";
}
InputCategory input_category(unsigned key, bool owned)
{
    if (owned) { return InputCategory::Owned; }
    if (!key || key >= 256) { return InputCategory::None; }
    if (key >= VK_NAVIGATION_VIEW && key <= VK_NAVIGATION_CANCEL) { return InputCategory::ReservedNavigation; }
    if (key >= 0xc3 && key <= 0xda) { return InputCategory::ReservedGamepad; }
    if (key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2) {
        return InputCategory::Mouse;
    }
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN
        || (key >= VK_LSHIFT && key <= VK_RMENU)) { return InputCategory::Modifier; }
    return InputCategory::Keyboard;
}
std::wstring input_diagnostic_text(InputCategory category, bool raw, bool asynchronous, bool settling)
{
    std::wstring_view name = L"none";
    switch (category) {
    case InputCategory::None: break;
    case InputCategory::Keyboard: name = L"keyboard"; break;
    case InputCategory::Modifier: name = L"modifier"; break;
    case InputCategory::Mouse: name = L"mouse"; break;
    case InputCategory::ReservedGamepad: name = L"reserved-gamepad"; break;
    case InputCategory::ReservedNavigation: name = L"reserved-navigation"; break;
    case InputCategory::Owned: name = L"owned"; break;
    }
    return L"input=" + std::wstring{name} + L"; raw=" + std::to_wstring(raw)
        + L"; async=" + std::to_wstring(asynchronous) + L"; up-settling=" + std::to_wstring(settling);
}
bool physical_async_key(unsigned key)
{
    return key > 0 && key < 256 && !(key >= 0xc3 && key <= 0xda)
        && !(key >= VK_NAVIGATION_VIEW && key <= VK_NAVIGATION_CANCEL);
}
bool confinement_candidate(std::uint32_t foreground, std::uint32_t shell, std::uint32_t self)
{
    return foreground && foreground != self && (!shell || foreground != shell);
}
void ReleasedKeyState::accepted_up(unsigned key, Tick now)
{
    if (key < released_.size()) { released_[key] = now; }
}
KeyState ReleasedKeyState::observe(unsigned key, bool asynchronous, bool physical, Tick now)
{
    if (key >= released_.size()) { return KeyState::Physical; }
    auto& released = released_[key];
    if (physical) { released.reset(); return KeyState::Physical; }
    if (!physical_async_key(key) || !asynchronous) { released.reset(); return KeyState::Clear; }
    if (released && now >= *released && now - *released < live_sample_age_ms) { return KeyState::Settling; }
    released.reset();
    return KeyState::Physical;
}

AppActivation OutputBoundary::observe_activation(AppIdentity identity)
{
    if (!known_app(identity) || identity != activation_.identity) {
        activation_.identity = identity;
        ++activation_.generation;
        override_.reset();
    }
    return activation_;
}
void OutputBoundary::reset_activation()
{
    activation_.identity = {};
    ++activation_.generation;
    override_.reset();
}
void OutputBoundary::clear_auto_override() { override_.reset(); }
bool OutputBoundary::auto_override_active() const
{
    return override_ && *override_ == activation_ && known_app(activation_.identity);
}
void OutputBoundary::apply_auto_policy(Observation& observation)
{
    observation.auto_overridden = auto_override_active() && observation.app == activation_;
    if (observation.block == BlockReason::Auto && heuristic_auto(observation.automatic)
        && observation.auto_overridden) {
        observation.allowed = true;
        observation.block = observation.physical_held ? BlockReason::Physical : BlockReason::None;
        observation.reason = L"Desktop requested for this app activation (Auto override)";
    }
}
bool OutputBoundary::request_desktop(const Observation& expected, Tick expires)
{
    const auto eligible = [](const Observation& value) {
        return value.available && value.baseline_known && value.cleanup_allowed && value.routing_stable
            && !value.physical_held && known_app(value.app.identity)
            && (value.allowed || (value.block == BlockReason::Auto && heuristic_auto(value.automatic)));
    };
    if (!eligible(expected) || now() > expires) { return false; }
    const auto current = observe(0);
    if (!eligible(current) || current.app != expected.app || current.context != expected.context
        || current.activity != expected.activity || current.routing != expected.routing || now() > expires) { return false; }
    override_ = current.app;
    return true;
}

bool fullscreen_pointer_confined(Rect window, Rect monitor, Rect clip, Rect desktop)
{
    return valid(window) && valid(monitor) && valid(clip) && valid(desktop) && clip != desktop
        && window.left <= monitor.left && window.top <= monitor.top
        && window.right >= monitor.right && window.bottom >= monitor.bottom
        && contains(monitor, {clip.left, clip.top}) && contains(monitor, {clip.right, clip.bottom});
}

void GameHandoff::foreground_changed(std::uintptr_t foreground)
{
    if (foreground != foreground_) { foreground_ = foreground; pointer_game_ = false; }
}

void GameHandoff::reset()
{
    foreground_ = 0;
    pointer_game_ = false;
}

std::wstring_view GameHandoff::update(bool automatic, std::uintptr_t foreground,
                                     NotificationState state, bool confined_fullscreen)
{
    foreground_changed(foreground);
    if (!automatic || !foreground) { pointer_game_ = false; return {}; }
    pointer_game_ = pointer_game_ || confined_fullscreen;
    if (const auto reason = notification_pause_reason(state); !reason.empty()) { return reason; }
    return pointer_game_ ? L"Yielding to a fullscreen app that confined the pointer (Auto)" : std::wstring_view{};
}

std::wstring_view notification_pause_reason(NotificationState state)
{
    switch (state) {
    case NotificationState::ExclusiveD3D: return L"Yielding to an exclusive full-screen Direct3D application";
    case NotificationState::Presentation: return L"Yielding to Windows presentation mode";
    case NotificationState::NotPresent: return L"Waiting for an active interactive desktop";
    case NotificationState::Busy: // Fullscreen video/browser is not evidence of a game.
    case NotificationState::Available:
    case NotificationState::QuietTime:
    case NotificationState::StoreApp: return {};
    }
    return L"Automatic waiting - unsupported Windows notification state";
}

namespace {
unsigned down_bit(OutputKind kind)
{
    switch (kind) {
    case OutputKind::PrimaryDown: return Primary;
    case OutputKind::SecondaryDown: return Secondary;
    case OutputKind::EscapeDown: return Escape;
    default: return 0;
    }
}

unsigned up_bit(OutputKind kind)
{
    switch (kind) {
    case OutputKind::PrimaryUp: return Primary;
    case OutputKind::SecondaryUp: return Secondary;
    case OutputKind::EscapeUp: return Escape;
    default: return 0;
    }
}
} // namespace

bool executable_excluded(std::wstring_view basename, const ExecutableExclusions& names)
{
    // Windows ordinal comparison is independent of the user's C/C++ locale.
    if (basename.empty() || basename.size() > 259) { return false; }
    for (const auto& name : names) {
        if (CompareStringOrdinal(basename.data(), static_cast<int>(basename.size()),
                                 name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

std::wstring_view validate_exclusions(const ExecutableExclusions& names)
{
    if (names.size() > 128) { return L"Use at most 128 executable basenames"; }
    for (const auto& name : names) {
        if (name.size() < 5 || name.size() > 259 || name.front() == L' ' || name.back() == L' '
            || CompareStringOrdinal(name.data() + name.size() - 4, 4, L".exe", 4, TRUE) != CSTR_EQUAL) {
            return L"Exclusions must be executable basenames ending in .exe (5-259 characters, no surrounding spaces)";
        }
        for (const auto c : name) {
            if (c < L' ' || c == 127 || std::wstring_view{L"<>:\"/\\|?*"}.find(c) != std::wstring_view::npos) {
                return L"Use an executable basename only, without paths, wildcards, or control characters";
            }
        }
    }
    return {};
}

bool same_context(const Observation& a, const Observation& b)
{
    return a.context == b.context && a.activity == b.activity
        && a.bounds == b.bounds && a.desktop == b.desktop;
}

unsigned physical_holds(unsigned asynchronous, unsigned observed_physical, unsigned owned)
{
    return (asynchronous & ~owned) | (observed_physical & owned);
}

bool process_integrity_allowed(unsigned host, unsigned target, bool ui_access)
{
    return !ui_access && (host == SECURITY_MANDATORY_MEDIUM_RID || host == SECURITY_MANDATORY_HIGH_RID)
        && target >= SECURITY_MANDATORY_LOW_RID && target <= host;
}

bool mouse_report_activity(bool absolute, long x, long y, unsigned buttons)
{
    return absolute || x != 0 || y != 0 || buttons != 0;
}

bool routing_query_disappeared(unsigned error, bool missing_window)
{
    return error == ERROR_INVALID_WINDOW_HANDLE || (missing_window
        && (error == ERROR_SUCCESS || error == ERROR_INVALID_HANDLE || error == ERROR_INVALID_PARAMETER));
}

namespace {
bool routing_wait_expired(std::optional<Tick> since, Tick now)
{
    return since && (now < *since || now - *since >= live_sample_age_ms);
}

bool pending_route(const Observation& observation)
{
    return observation.available && observation.routing_pending && observation.block == BlockReason::Routing
        && !observation.allowed && !observation.routing_stable && !observation.physical_held;
}
}

Emission Dispatcher::release(OutputKind kind, unsigned bit)
{
    if (!(owned_ & bit)) { return Emission::Accepted; }
    const Output output{kind};
    const auto current = boundary_.observe(owned_, &output);
    // Cleanup is deliberately independent of new-action eligibility and enablement.
    if (current.physical_held & bit) { return Emission::Blocked; }
    // A routing gap is not an OS rejection. Keep the accepted down and retry only
    // after its release context is trusted, rather than latching a permanent fault.
    if (!current.cleanup_allowed || !current.baseline_known) { return Emission::Deferred; }
    if (!current.routing_stable) { return Emission::Deferred; }
    const auto accepted = boundary_.send(output, current.desktop);
    if (accepted == 1) {
        owned_ &= ~bit;
        return Emission::Accepted;
    }
    if (accepted > 1) { uncertain_ = true; }
    return Emission::Blocked;
}

Emission Dispatcher::emit(const Output& output, const Observation& expected, std::optional<Tick> routing_wait_since)
{
    if (faulted_ || uncertain_) { return Emission::Blocked; }
    const auto up = up_bit(output.kind);
    if (up) {
        const auto result = release(output.kind, up);
        if (result == Emission::Blocked) { faulted_ = true; }
        return result;
    }
    const auto current_time = [&] {
        const auto now = boundary_.now();
        return now <= expected.expires && !routing_wait_expired(routing_wait_since, now);
    };
    const auto current = boundary_.observe(owned_, &output);
    const auto query_pending = [&](const Observation& value) {
        return pending_route(value) && same_context(value, expected) && current_time();
    };
    if (query_pending(current)) { return Emission::Deferred; }
    if (!current.available || !current.allowed || !current.baseline_known || current.mode == Mode::Blocked
        || current.physical_held || !same_context(current, expected) || !current_time()) {
        return Emission::Blocked;
    }
    const Point point = output.kind == OutputKind::Move ? output.point : current.cursor;
    if (!contains(current.bounds, point)) { return Emission::Blocked; }
    const auto final = boundary_.observe(owned_, &output);
    if (query_pending(final)) { return Emission::Deferred; }
    if (!final.available || !final.allowed || !final.baseline_known || final.mode == Mode::Blocked || final.physical_held
        || !same_context(current, final) || !current_time()) { return Emission::Blocked; }
    if (!current.routing_stable || !final.routing_stable || current.routing != final.routing
        || current.mode != final.mode
        || (output.kind != OutputKind::Move && final.cursor != current.cursor)) { return Emission::Deferred; }
    const auto down = down_bit(output.kind);
    if (down && (owned_ & down)) { return Emission::Blocked; }
    if (!current_time()) { return Emission::Blocked; }
    const auto accepted = boundary_.send(output, current.desktop);
    if (accepted == 1) {
        owned_ |= down;
        if (down) { attempts_ = 0; }
        return Emission::Accepted;
    }
    // A failed event is not replayed. Impossible acceptance is not counted as a
    // known down; it latches terminal uncertainty (only process restart clears it).
    if (accepted > 1) { uncertain_ = true; }
    faulted_ = true;
    return Emission::Blocked;
}

void Dispatcher::cleanup(Tick now)
{
    ++attempts_;
    retry_at_ = now + 250;
    const auto primary = release(OutputKind::PrimaryUp, Primary);
    const auto secondary = release(OutputKind::SecondaryUp, Secondary);
    const auto escape = release(OutputKind::EscapeUp, Escape);
    if (primary == Emission::Blocked || secondary == Emission::Blocked || escape == Emission::Blocked
        || uncertain_ || (owned_ && attempts_ >= 3)) { faulted_ = true; }
    if (!owned_) { attempts_ = 0; }
}

void Dispatcher::cancel(Tick now)
{
    // Repeated invalid samples must not replenish the bounded retry budget.
    if (owned_ && attempts_ == 0) { cleanup(now); }
}

void Dispatcher::retry(Tick now)
{
    if (owned_ && attempts_ > 0 && attempts_ < 3 && now >= retry_at_) { cleanup(now); }
}

void Dispatcher::recover(Tick now)
{
    attempts_ = 0;
    if (owned_) { cleanup(now); }
    if (!owned_ && !uncertain_) {
        faulted_ = false;
        attempts_ = 0;
    }
}

void Live::disarm(Tick now, std::wstring_view reason)
{
    automatic_neutral_ = {};
    automatic_observation_.reset();
    armed_ = false;
    controller_active_ = false;
    routing_wait_since_.reset();
    arm_after_ = now;
    buttons_ = 0;
    pointer_.reset(now);
    output_.cancel(now);
    if (output_.faulted()) {
        boundary_.clear_auto_override();
        effective_ = EffectiveMode::Blocked;
    }
    reason_ = output_.faulted() ? L"FAULT - output rejected/uncertain; inspect desktop, then Retry cleanup"
        : output_.owned() ? L"Waiting for a safe button release"
        : reason;
}

void Live::pause(Tick now, std::wstring_view reason)
{
    enabled_ = false;
    disarm(now, reason);
}

void Live::invalidate(Tick now, std::wstring_view reason)
{
    disarm(now, enabled_ ? reason : reason_);
    previous_.reset();
}

void Live::select(std::optional<unsigned> slot, const Snapshot& snapshot, Tick now)
{
    pause(now, L"Controller selection changed; fresh neutral required");
    slot_.reset();
    epoch_.reset();
    if (slot && *slot < slot_count && snapshot.sequence
        && snapshot.sampled_at <= now && now - snapshot.sampled_at <= live_sample_age_ms
        && snapshot.slots[*slot].connection == Connection::Connected) {
        slot_ = slot;
        epoch_ = snapshot.slots[*slot].connection_epoch;
    }
}

void Live::enable(Tick now)
{
    enabled_ = false;
    disarm(now, L"PAUSED - release physical controls, then center controller on an eligible desktop");
    if (!slot_) { reason_ = L"PAUSED - explicitly reconfirm a connected XInput slot"; return; }
    if (output_.faulted() || output_.owned()) { return; }
    if (!boundary_.observe(0).available) { reason_ = L"PAUSED - required live observers/emergency path unavailable"; return; }
    enabled_ = true;
    previous_.reset();
}

void Live::recover(Tick now)
{
    pause(now);
    output_.recover(now);
    disarm(now, L"Cleanup complete; fresh neutral and controller input required");
}

void Live::suspend_automatic(Tick now, std::wstring_view reason)
{
    boundary_.clear_auto_override();
    automatic_paused_ = true;
    effective_ = EffectiveMode::ManualNative;
    pause(now, reason);
}

void Live::resume_automatic(Tick now)
{
    pause(now, L"Automatic resumed; center controller, then move a stick");
    if (!output_.faulted() && !output_.owned()) { automatic_paused_ = false; }
}

void Live::record_effective(const Observation& observation, bool prerequisites)
{
    if (observation.physical_held) { physical_owner_ = true; }
    if (prerequisites && armed_ && !output_.faulted() && pending_route(observation)
        && previous_ && same_context(*previous_, observation)) {
        blocked_ = BlockReason::Routing;
        return; // Ownership is retained during a bounded no-output wait, not new eligibility.
    }
    blocked_ = !prerequisites ? BlockReason::Infrastructure
        : !observation.allowed && observation.block != BlockReason::Auto ? observation.block
        : observation.physical_held ? BlockReason::Physical : observation.block;
    effective_ = !prerequisites || output_.faulted() || (output_.owned() && !armed_)
        || !observation.available || !observation.baseline_known
        || (!observation.allowed && observation.block != BlockReason::Auto)
        ? EffectiveMode::Blocked : automatic_paused_ ? EffectiveMode::ManualNative
        : observation.block == BlockReason::Auto ? EffectiveMode::AutoNative
        : physical_owner_ ? EffectiveMode::Physical : EffectiveMode::Desktop;
}

void Live::physical_takeover(Tick now)
{
    physical_owner_ = true;
    if (effective_ == EffectiveMode::Desktop) { effective_ = EffectiveMode::Physical; }
    invalidate(now, L"Mouse and keyboard in use; center controller before taking over");
}

bool Live::request_desktop(Tick now, bool prerequisites)
{
    const auto observation = boundary_.observe(output_.owned());
    record_effective(observation, prerequisites);
    if (!prerequisites || output_.owned() || output_.faulted()
        || !boundary_.request_desktop(observation, now + live_sample_age_ms)) {
        reason_ = output_.faulted() || output_.owned() ? L"Desktop request blocked by pending output recovery"
            : !prerequisites ? block_reason_text(BlockReason::Infrastructure)
            : observation.block == BlockReason::None ? L"Desktop request needs a stable known app and released physical controls"
            : block_reason_text(observation.block);
        return false;
    }
    resume_automatic(now);
    effective_ = EffectiveMode::Desktop;
    blocked_ = BlockReason::None;
    ++desktop_requests_;
    return true;
}

void Live::observe_text_fault(std::uint64_t generation, Tick now)
{
    if (generation <= text_fault_generation_) { return; }
    text_fault_generation_ = generation;
    boundary_.clear_auto_override();
    pause(now, block_reason_text(BlockReason::TextOutput));
    effective_ = EffectiveMode::Blocked;
    blocked_ = BlockReason::TextOutput;
}

void Live::hold_for_keyboard(bool held, Tick now)
{
    if (keyboard_held_ == held) { return; }
    keyboard_held_ = held;
    if (held) {
        physical_owner_ = false;
        if (effective_ == EffectiveMode::Physical) { effective_ = EffectiveMode::Desktop; }
    }
    if (held) {
        physical_owner_ = false;
        if (effective_ == EffectiveMode::Physical) { effective_ = EffectiveMode::Desktop; }
    }
    pause(now, held ? L"Keyboard owns controller routing; desktop pointer suspended"
                   : L"Keyboard closed; fresh neutral and controller input required");
}

std::wstring_view Live::prefer_slot(std::optional<unsigned> slot, Tick now)
{
    if (slot && *slot >= slot_count) { return L"Choose Automatic or a controller slot from 0 to 3"; }
    pause(now, L"Controller preference changed; fresh neutral and input required");
    preferred_ = slot;
    slot_.reset();
    epoch_.reset();
    return {};
}

bool Live::activate_from_neutral(unsigned slot, const Snapshot& neutral,
                                 const Observation& observation, Tick now)
{
    if (output_.owned() || output_.faulted() || !neutral.sequence || neutral.sampled_at > now
        || now - neutral.sampled_at > live_sample_age_ms || boundary_.now() > neutral.sampled_at + live_sample_age_ms
        || !pointer_neutral(neutral.slots[slot], settings_)) { return false; }
    // Automatic discovery observed neutrality before this controller's fresh intent.
    // Unlike explicit enable, it can arm from that recent real sample without another user gesture.
    pause(now);
    slot_ = slot;
    epoch_ = neutral.slots[slot].connection_epoch;
    sequence_ = neutral.sequence;
    sampled_at_ = neutral.sampled_at;
    previous_ = observation;
    automatic_observation_ = observation;
    enabled_ = armed_ = true;
    reason_ = L"Automatic - controller acquired";
    return true;
}

bool Live::handle_mode_chord(const Snapshot& snapshot, Tick now, bool prerequisites)
{
    // This observer stays live in native-gamepad mode, independently of desktop eligibility.
    if (!snapshot.sequence || snapshot.sampled_at > now || now - snapshot.sampled_at > live_sample_age_ms
        || (chord_snapshot_ && (snapshot.sequence < chord_snapshot_->sequence
            || snapshot.sampled_at < chord_snapshot_->sampled_at
            || (snapshot.sequence == chord_snapshot_->sequence && snapshot.sampled_at != chord_snapshot_->sampled_at)))) {
        chord_snapshot_.reset();
        chord_released_ = false;
        return false;
    }
    bool any = false, complete = false;
    for (unsigned index = 0; index < slot_count; ++index) {
        const auto& pad = snapshot.slots[index];
        if (chord_snapshot_ && (pad.connection != chord_snapshot_->slots[index].connection
            || pad.connection_epoch != chord_snapshot_->slots[index].connection_epoch)) { chord_released_ = false; }
        if (pad.connection != Connection::Connected) { continue; }
        any = any || (pad.buttons & 0x0030) != 0;
        complete = complete || (pad.buttons & 0x0030) == 0x0030;
    }
    if (chord_snapshot_ && snapshot.sequence == chord_snapshot_->sequence) { return complete; }
    if (!chord_snapshot_ || snapshot.sampled_at - chord_snapshot_->sampled_at > live_sample_age_ms) {
        chord_released_ = false;
    }
    const bool toggle = complete && chord_released_;
    chord_snapshot_ = snapshot;
    if (!any) { chord_released_ = true; }
    if (complete) {
        chord_released_ = false; // Both buttons on all connected pads must be released before another toggle.
        if (toggle) {
            const auto observation = boundary_.observe(output_.owned());
            record_effective(observation, prerequisites);
            if (effective_ != EffectiveMode::Desktop && effective_ != EffectiveMode::Physical) { request_desktop(now, prerequisites); }
            else { suspend_automatic(now, L"Native gamepad input - Menu+View switches back to desktop control"); }
        } else if (!automatic_paused_) {
            pause(now, L"Release Menu+View, then center the controller");
        }
    }
    return complete;
}

void Live::automatic_step(const Snapshot& snapshot, Tick now, bool prerequisites)
{
    if (handle_mode_chord(snapshot, now, prerequisites)) {
        output_.retry(now);
        return; // The switching chord and any simultaneous A/X/B never become desktop actions.
    }
    if (keyboard_held_) {
        output_.retry(now);
        return;
    }
    if (automatic_paused_ || output_.faulted()) {
        step(snapshot, now); // Bounded owned-output cleanup still runs while suspended.
        return;
    }
    if (output_.owned() && !armed_) { step(snapshot, now); return; }
    if (!prerequisites) {
        effective_ = EffectiveMode::Blocked;
        blocked_ = BlockReason::Infrastructure;
        pause(now, L"Automatic waiting - configuration, observers, or inspection mode blocks input");
        step(snapshot, now);
        return;
    }
    if (!snapshot.sequence || snapshot.sequence < automatic_sequence_ || snapshot.sampled_at > now
        || snapshot.sampled_at < automatic_sampled_at_
        || (snapshot.sequence == automatic_sequence_ && snapshot.sampled_at != automatic_sampled_at_)
        || now - snapshot.sampled_at > live_sample_age_ms) {
        pause(now, L"Automatic waiting - controller sample unavailable or stale");
        step(snapshot, now);
        return;
    }
    if (snapshot.sequence == automatic_sequence_) { step(snapshot, now); return; }
    automatic_sequence_ = snapshot.sequence;
    automatic_sampled_at_ = snapshot.sampled_at;
    bool connected = false;
    for (unsigned index = 0; index < slot_count; ++index) {
        if (preferred_ && *preferred_ != index) { continue; }
        const auto& pad = snapshot.slots[index];
        if (pad.connection != Connection::Connected) { continue; }
        connected = true;
    }
    if (!connected) {
        pause(now, L"Automatic waiting - no connected controller");
        slot_.reset();
        epoch_.reset();
        step(snapshot, now);
        return;
    }
    if (enabled_ && slot_ && snapshot.slots[*slot_].connection == Connection::Connected
        && snapshot.slots[*slot_].connection_epoch == *epoch_
        && (output_.owned() || !pointer_neutral(snapshot.slots[*slot_], settings_))) {
        // Existing live routing already revalidates the desktop and each output.
        // Avoid another expensive native query on every held-motion sample.
        step(snapshot, now);
        if (armed_ && !output_.faulted()) {
            for (unsigned index = 0; index < slot_count; ++index) {
                const auto& pad = snapshot.slots[index];
                if ((!preferred_ || *preferred_ == index) && pad.connection == Connection::Connected
                    && pointer_neutral(pad, settings_) && snapshot.sampled_at > arm_after_) {
                    automatic_neutral_[index] = snapshot;
                } else {
                    automatic_neutral_[index].reset();
                }
            }
        }
        return;
    }
    auto observation = boundary_.observe(output_.owned());
    record_effective(observation, prerequisites);
    if (armed_ && pending_route(observation) && previous_ && same_context(*previous_, observation)
        && boundary_.now() <= snapshot.sampled_at + live_sample_age_ms) {
        step(snapshot, now);
        return;
    }
    if (!observation.available || !observation.allowed || !observation.baseline_known
        || observation.physical_held || observation.mode == Mode::Blocked
        || boundary_.now() > snapshot.sampled_at + live_sample_age_ms) {
        pause(now, observation.physical_held ? L"Physical input has control; center pad before using it" : observation.reason);
        step(snapshot, now);
        return;
    }
    if (automatic_observation_ && !same_context(*automatic_observation_, observation)) {
        pause(now, L"Automatic waiting - fresh neutral after context or physical input change");
    }
    automatic_observation_ = observation;
    if (!observation.routing_stable) { step(snapshot, now); return; }

    const auto intent = [&](const Controller& pad) {
        return (pad.buttons & (a_button | 0x6000)) != 0
            || std::hypot(pad.left_x, pad.left_y) / 32767.0 > settings_.left_enter
            || (pad.right_y != 0 && std::hypot(pad.right_x, pad.right_y) / 32767.0 > settings_.right_enter);
    };
    std::optional<unsigned> candidate;
    std::optional<Snapshot> neutral_sample;
    unsigned candidates = 0;
    for (unsigned index = 0; index < slot_count; ++index) {
        auto& baseline = automatic_neutral_[index];
        const auto& pad = snapshot.slots[index];
        if ((preferred_ && *preferred_ != index) || pad.connection != Connection::Connected
            || (baseline && (baseline->slots[index].connection_epoch != pad.connection_epoch
                || now - baseline->sampled_at > live_sample_age_ms))) { baseline.reset(); }
        if ((preferred_ && *preferred_ != index) || pad.connection != Connection::Connected) { continue; }
        if (pointer_neutral(pad, settings_)) {
            if (snapshot.sampled_at > arm_after_) { baseline = snapshot; }
        } else if (intent(pad)) {
            if (baseline && baseline->sequence < snapshot.sequence) {
                candidate = index;
                neutral_sample = baseline;
                ++candidates;
            }
            baseline.reset(); // Never replay another controller's held intent later.
        } else if ((pad.buttons & standard_buttons) || pad.left_trigger > 30 || pad.right_trigger > 30) {
            baseline.reset();
        }
    }
    const bool current_busy = enabled_ && slot_ && snapshot.slots[*slot_].connection == Connection::Connected
        && (output_.owned() || !pointer_neutral(snapshot.slots[*slot_], settings_));
    if (!current_busy && candidates > 1) {
        pause(now, L"Multiple controllers moved together; use one pad or choose a controller preference");
        step(snapshot, now);
        return;
    }
    if (!current_busy && candidate && candidates == 1 && (!enabled_ || slot_ != candidate)) {
        activate_from_neutral(*candidate, *neutral_sample, observation, now);
    }
    if (!enabled_) { reason_ = L"Automatic ready - center a controller, then move a stick or press A"; }
    step(snapshot, now);
}

std::wstring_view Live::set_pointer_settings(const PointerSettings& settings, Tick now)
{
    if (const auto error = validate_pointer_settings(settings); !error.empty()) { return error; }
    pause(now, L"Pointer settings applied; fresh neutral and controller input required");
    settings_ = settings;
    previous_.reset();
    return {};
}

std::wstring_view Live::set_exclusions(const ExecutableExclusions& names, Tick now)
{
    if (const auto error = validate_exclusions(names); !error.empty()) { return error; }
    auto copy = names;
    pause(now, L"Exclusions applied; fresh neutral and controller input required");
    boundary_.exclusions_ = std::move(copy);
    boundary_.clear_auto_override();
    previous_.reset();
    return {};
}

bool Live::emit(Output output, const Observation& observation, Tick now)
{
    const auto result = output_.emit(output, observation, routing_wait_since_);
    const auto finished_at = boundary_.now();
    if (routing_wait_expired(routing_wait_since_, finished_at)) {
        effective_ = EffectiveMode::Blocked;
        disarm(finished_at, L"PAUSED - routing did not stabilize within 100 ms; fresh neutral required");
        return false;
    }
    if (result == Emission::Accepted) {
        if (!up_bit(output.kind)) {
            routing_wait_since_.reset();
            controller_active_ = true;
            physical_owner_ = false;
            effective_ = EffectiveMode::Desktop;
        }
        return true;
    }
    if (result == Emission::Deferred) {
        if (blocked_ == BlockReason::None) { blocked_ = BlockReason::Routing; }
        defer(boundary_.now());
        return false;
    }
    disarm(now, L"PAUSED - unsafe recipient, physical takeover, or expired input; fresh neutral required");
    return false;
}

void Live::defer(Tick now)
{
    if (!routing_wait_since_) { routing_wait_since_ = now; }
    if (routing_wait_expired(routing_wait_since_, now)) {
        effective_ = EffectiveMode::Blocked;
        disarm(now, L"PAUSED - routing did not stabilize within 100 ms; fresh neutral required");
        return;
    }
    reason_ = blocked_ == BlockReason::Settling ? block_reason_text(BlockReason::Settling)
        : L"LIVE - routing changing; event skipped, held edges not replayed";
}

void Live::step(const Snapshot& snapshot, Tick now)
{
    output_.retry(now);
    if (keyboard_held_) { return; }
    if (output_.faulted()) {
        disarm(now, L"FAULT - Retry cleanup required");
        return;
    }
    if (output_.owned() && !armed_) {
        reason_ = L"Waiting for a safe button release";
        return;
    }
    if (!enabled_) { return; }
    if (!snapshot.sequence || snapshot.sequence < sequence_ || snapshot.sampled_at > now
        || snapshot.sampled_at < sampled_at_
        || (snapshot.sequence == sequence_ && snapshot.sampled_at != sampled_at_)
        || now - snapshot.sampled_at > live_sample_age_ms) {
        disarm(now, L"PAUSED - stale, missing, or invalid controller sample (100 ms)");
        return;
    }
    const bool fresh = snapshot.sequence > sequence_;
    const bool gap = sequence_ && snapshot.sampled_at - sampled_at_ > live_sample_age_ms;
    sequence_ = snapshot.sequence;
    sampled_at_ = snapshot.sampled_at;
    const auto& controller = snapshot.slots[*slot_];
    if (controller.connection != Connection::Connected || controller.connection_epoch != *epoch_) {
        slot_.reset();
        epoch_.reset();
        pause(now, L"Connection changed; automatic control waits for fresh neutral and input");
        return;
    }
    if ((controller.buttons & 0x0030) == 0x0030) {
        pause(now, L"Release Menu+View, then center the controller");
        return;
    }
    if (gap && !armed_) { disarm(now, L"PAUSED - controller scheduling gap; fresh neutral required"); return; }
    auto observation = boundary_.observe(output_.owned());
    record_effective(observation);
    observation.expires = snapshot.sampled_at + live_sample_age_ms;
    if (boundary_.now() > observation.expires) {
        disarm(now, L"PAUSED - controller sample expired during observation; fresh neutral required");
        return;
    }
    if (previous_ && !same_context(*previous_, observation)) {
        previous_ = observation;
        disarm(now, L"PAUSED - unsafe context, geometry, or physical takeover; fresh neutral required");
        return;
    }
    const auto observed_at = boundary_.now();
    if (routing_wait_expired(routing_wait_since_, observed_at)) {
        effective_ = EffectiveMode::Blocked;
        disarm(now, L"PAUSED - routing did not stabilize within 100 ms; fresh neutral required");
        return;
    }
    if (armed_ && pending_route(observation)) {
        buttons_ = controller.buttons;
        pointer_.skip_interval(controller, now, settings_);
        defer(observed_at);
        return; // Retain accepted downs, but never emit from an unknown route.
    }
    previous_ = observation;
    if (!observation.available || !observation.allowed || !observation.baseline_known
        || observation.physical_held || observation.mode == Mode::Blocked) {
        disarm(now, observation.physical_held ? L"PAUSED - physical key or mouse button held" : observation.reason);
        return;
    }
    if (gap) {
        // A fresh reader sample after a UI move/size stall is not stale hardware.
        // Preserve accepted holds, but never replay elapsed motion or missed presses.
        buttons_ = controller.buttons;
        pointer_.reset(now);
    }
    if (!fresh) { return; }
    if (!observation.routing_stable) {
        if (armed_) { buttons_ = controller.buttons; }
        pointer_.skip_interval(controller, now, settings_);
        defer(boundary_.now());
        return;
    }
    if (!armed_) {
        routing_wait_since_.reset();
        if (snapshot.sampled_at <= arm_after_ || !pointer_neutral(controller, settings_)) {
            reason_ = L"PAUSED - release ALL controller buttons/triggers and center both sticks";
            return;
        }
        armed_ = true;
        pointer_.reset(now);
        buttons_ = 0;
        reason_ = L"DESKTOP READY - Menu+View switches to native gamepad input";
        return;
    }
    const auto pressed = static_cast<std::uint16_t>(controller.buttons & ~buttons_);
    buttons_ = controller.buttons; // Includes deferred/ignored edges across routing changes.
    const bool primary = (controller.buttons & a_button) != 0;
    // A tap's accepted down survives a benign routing deferral of its up.
    if ((output_.owned() & Secondary) && !emit({OutputKind::SecondaryUp}, observation, now)) { return; }
    if ((output_.owned() & Escape) && !emit({OutputKind::EscapeUp}, observation, now)) { return; }
    if (!primary && (output_.owned() & Primary)) {
        if (!emit({OutputKind::PrimaryUp}, observation, now)) { return; }
    }
    const auto motion = pointer_.step(controller, observation.cursor, observation.bounds, now,
                                      !primary, settings_);
    if (motion.point != observation.cursor && !emit({OutputKind::Move, motion.point}, observation, now)) { return; }
    if ((pressed & a_button) && !(output_.owned() & Primary)) {
        if (!emit({OutputKind::PrimaryDown}, observation, now)) { return; }
    }
    if (!primary && !(output_.owned() & Primary)) {
        if (pressed & 0x4000) {
            if (!emit({OutputKind::SecondaryDown}, observation, now)) { return; }
            if (!emit({OutputKind::SecondaryUp}, observation, now)) { return; }
            reason_ = L"LIVE - secondary click; desktop/menu routing remains active";
            return;
        }
        if (pressed & 0x2000) {
            if (!emit({OutputKind::EscapeDown}, observation, now)) { return; }
            if (!emit({OutputKind::EscapeUp}, observation, now)) { return; }
            reason_ = L"LIVE - Escape; desktop/menu routing remains active";
            return;
        }
        if (motion.wheel && !emit({OutputKind::Wheel, {}, motion.wheel}, observation, now)) { return; }
    }
    reason_ = output_.owned() ? L"LIVE - primary held / dragging; physical input or Pause cancels"
        : L"LIVE - desktop pointer / scroll; Menu+View switches to native gamepad input";
    routing_wait_since_.reset();
}

} // namespace gamepad::live
