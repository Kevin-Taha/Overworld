#include "Core.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace gamepad;

namespace {
void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Action require_action(const Result& result)
{
    check(result.action.has_value(), "expected a simulated action");
    return *result.action;
}

struct Fixture {
    Core core;
    Snapshot snapshot;
    Gates gates{true, 1, true, true, 0, 0, 0};
    Tick now = 1000;
    int downs = 0;
    int ups = 0;

    Fixture()
    {
        for (auto& slot : snapshot.slots) {
            slot.connection = Connection::Connected;
            slot.connection_epoch = 1;
        }
    }

    Result record(Result result)
    {
        if (result.action) {
            result.action->kind == ActionKind::ADown ? ++downs : ++ups;
        }
        check(downs - ups == (core.owns_a() ? 1 : 0), "ledger and emitted simulation disagree");
        check(ups <= downs, "released output not owned by the core");
        return result;
    }

    Result tick()
    {
        now += 16;
        ++snapshot.sequence;
        snapshot.sampled_at = now;
        return record(core.step(snapshot, gates, now));
    }

    void select(unsigned slot = 0) { record(core.select_slot(slot, now)); }
    void enable() { record(core.enable(now)); }
    void arm()
    {
        select();
        enable();
        check(tick().status.state == State::Ready, "neutral snapshot should arm");
    }
    void hold()
    {
        arm();
        snapshot.slots[0].buttons = a_button;
        check(require_action(tick()).kind == ActionKind::ADown, "fresh A should emit down");
    }
    void expect_no_down(Result result)
    {
        check(!result.action || result.action->kind != ActionKind::ADown, "unexpected fresh down");
    }
};

void startup_and_selection()
{
    Fixture f;
    check(f.core.status().state == State::Disabled, "must start disabled");
    f.snapshot.slots[0].buttons = a_button;
    f.expect_no_down(f.tick());
    check(f.core.status().state == State::Disabled, "sampling auto-enabled core");
    f.enable();
    check(f.tick().status.reason == Reason::NoSelection, "enable must not auto-select");
    check(!f.core.selected_slot(), "slot selection must remain empty");
    f.record(f.core.select_slot(4, f.now));
    check(f.core.status().reason == Reason::InvalidSelection, "invalid slot must be visible");
    check(!f.core.selected_slot(), "invalid slot must not be stored");
}

void held_before_enable()
{
    Fixture f;
    f.select();
    f.snapshot.slots[0].buttons = a_button;
    f.tick();
    f.enable();
    for (int i = 0; i < 3; ++i) {
        f.expect_no_down(f.tick());
    }
    check(f.core.status().reason == Reason::AwaitingNeutral, "held initial A must wait");
    f.snapshot.slots[0].buttons = 0;
    check(f.tick().status.state == State::Ready, "release should arm without an up");
    check(f.ups == 0, "initial physical A must not be owned");
    f.snapshot.slots[0].buttons = a_button;
    f.tick();
    f.tick(); // Unchanged packet and held button do not repeat.
    check(f.downs == 1, "fresh edge should emit exactly once, regardless of packet");
    f.snapshot.slots[0].buttons = 0;
    f.tick();
    check(f.ups == 1 && !f.core.owns_a(), "ordinary release must clear owned output");
}

void neutral_means_all_controls()
{
    Fixture f;
    f.select();
    f.enable();
    auto& c = f.snapshot.slots[0];
    c.buttons = 0x2000;
    check(f.tick().status.reason == Reason::AwaitingNeutral, "non-A button blocks arming");
    c.buttons = 0;
    c.left_trigger = 31;
    check(f.tick().status.reason == Reason::AwaitingNeutral, "trigger blocks arming");
    c.left_trigger = 0;
    c.right_trigger = 31;
    check(!neutral(c), "right trigger blocks neutrality");
    c.right_trigger = 0;
    c.left_x = -32768;
    c.left_y = -32768;
    check(f.tick().status.reason == Reason::AwaitingNeutral, "extreme axes block arming");
    c.left_x = 6000;
    c.left_y = 6000;
    check(!neutral(c), "left deadzone must be radial, not rectangular");
    c.left_x = c.left_y = 0;
    c.right_y = 8690;
    check(f.tick().status.reason == Reason::AwaitingNeutral, "right stick blocks arming");
    c.right_y = 0;
    c.buttons = 0x0400;
    check(neutral(c), "reserved/Guide bit must not participate in mapping");
    check(f.tick().status.state == State::Ready, "neutral controller should arm");
}

void pause_and_exit()
{
    Fixture f;
    f.hold();
    const auto pause = f.record(f.core.pause(f.now));
    check(pause.action && pause.action->cleanup, "pause must synchronously clean up");
    check(pause.status.reason == Reason::UserPause, "explicit pause reason lost");
    f.record(f.core.pause(f.now));
    f.expect_no_down(f.tick());
    check(f.ups == 1, "repeated pause must not release twice");
    f.enable();
    f.expect_no_down(f.tick());
    check(!f.core.owns_a(), "A held across pause must not replay");
    f.snapshot.slots[0].buttons = 0;
    f.tick();
    f.snapshot.slots[0].buttons = a_button;
    f.tick();
    f.record(f.core.shutdown(f.now));
    check(!f.core.owns_a() && f.downs == f.ups, "exit must clear ownership");
    f.enable();
    f.expect_no_down(f.tick());
    check(f.core.status().reason == Reason::Exiting, "shutdown must be terminal");
}

void slot_switch_and_deselection()
{
    Fixture f;
    f.hold();
    f.snapshot.slots[1].buttons = a_button;
    f.select(1);
    check(!f.core.owns_a(), "switch must release old slot ownership immediately");
    f.expect_no_down(f.tick());
    f.snapshot.slots[1].buttons = 0;
    f.tick();
    f.snapshot.slots[1].buttons = a_button;
    f.tick();
    check(f.core.owns_a(), "new selected slot fresh input should work");
    f.record(f.core.select_slot({}, f.now));
    f.expect_no_down(f.tick());
    check(f.core.status().reason == Reason::NoSelection, "deselection must be visible");
    check(f.downs == f.ups, "deselection must balance ledger");
}

void reconnect_and_errors()
{
    Fixture f;
    f.hold();
    auto& c = f.snapshot.slots[0];
    c.connection = Connection::Disconnected;
    ++c.connection_epoch;
    check(f.tick().status.reason == Reason::Disconnected, "disconnect reason missing");
    check(!f.core.owns_a(), "disconnect must clean up");
    check(f.core.selected_slot() == 0u, "disconnect must not fail over");
    c.connection = Connection::Connected;
    ++c.connection_epoch;
    check(f.tick().status.reason == Reason::ConfirmSelection, "reconnect requires confirmation");
    f.enable();
    check(f.tick().status.reason == Reason::ConfirmSelection, "enable is not slot confirmation");
    f.select();
    f.expect_no_down(f.tick());
    c.buttons = 0;
    f.tick();
    c.buttons = a_button;
    f.tick();
    check(f.core.owns_a(), "fresh input after reconnect/reconfirm should work");
    // Worker can observe disconnect/reconnect between UI reads. The epoch preserves it.
    c.connection_epoch += 2;
    c.packet = 0;
    check(f.tick().status.reason == Reason::ConfirmSelection, "mailbox must preserve connection change");
    check(!f.core.owns_a(), "connection epoch change must release");
    f.select();
    c.connection = Connection::Error;
    c.error = 5;
    check(f.tick().status.reason == Reason::ReaderError, "unexpected reader failure must be visible");
    c.connection = Connection::Connected;
    check(f.tick().status.reason == Reason::ConfirmSelection, "error recovery must require confirmation");
}

void context_and_session_gates()
{
    for (bool context : {true, false}) {
        Fixture f;
        f.hold();
        if (context) {
            f.gates.context_allowed = false;
        } else {
            f.gates.session_available = false;
        }
        const auto lost = f.tick();
        check(lost.status.reason == (context ? Reason::ContextBlocked : Reason::SessionUnavailable),
              "lost gate reason missing");
        check(lost.action && lost.action->cleanup && !f.core.owns_a(), "blocked gates must allow cleanup");
        f.gates.context_allowed = f.gates.session_available = true;
        f.expect_no_down(f.tick());
        f.expect_no_down(f.tick());
        f.snapshot.slots[0].buttons = 0;
        f.tick();
        f.snapshot.slots[0].buttons = a_button;
        f.tick();
        check(f.core.owns_a(), "new input after gates restored should work");
        ++f.gates.context_generation;
        f.tick();
        check(!f.core.owns_a(), "allowed-to-allowed context switch must release");
        f.expect_no_down(f.tick());
    }
}

void physical_baseline_and_takeover()
{
    Fixture f;
    f.select();
    f.enable();
    f.gates.physical_baseline_known = false;
    check(f.tick().status.reason == Reason::PhysicalUnknown, "absence of events is not a baseline");
    f.gates.physical_baseline_known = true;
    f.gates.physical_modifiers = 1;
    check(f.tick().status.reason == Reason::PhysicalHeld, "preexisting modifier must block");
    f.gates.physical_modifiers = 0;
    f.gates.physical_mouse_buttons = 1;
    check(f.tick().status.reason == Reason::PhysicalHeld, "preexisting mouse button must block");
    f.gates.physical_mouse_buttons = 0;
    f.tick();
    f.tick();
    f.snapshot.slots[0].buttons = a_button;
    f.tick();
    check(f.core.owns_a(), "fresh A after physical neutrality should work");
    ++f.gates.physical_activity_generation;
    check(require_action(f.tick()).cleanup, "physical movement takeover must clean up");
    f.expect_no_down(f.tick());
    f.snapshot.slots[0].buttons = 0;
    f.tick();
    f.snapshot.slots[0].buttons = a_button;
    f.tick();
    f.gates.physical_baseline_known = false;
    check(f.tick().status.reason == Reason::PhysicalUnknown, "lost baseline must block");
    check(!f.core.owns_a(), "lost baseline must clean up");
}

void freshness_and_serialization()
{
    Fixture f;
    f.arm();
    f.record(f.core.pause(f.now));
    // A reader snapshot queued before pause cannot produce an action after it.
    f.snapshot.slots[0].buttons = a_button;
    f.expect_no_down(f.tick());
    f.enable();
    f.snapshot.slots[0].buttons = 0;
    f.expect_no_down(f.record(f.core.step(f.snapshot, f.gates, f.now)));
    check(f.core.status().state != State::Ready, "cached sample must not rearm after enable");
    f.tick();
    f.snapshot.slots[0].buttons = a_button;
    f.tick();
    check(f.core.owns_a(), "fresh samples should arm and press");
    f.now += maximum_sample_age_ms + 1;
    check(f.record(f.core.step(f.snapshot, f.gates, f.now)).status.reason == Reason::StaleSample,
          "reader stall must be visible");
    check(!f.core.owns_a(), "reader stall must release");
    f.expect_no_down(f.tick());
    f.expect_no_down(f.tick());
    f.snapshot.slots[0].buttons = 0;
    f.tick();
    f.snapshot.slots[0].buttons = a_button;
    f.tick();
    f.now += maximum_sample_age_ms + 1;
    f.tick(); // New snapshot after an unobserved scheduling gap.
    check(!f.core.owns_a(), "fresh packet must not hide a long polling gap");
    auto invalid = f.snapshot;
    --invalid.sequence;
    check(f.record(f.core.step(invalid, f.gates, f.now)).status.reason == Reason::InvalidSample,
          "out-of-order sample must be rejected");
    invalid = f.snapshot;
    invalid.sampled_at = f.now + 1;
    check(f.record(f.core.step(invalid, f.gates, f.now)).status.reason == Reason::InvalidSample,
          "future timestamp must be rejected");
}

void independent_slots_and_packet_identity()
{
    Fixture f;
    f.arm();
    f.snapshot.slots[1].buttons = a_button;
    f.snapshot.slots[1].connection_epoch += 2;
    f.expect_no_down(f.tick());
    check(f.core.status().state == State::Ready, "unselected device must not affect core");
    f.snapshot.slots[0].packet = 0xffffffff;
    f.snapshot.slots[0].buttons = a_button;
    f.tick();
    f.snapshot.slots[0].packet = 0;
    f.tick();
    check(f.downs == 1, "packet wrap must not imply new identity or repeat input");
}

void samples_queued_before_enable()
{
    Fixture f;
    f.select();
    f.tick();
    // The worker sampled neutral before the Enable command, but UI has not read it.
    ++f.snapshot.sequence;
    f.snapshot.sampled_at = ++f.now;
    ++f.now;
    f.enable();
    f.record(f.core.step(f.snapshot, f.gates, f.now));
    check(f.core.status().state == State::Paused, "pre-enable neutral cannot arm");
    f.snapshot.slots[0].buttons = a_button;
    f.expect_no_down(f.tick());
    f.snapshot.slots[0].buttons = 0;
    f.tick();
    f.snapshot.slots[0].buttons = a_button;
    check(require_action(f.tick()).kind == ActionKind::ADown, "post-enable neutral must allow fresh A");
}
} // namespace

int main()
{
    const std::pair<const char*, void(*)()> tests[] = {
        {"startup and explicit selection", startup_and_selection},
        {"initial held state and fresh edges", held_before_enable},
        {"neutrality across all controls", neutral_means_all_controls},
        {"pause, resume, and exit cleanup", pause_and_exit},
        {"slot switch and deselection", slot_switch_and_deselection},
        {"disconnect, reconnect, and errors", reconnect_and_errors},
        {"context and session gates", context_and_session_gates},
        {"physical baseline and takeover", physical_baseline_and_takeover},
        {"freshness and serialized cancellation", freshness_and_serialization},
        {"slot isolation and packet wrap", independent_slots_and_packet_identity},
        {"pre-enable mailbox snapshot barrier", samples_queued_before_enable},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::runtime_error& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << std::size(tests) << " cases, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
