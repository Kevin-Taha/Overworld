#include "Core.h"

namespace gamepad {
namespace {
bool centered(std::int16_t x, std::int16_t y, std::int64_t radius)
{
    return std::int64_t{x} * x + std::int64_t{y} * y <= radius * radius;
}
} // namespace

bool neutral(const Controller& controller)
{
    return (controller.buttons & standard_buttons) == 0
        && controller.left_trigger <= 30 && controller.right_trigger <= 30
        && centered(controller.left_x, controller.left_y, 7849)
        && centered(controller.right_x, controller.right_y, 8689);
}

std::optional<Action> Core::disarm(Tick now, Reason reason)
{
    armed_ = false;
    arm_after_ = now;
    if (!owns_a_) {
        return {};
    }
    owns_a_ = false;
    return Action{ActionKind::AUp, now, reason, true};
}

Result Core::finish(State state, Reason reason, std::optional<Action> action)
{
    status_ = {state, reason};
    return {status_, action};
}

State Core::inactive_state() const
{
    return paused_ || exiting_ ? State::Paused : State::Disabled;
}

Reason Core::inactive_reason() const
{
    return exiting_ ? Reason::Exiting : paused_ ? Reason::UserPause : Reason::Startup;
}

Result Core::select_slot(std::optional<unsigned> slot, Tick now)
{
    const auto action = disarm(now, Reason::SelectionChanged);
    connection_epoch_.reset();
    confirmation_required_ = false;
    selected_ = slot && *slot < slot_count ? slot : std::nullopt;
    if (slot && !selected_) {
        return finish(State::Paused, Reason::InvalidSelection, action);
    }
    return finish(enabled_ ? State::Paused : inactive_state(),
                  enabled_ ? (selected_ ? Reason::AwaitingSample : Reason::NoSelection)
                           : inactive_reason(), action);
}

Result Core::enable(Tick now)
{
    if (exiting_) {
        return finish(State::Paused, Reason::Exiting);
    }
    enabled_ = true;
    paused_ = false;
    return finish(State::Paused, selected_ ? Reason::AwaitingSample : Reason::NoSelection,
                  disarm(now, Reason::Enabled));
}

Result Core::pause(Tick now)
{
    enabled_ = false;
    paused_ = true;
    return finish(State::Paused, inactive_reason(), disarm(now, Reason::UserPause));
}

Result Core::shutdown(Tick now)
{
    enabled_ = false;
    exiting_ = true;
    return finish(State::Paused, Reason::Exiting, disarm(now, Reason::Exiting));
}

Result Core::step(const Snapshot& snapshot, const Gates& gates, Tick now)
{
    const bool gates_changed = previous_gates_ && *previous_gates_ != gates;
    previous_gates_ = gates;
    bool boundary = gates_changed;
    std::optional<Action> action;
    if (boundary) {
        action = disarm(now, Reason::AwaitingNeutral);
    }

    const auto blocked = [&](Reason reason) {
        const auto release = disarm(now, reason);
        if (release) {
            action = release;
        }
        // Keep the actual gate-loss cause on a release already caused by a boundary.
        if (action) {
            action->reason = reason;
        }
        return finish(enabled_ ? State::Paused : inactive_state(),
                      enabled_ ? reason : inactive_reason(), action);
    };

    if (snapshot.sequence == 0) {
        return blocked(Reason::AwaitingSample);
    }
    if (snapshot.sequence < last_sequence_ || snapshot.sampled_at > now
        || (last_sequence_ != 0 && snapshot.sampled_at < last_sample_time_)
        || (snapshot.sequence == last_sequence_ && snapshot.sampled_at != last_sample_time_)) {
        return blocked(Reason::InvalidSample);
    }

    const bool fresh = snapshot.sequence > last_sequence_;
    if (fresh) {
        if (last_sequence_ != 0 && snapshot.sampled_at - last_sample_time_ > maximum_sample_age_ms) {
            boundary = true;
            const auto release = disarm(now, Reason::StaleSample);
            if (release) {
                action = release;
            }
        }
        last_sequence_ = snapshot.sequence;
        last_sample_time_ = snapshot.sampled_at;
    }
    if (now - snapshot.sampled_at > maximum_sample_age_ms) {
        return blocked(Reason::StaleSample);
    }
    if (!selected_) {
        return blocked(Reason::NoSelection);
    }

    const auto& controller = snapshot.slots[*selected_];
    if (connection_epoch_ && *connection_epoch_ != controller.connection_epoch) {
        confirmation_required_ = true;
    }
    connection_epoch_ = controller.connection_epoch;
    if (controller.connection == Connection::Disconnected || controller.connection == Connection::Error) {
        confirmation_required_ = true;
        return blocked(controller.connection == Connection::Disconnected
                           ? Reason::Disconnected : Reason::ReaderError);
    }
    if (controller.connection != Connection::Connected) {
        return blocked(Reason::AwaitingSample);
    }
    if (confirmation_required_) {
        return blocked(Reason::ConfirmSelection);
    }
    if (!enabled_) {
        return blocked(inactive_reason());
    }
    if (!gates.session_available) {
        return blocked(Reason::SessionUnavailable);
    }
    if (!gates.context_allowed) {
        return blocked(Reason::ContextBlocked);
    }
    if (!gates.physical_baseline_known) {
        return blocked(Reason::PhysicalUnknown);
    }
    if (gates.physical_modifiers != 0 || gates.physical_mouse_buttons != 0) {
        return blocked(Reason::PhysicalHeld);
    }
    if (boundary) {
        return finish(State::Paused, Reason::AwaitingNeutral, action);
    }
    if (!armed_) {
        if (!fresh || snapshot.sampled_at <= arm_after_ || !neutral(controller)) {
            return finish(State::Paused, Reason::AwaitingNeutral, action);
        }
        armed_ = true;
    }
    if (fresh) {
        const bool down = (controller.buttons & a_button) != 0;
        if (down != owns_a_) {
            owns_a_ = down;
            action = Action{down ? ActionKind::ADown : ActionKind::AUp, now,
                            down ? Reason::SimulatedAHeld : Reason::Ready, false};
        }
    }
    return finish(State::Ready, owns_a_ ? Reason::SimulatedAHeld : Reason::Ready, action);
}

std::wstring_view state_text(State state)
{
    switch (state) {
    case State::Disabled: return L"Disabled";
    case State::Paused: return L"Paused";
    case State::Ready: return L"Ready";
    }
    return L"Unknown state";
}

std::wstring_view reason_text(Reason reason)
{
    switch (reason) {
    case Reason::Startup: return L"Explicit Enable dry run required";
    case Reason::UserPause: return L"Explicit pause; Enable dry run to resume";
    case Reason::Exiting: return L"Exiting; simulated ownership released";
    case Reason::NoSelection: return L"No controller selected";
    case Reason::InvalidSelection: return L"Invalid slot; select one of slots 0-3";
    case Reason::AwaitingSample: return L"Waiting for a new controller snapshot";
    case Reason::StaleSample: return L"Reader stale or stalled (over 250 ms)";
    case Reason::InvalidSample: return L"Invalid or out-of-order snapshot";
    case Reason::Disconnected: return L"Selected slot disconnected; reconfirm selection after reconnect";
    case Reason::ReaderError: return L"Selected-slot reader error; inspect slot and reconfirm";
    case Reason::ConfirmSelection: return L"Connection changed; explicitly select/reconfirm this slot";
    case Reason::ContextBlocked: return L"Simulated context is blocked";
    case Reason::SessionUnavailable: return L"Simulated session is unavailable";
    case Reason::PhysicalUnknown: return L"Simulated physical baseline is unknown";
    case Reason::PhysicalHeld: return L"Simulated physical modifier or mouse button is held";
    case Reason::AwaitingNeutral: return L"Release controller buttons/triggers and center both sticks";
    case Reason::Ready: return L"Dry run armed; a fresh A press records a simulated down/up";
    case Reason::SimulatedAHeld: return L"Simulated A is held (no desktop input)";
    case Reason::SelectionChanged: return L"Selection changed/reconfirmed";
    case Reason::Enabled: return L"Dry run explicitly enabled";
    }
    return L"Unknown reason";
}
} // namespace gamepad
