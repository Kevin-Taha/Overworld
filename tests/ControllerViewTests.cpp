#include "ControllerView.h"
#include "MonotonicClock.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace overworld;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}

Snapshot sample()
{
    Snapshot value;
    value.sequence = 40;
    value.sampled_at = 1000;
    for (auto& pad : value.slots) { pad.connection = Connection::Disconnected; }
    value.slots[2].connection = Connection::Connected;
    value.slots[2].connection_epoch = 6;
    value.slots[2].buttons = 0xFFFF;
    value.slots[2].left_x = -32768;
    value.slots[2].left_y = 32767;
    value.slots[2].right_x = 12345;
    value.slots[2].right_y = -4567;
    value.slots[2].left_trigger = 128;
    value.slots[2].right_trigger = 255;
    return value;
}

bool clear(const ControllerViewState& state)
{
    return !state.buttons && !state.left_x && !state.left_y && !state.right_x && !state.right_y
        && !state.left_trigger && !state.right_trigger && state.route == ControllerViewRoute::Observing;
}

void frames()
{
    auto snapshot = sample();
    const auto frame = controller_view_frame(snapshot, 1000, 2, ControllerViewRoute::Keyboard);
    check(frame.sequence == snapshot.sequence && frame.sampled_at == snapshot.sampled_at,
        "retain original consumed sequence and timestamp");
    const auto& state = frame.state;
    check(state.signal == ControllerViewSignal::Ready && state.slot == 2u
        && state.route == ControllerViewRoute::Keyboard, "follow keyboard's original selected slot");
    check(state.buttons == standard_buttons && state.left_x == -32768 && state.left_y == 32767
        && state.right_x == 12345 && state.right_y == -4567 && state.left_trigger == 128
        && state.right_trigger == 255, "exact mapped values; exclude unobservable Guide/reserved bits");
    for (const auto route : {ControllerViewRoute::Desktop, ControllerViewRoute::Keyboard,
        ControllerViewRoute::Paused, ControllerViewRoute::Observing}) {
        const auto observed = controller_view_frame(snapshot, 1000, 2, route).state;
        check(observed.route == route && observed.buttons == standard_buttons,
            "raw observation does not misrepresent paused input as output");
    }
    const auto unselected = controller_view_frame(snapshot, 1000, {}, ControllerViewRoute::Desktop).state;
    check(unselected.signal == ControllerViewSignal::Ready && unselected.slot == 2u
        && unselected.route == ControllerViewRoute::Observing, "single-pad fallback is observation, not acquisition");
    snapshot.slots[0].connection = Connection::Connected;
    snapshot.slots[0].buttons = a_button;
    const auto ambiguous = controller_view_frame(snapshot, 1000, {}, ControllerViewRoute::Desktop).state;
    check(ambiguous.signal == ControllerViewSignal::Ambiguous && !ambiguous.slot && clear(ambiguous),
        "never merge pads or guess when no mapper selection exists");
    const auto selected = controller_view_frame(snapshot, 1000, 2, ControllerViewRoute::Desktop).state;
    check(selected.buttons == standard_buttons && selected.slot == 2u, "other pads cannot replace selected values");
    snapshot.slots[2].connection = Connection::Disconnected;
    const auto missing = controller_view_frame(snapshot, 1000, 2, ControllerViewRoute::Keyboard).state;
    check(missing.signal == ControllerViewSignal::Disconnected && missing.slot == 2u && clear(missing),
        "disconnected original target never falls back to another connected pad");
    snapshot.slots[2].connection = Connection::Error;
    const auto error = controller_view_frame(snapshot, 1000, 2, ControllerViewRoute::Desktop).state;
    check(error.signal == ControllerViewSignal::ReadError && clear(error), "read failure explicitly blanks held controls");
    snapshot.slots[2].connection = Connection::Unknown;
    check(controller_view_frame(snapshot, 1000, 2, ControllerViewRoute::Desktop).state.signal
        == ControllerViewSignal::Waiting, "pending reader is not fabricated disconnected success");
    const auto invalid = controller_view_frame(snapshot, 1000, slot_count, ControllerViewRoute::Desktop).state;
    check(invalid.signal == ControllerViewSignal::ReadError && !invalid.slot && clear(invalid), "reject invalid slot");
    snapshot = sample();
    snapshot.slots[2].connection_epoch++;
    snapshot.slots[2].buttons = 0;
    snapshot.slots[2].left_trigger = snapshot.slots[2].right_trigger = 0;
    const auto replacement = controller_view_frame(snapshot, 1001, 2, ControllerViewRoute::Paused).state;
    check(!replacement.buttons && !replacement.left_trigger && !replacement.right_trigger,
        "replacement shows only current snapshot; no retained holds across epochs");
    snapshot = {};
    check(controller_view_frame(snapshot, 1, {}, ControllerViewRoute::Desktop).state.signal
        == ControllerViewSignal::Waiting, "empty initial mailbox remains waiting");
}

void freshness()
{
    const auto snapshot = sample();
    const auto frame = controller_view_frame(snapshot, 1000, 2, ControllerViewRoute::Desktop);
    check(controller_view_present(frame, 1100).signal == ControllerViewSignal::Ready, "100-ms boundary inclusive");
    for (const Tick now : {Tick{1101}, Tick{999}, (std::numeric_limits<Tick>::max)()}) {
        const auto state = controller_view_present(frame, now);
        check(state.signal == ControllerViewSignal::Stale && clear(state),
            "delayed rendering/future timestamps cannot leave stuck controls");
        const auto consumed = controller_view_frame(snapshot, now, 2, ControllerViewRoute::Desktop).state;
        check(consumed.signal == ControllerViewSignal::Stale && clear(consumed),
            "already stale at Host publication is also blank");
    }
    const auto stale = controller_view_frame(snapshot, 1101, 2, ControllerViewRoute::Desktop);
    check(controller_view_present(stale, 1000).signal == ControllerViewSignal::Stale,
        "clock rollback cannot revive already rejected input");
    auto duplicate = frame;
    for (Tick now = 1001; now <= 1110; ++now) {
        duplicate = controller_view_frame(snapshot, now, 2, ControllerViewRoute::Desktop);
    }
    check(duplicate.sampled_at == 1000 && duplicate.state.signal == ControllerViewSignal::Stale,
        "repeated publication never renews sample timestamp");
    const auto now = monotonic_now();
    check(now <= monotonic_now(), "reader and view share the same steady clock implementation");
}

void axes()
{
    check(controller_view_axis(-32768) == -1 && controller_view_axis(0) == 0
        && controller_view_axis(32767) == 1, "both stick endpoints map exactly");
    float previous = -2;
    for (int value = -32768; value <= 32767; ++value) {
        const auto normalized = controller_view_axis(static_cast<std::int16_t>(value));
        check(std::isfinite(normalized) && normalized >= -1 && normalized <= 1
            && normalized > previous, "all stick values preserve direction, magnitude and range without deadzones");
        previous = normalized;
    }
}

}

int main()
{
    try { frames(); freshness(); axes(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS controller view snapshot identity, selection, raw controls, freshness and axes.\n";
}
