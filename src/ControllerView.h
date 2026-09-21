#pragma once

#include "Core.h"

namespace overworld {
inline constexpr Tick controller_view_sample_age_ms = 100;
enum class ControllerViewSignal { Waiting, Ready, Stale, Disconnected, ReadError, Ambiguous };
enum class ControllerViewRoute { Observing, Desktop, Keyboard, Paused };

struct ControllerViewState {
    ControllerViewSignal signal = ControllerViewSignal::Waiting;
    ControllerViewRoute route = ControllerViewRoute::Observing;
    std::optional<unsigned> slot;
    std::uint16_t buttons = 0;
    std::uint8_t left_trigger = 0, right_trigger = 0;
    std::int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    bool operator==(const ControllerViewState&) const = default;
};

struct ControllerViewFrame {
    ControllerViewState state;
    Tick sampled_at = 0;
    std::uint64_t sequence = 0;
};

[[nodiscard]] constexpr bool controller_view_fresh(Tick sampled_at, Tick now)
{
    return sampled_at <= now && now - sampled_at <= controller_view_sample_age_ms;
}

[[nodiscard]] constexpr float controller_view_axis(std::int16_t value)
{
    return static_cast<float>(value) / (value < 0 ? 32768.0f : 32767.0f);
}

[[nodiscard]] inline ControllerViewState controller_view_clear(
    ControllerViewState state, ControllerViewSignal signal)
{
    state.signal = signal;
    state.route = ControllerViewRoute::Observing;
    state.buttons = 0;
    state.left_trigger = state.right_trigger = 0;
    state.left_x = state.left_y = state.right_x = state.right_y = 0;
    return state;
}

// This observes an already-consumed batch; it never selects a controller for the mapper.
[[nodiscard]] inline ControllerViewFrame controller_view_frame(const Snapshot& snapshot, Tick now,
    std::optional<unsigned> selected, ControllerViewRoute route)
{
    ControllerViewFrame frame{{}, snapshot.sampled_at, snapshot.sequence};
    auto& state = frame.state;
    state.slot = selected;
    if (!snapshot.sequence) { return frame; }
    if (!controller_view_fresh(snapshot.sampled_at, now)) {
        state.signal = ControllerViewSignal::Stale;
        return frame;
    }
    if (selected && *selected >= slot_count) {
        state.slot.reset();
        state.signal = ControllerViewSignal::ReadError;
        return frame;
    }
    if (!selected) {
        unsigned connected = 0;
        bool failed = false, pending = false;
        for (unsigned index = 0; index < slot_count; ++index) {
            const auto connection = snapshot.slots[index].connection;
            if (connection == Connection::Connected) { state.slot = index; ++connected; }
            failed = failed || connection == Connection::Error;
            pending = pending || connection == Connection::Unknown;
        }
        if (connected != 1) {
            state.slot.reset();
            state.signal = connected > 1 ? ControllerViewSignal::Ambiguous
                : failed ? ControllerViewSignal::ReadError
                : pending ? ControllerViewSignal::Waiting : ControllerViewSignal::Disconnected;
            return frame;
        }
        route = ControllerViewRoute::Observing;
    }
    const auto& pad = snapshot.slots[*state.slot];
    if (pad.connection != Connection::Connected) {
        state.signal = pad.connection == Connection::Error ? ControllerViewSignal::ReadError
            : pad.connection == Connection::Disconnected ? ControllerViewSignal::Disconnected
            : ControllerViewSignal::Waiting;
        return frame;
    }
    state.signal = ControllerViewSignal::Ready;
    state.route = route;
    state.buttons = pad.buttons & standard_buttons;
    state.left_trigger = pad.left_trigger;
    state.right_trigger = pad.right_trigger;
    state.left_x = pad.left_x;
    state.left_y = pad.left_y;
    state.right_x = pad.right_x;
    state.right_y = pad.right_y;
    return frame;
}

[[nodiscard]] inline ControllerViewState controller_view_present(const ControllerViewFrame& frame, Tick now)
{
    return frame.sequence && !controller_view_fresh(frame.sampled_at, now)
        ? controller_view_clear(frame.state, ControllerViewSignal::Stale) : frame.state;
}
} // namespace overworld
