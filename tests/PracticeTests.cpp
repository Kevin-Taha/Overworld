#include "Practice.h"
#include "LaunchMode.h"

#include <iostream>
#include <stdexcept>

using namespace gamepad;
using namespace gamepad::practice;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
struct Fixture {
    Model model;
    Snapshot sample;
    Tick now = 1000;
    Fixture()
    {
        model.focus(true);
        sample.slots[0].connection = Connection::Connected;
        sample.slots[0].connection_epoch = 1;
        tick();
    }
    Controller& pad() { return sample.slots[0]; }
    void tick(Tick elapsed = 16)
    {
        now += elapsed;
        sample.sampled_at = now;
        ++sample.sequence;
        model.step(sample, now);
    }
    void button(std::uint16_t value) { pad().buttons = value; tick(); pad().buttons = 0; tick(); }
};
void lessons()
{
    Fixture f;
    for (unsigned i = 0; i < 3; ++i) { f.model.move_local(aim_target(i)); }
    check(f.model.complete(), "aim requires three visited targets");
    f.model.select(Lesson::Click);
    f.model.move_local({300, 175});
    f.model.secondary_local();
    check(!f.model.complete(), "right click alone does not complete two-click lesson");
    f.model.primary_local(true);
    f.model.move_local({30, 30});
    f.model.primary_local(false);
    check(!f.model.primary_clicked(), "primary release outside card is not a click");
    f.model.move_local({300, 175});
    f.model.primary_local(true); f.model.primary_local(false);
    f.model.secondary_local();
    check(f.model.complete() && f.model.context_open(), "A then X selects and opens local options");
    f.model.select(Lesson::Drag);
    f.model.move_local({140, 190});
    f.model.primary_local(true);
    f.model.move_local({240, 190});
    check(f.model.dragging() && !f.model.complete(), "hold moves but is not a completed drop");
    f.model.primary_local(false);
    check(!f.model.complete(), "drop outside target not successful");
    f.model.primary_local(true);
    f.model.move_local({490, 210});
    f.model.primary_local(false);
    check(f.model.complete(), "complete only after full card is dropped inside target");
    f.model.select(Lesson::Scroll);
    f.model.move_local({20, 20}); f.model.scroll_local(-480);
    check(f.model.scroll_offset() == 0, "scroll outside list is ignored");
    f.model.move_local({300, 180}); f.model.scroll_local(-240);
    check(f.model.scroll_offset() == 240 && !f.model.complete(), "scroll progress stays local");
    f.model.scroll_local(-240);
    f.model.move_local({300, 300}); f.model.primary_local(true); f.model.primary_local(false);
    check(f.model.complete(), "revealed final card clicked");
    f.model.select(Lesson::Type);
    f.model.open_keyboard(); f.tick();
    f.model.keyboard_local(0, 0, f.now);
    check(f.model.keyboard().text() == L"a" && f.model.complete(), "fallback uses bounded Daisywheel sample");
    check(f.model.completed_count() == 5, "all five skills complete");
    f.model.reset();
    check(f.model.completed_count() == 0 && f.model.lesson() == Lesson::Aim
        && f.model.keyboard().text().empty(), "reset discards progress and local text without persistence");
}
void controller()
{
    Fixture f;
    const auto start = f.model.pointer();
    f.pad().left_x = 32767; f.tick(); f.tick();
    check(f.model.pointer().x > start.x, "real Pointer response drives scene");
    const auto before = f.model.pointer();
    f.pad().buttons = 0x0100; f.tick();
    check(f.model.pointer().x - before.x <= 5, "LB uses real precision multiplier");
    const auto held = f.model.pointer();
    f.model.focus(false); f.tick();
    check(f.model.pointer() == held && f.model.input() == InputState::Background, "background never moves");
    f.model.focus(true); f.tick();
    check(f.model.pointer() == held, "focus return with held stick cannot reacquire");
    f.pad().left_x = 0; f.pad().buttons = 0; f.tick();
    f.pad().left_x = 32767; f.tick(); f.tick();
    check(f.model.pointer().x > held.x, "fresh neutral followed by motion reacquires");
    f.model.physical_takeover();
    const auto physical = f.model.pointer();
    f.tick();
    check(f.model.pointer() == physical, "physical takeover rejects held controller input");
    f.pad().left_x = 0; f.tick();
    f.pad().left_x = 32767; f.tick(); f.tick();
    check(f.model.pointer().x > physical.x, "deliberate neutral restores scene-only control");
    const auto previous = f.model.pointer();
    f.tick(200);
    check(f.model.pointer() == previous, "consumer gap cannot replay motion");
    f.pad().left_x = 0; f.tick();
    f.pad().left_x = 32767; f.tick(); f.tick();
    f.pad().connection_epoch = 2; f.tick();
    const auto reconnect = f.model.pointer();
    f.tick();
    check(f.model.pointer() == reconnect && f.model.input() == InputState::Neutral, "reconnect rejects held input");
    f.pad().connection = Connection::Disconnected; f.tick();
    check(f.model.input() == InputState::NoController, "disconnect is explicit");
    f.pad().connection = Connection::Error; f.tick();
    check(f.model.input() == InputState::ReaderError, "reader failure is not success-shaped");
}
void safety()
{
    Fixture f;
    f.model.select(Lesson::Drag);
    f.tick();
    f.model.move_local({140, 190});
    f.pad().buttons = a_button; f.tick();
    check(f.model.dragging(), "controller A owns local drag");
    f.model.focus(false);
    check(!f.model.dragging() && !f.model.complete(), "focus loss cancels rather than drops");
    f.model.focus(true);
    f.tick(); f.pad().buttons = 0; f.tick();
    check(!f.model.complete(), "release after cancellation does not complete action");
    f.pad().left_x = 32767; f.tick(); f.tick();
    f.sample.slots[1] = f.pad();
    f.sample.slots[1].left_x = -32767;
    f.tick();
    check(f.model.input() == InputState::Multiple, "mixed controllers cancel");
    f.sample.slots[1].connection = Connection::Disconnected;
    const auto point = f.model.pointer();
    f.tick();
    check(f.model.pointer() == point, "unwinding ambiguous gesture cannot rearm remaining controller");
    f.pad().left_x = 0; f.tick();
    f.pad().left_x = 32767; f.tick();
    auto stale = f.sample;
    stale.sequence++;
    stale.sampled_at = f.now - 101;
    f.model.step(stale, f.now);
    check(f.model.input() == InputState::Stale, "stale snapshots explicitly rejected");
    f.model.step(f.sample, f.now - 500);
    check(f.model.input() == InputState::Stale, "clock rollback rejected");
}
void typing()
{
    Fixture f;
    f.button(0x8000);
    check(f.model.lesson() == Lesson::Type && f.model.keyboard_open() && f.model.keyboard().text().empty(),
        "Y jumps to the owned typing sample, opens, and does not type");
    f.tick();
    f.pad().left_y = 32767; f.tick();
    f.button(a_button);
    check(f.model.keyboard().text() == L"a" && f.model.complete(), "controller selects actual group and A character");
    f.button(0x0080);
    check(f.model.keyboard().text() == L"a", "RS cannot record or fake a voice success");
    f.pad().left_y = 0; f.tick();
    f.button(0x0020);
    check(!f.model.keyboard_open() && !f.model.exit_requested(), "View closes keyboard before practice");
    f.tick(); f.button(0x0020);
    check(f.model.exit_requested(), "View provides controller exit outside keyboard");
    f.model.reset(); f.model.select(Lesson::Type);
    f.model.one_handed(true); f.model.open_keyboard();
    f.model.keyboard_local(0, 0, f.now);
    check(f.model.keyboard().text() == L"a", "one-handed uses two-stage existing model");
    f.model.retry();
    check(!f.model.complete() && f.model.keyboard().text().empty(), "retry clears current sample and success");
}
void local_actions()
{
    Fixture f;
    f.model.move_local({480, 550});
    f.button(a_button);
    const auto click = f.model.take_click();
    check(click && click->down == live::Point{480, 550} && click->up == click->down,
        "controller can click local chrome outside the scene");
    check(!f.model.take_click(), "local action consumed once");
    f.pad().buttons = a_button; f.tick();
    f.model.focus(false); f.model.focus(true); f.pad().buttons = 0; f.tick();
    check(!f.model.take_click(), "focus loss cannot leave a deferred local action");
    f.model.show_help(true);
    for (unsigned i = 0; i < 3; ++i) { f.model.move_local(aim_target(i)); }
    check(!f.model.complete(), "help overlay cannot progress underlying scene");
    f.tick(); f.button(0x0020);
    check(!f.model.help() && !f.model.exit_requested(), "View returns from help without exiting");
    f.model.select(Lesson::Drag);
    f.model.move_local({140, 190}); f.model.primary_local(true); f.model.move_local({480, 210});
    f.model.cancel_local();
    check(!f.model.dragging() && !f.model.complete() && !f.model.take_click(), "mouse departure cancels rather than drops");
    check(f.model.display_scale(2) && !f.model.display_scale(0), "scale validation is explicit");
    f.tick(); f.pad().left_x = 32767; f.tick();
    const auto before = f.model.pointer();
    f.tick();
    check(f.model.pointer().x - before.x <= 9, "practice speed remains physical-pixel based at 192 DPI");
}
void controller_window_keys()
{
    Fixture f;
    LocalKeys keys;
    f.model.select(Lesson::Drag); f.tick();
    f.model.move_local({140, 190}); f.pad().buttons = a_button; f.tick();
    check(f.model.dragging(), "controller gesture begins before synthesized window messages");
    for (unsigned key = 0; key < 256; ++key) {
        const bool reserved = (key >= 0x88 && key <= 0x8f) || (key >= 0xc3 && key <= 0xda);
        if (!reserved) { continue; }
        check(!keys.observe(f.model, key, true) && !keys.observe(f.model, key, false),
            "own VK_NAVIGATION/GAMEPAD down and up are not physical takeover");
        check(!keys.held() && f.model.dragging() && f.model.input() == InputState::Ready,
            "controller-generated key messages cannot clear the drag or enter the physical hold ledger");
    }
    check(keys.observe(f.model, 0x25, true) && keys.held() && !f.model.dragging(),
        "ordinary left arrow still takes over and cancels the drag");
    check(keys.observe(f.model, 0x25, false) && !keys.held(), "ordinary release clears physical hold");
    f.pad().buttons = 0; f.model.select(Lesson::Type); f.tick(); f.button(0x8000); f.tick();
    f.pad().left_y = 32767; f.tick();
    check(f.model.keyboard().highlighted_petal() == 0u, "keyboard group selected");
    for (const unsigned key : {0x88u, 0x8fu, 0xc3u, 0xdau}) {
        keys.observe(f.model, key, true); keys.observe(f.model, key, false);
        check(f.model.keyboard().highlighted_petal() == 0u, "reserved keys cannot erase keyboard selection");
    }
    f.button(a_button);
    check(f.model.keyboard().text() == L"a" && f.model.complete(), "typing survives own synthesized key messages");
    for (const unsigned key : {0x26u, 0x0du, 0x10u, 0x41u, 0x87u, 0x90u, 0xc2u, 0xdbu}) {
        check(keys.observe(f.model, key, true) && keys.held(), "ordinary navigation, Enter, Shift, and letters remain physical");
        check(keys.observe(f.model, key, false) && !keys.held(), "ordinary key-up never remains held");
    }
    check(f.model.complete() && f.model.keyboard().text() == L"a", "real takeover keeps earned progress and local sample");
}
void unread_samples()
{
    Fixture f;
    auto unread = f.sample;
    ++unread.sequence;
    unread.sampled_at = f.now + 1;
    f.model.focus(false, f.now + 2);
    f.model.focus(true, f.now + 3);
    f.model.step(unread, f.now + 4);
    const auto point = f.model.pointer();
    f.pad().left_x = 32767;
    f.sample.sequence = unread.sequence;
    f.tick(); f.tick();
    check(f.model.pointer() == point, "unread neutral from before focus return cannot rearm a held controller");
    f.pad().left_x = 0; f.tick();
    f.pad().left_x = 32767; f.tick(); f.tick();
    check(f.model.pointer().x > point.x, "only neutral sampled after focus return permits movement");
    f.model.physical_takeover();
    f.model.discard_samples_through(f.now + 2);
    unread = f.sample; unread.slots[0].left_x = 0; ++unread.sequence;
    unread.sampled_at = f.now + 2;
    f.model.step(unread, f.now + 3);
    const auto after = f.model.pointer();
    f.sample.sequence = unread.sequence; f.tick(); f.tick();
    check(f.model.pointer() == after, "equal-timestamp pre-takeover neutral is conservatively discarded");
}
void launch()
{
    check(reader_lease_current(1050, 1000) && reader_lease_current(1050, 1049), "reader lease permits fresh focused sampling");
    check(!reader_lease_current(1050, 1050) && !reader_lease_current(1050, 1051)
        && !reader_lease_current(1050, 999) && !reader_lease_current(0, 0),
        "expired, future-clock, and absent reader leases fail closed");
    check(launch_mode({}) == LaunchMode::Desktop, "normal default unchanged");
    for (const auto flag : {L"--practice", L"--inspect", L"--keyboard-preview", L"--keyboard-preview-one-handed"}) {
        const std::array<std::wstring_view, 1> args{flag};
        check(launch_mode(args) != LaunchMode::Desktop && launch_mode(args) != LaunchMode::Invalid, "explicit no-output mode recognized");
        const std::array<std::wstring_view, 2> mixed{flag, L"--restore-cursor"};
        check(launch_mode(mixed) == LaunchMode::Invalid, "mixed flags never activate output or cursor recovery");
    }
    const std::array<std::wstring_view, 1> invalid{L"--Practice"};
    check(launch_mode(invalid) == LaunchMode::Invalid, "typos fail closed");
}
}
int main()
{
    try { launch(); lessons(); controller(); safety(); typing(); local_actions(); controller_window_keys(); unread_samples(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Practice: launch isolation, five skills, real models, reconnect, takeover, reset passed.\n";
}
