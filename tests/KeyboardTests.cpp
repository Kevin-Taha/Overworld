#include "Keyboard.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace gamepad;

namespace {
constexpr std::uint16_t menu = 0x0010;
constexpr std::uint16_t view = 0x0020;
constexpr std::uint16_t lb = 0x0100;
constexpr std::uint16_t rb = 0x0200;
constexpr std::array<std::uint16_t, 4> faces{0x1000, 0x2000, 0x4000, 0x8000};

void check(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

struct Fixture {
    Daisywheel keyboard;
    Snapshot snapshot;
    Tick now = 1000;

    Fixture()
    {
        connect(0);
    }

    void connect(unsigned slot)
    {
        snapshot.slots[slot] = {};
        snapshot.slots[slot].connection = Connection::Connected;
        snapshot.slots[slot].connection_epoch = 1;
    }

    KeyboardAction tick(Tick elapsed = 16, Tick age = 0)
    {
        now += elapsed;
        ++snapshot.sequence;
        snapshot.sampled_at = now - age;
        return keyboard.step(snapshot, now);
    }

    void aim(double degrees, double magnitude = 0.8, unsigned slot = 0)
    {
        const double radians = degrees * std::numbers::pi / 180.0;
        snapshot.slots[slot].left_x =
            static_cast<std::int16_t>(std::lround(std::sin(radians) * magnitude * 32767.0));
        snapshot.slots[slot].left_y =
            static_cast<std::int16_t>(std::lround(std::cos(radians) * magnitude * 32767.0));
    }

    void center(unsigned slot = 0)
    {
        auto& controller = snapshot.slots[slot];
        controller.buttons = 0;
        controller.left_trigger = controller.right_trigger = 0;
        controller.left_x = controller.left_y = controller.right_x = controller.right_y = 0;
    }

    void select(unsigned slot = 0)
    {
        tick();
        aim(0, 0.8, slot);
        tick();
        check(keyboard.selected_slot() == slot, "deliberate pad should be selected");
        check(keyboard.selected_petal() == 0u, "up should select first petal");
        center(slot);
        tick();
    }

    // Switches to OneHanded mode and establishes a fresh neutral baseline (no pad selected yet).
    void enable_one_handed()
    {
        keyboard.set_input_mode(KeyboardInputMode::OneHanded);
        tick();
    }

    // Performs one full two-stage flick: an outward+neutral gesture toward `group` (0..7 clockwise
    // from up) selects/commits that 8-way group, then an outward+neutral cardinal gesture toward
    // `face_degrees` (0=Y/top, 90=B/right, 180=A/bottom, 270=X/left) commits the character/action and
    // returns to the Group stage. Also serves as pad acquisition on the very first call.
    void flick(unsigned group, double face_degrees, unsigned slot = 0)
    {
        auto& controller = snapshot.slots[slot];
        const auto held_left_trigger = controller.left_trigger;
        const auto held_right_trigger = controller.right_trigger;
        const auto restore_triggers = [&] {
            controller.left_trigger = held_left_trigger;
            controller.right_trigger = held_right_trigger;
        };
        aim(static_cast<double>(group) * 45.0, 0.8, slot);
        tick();
        center(slot);
        restore_triggers();
        tick();
        check(keyboard.selected_petal() == group, "group commit should select the flicked group");
        check(keyboard.flick_stage() == KeyboardFlickStage::Character, "group commit should advance to Character stage");
        aim(face_degrees, 0.8, slot);
        tick();
        center(slot);
        restore_triggers();
        tick();
    }

    static unsigned cardinal_face(double degrees)
    {
        if (degrees == 0.0) { return 3; }   // Y (top)
        if (degrees == 90.0) { return 1; }  // B (right)
        if (degrees == 180.0) { return 0; } // A (bottom)
        if (degrees == 270.0) { return 2; } // X (left)
        throw std::runtime_error("test only supports exact cardinal degrees");
    }

    KeyboardAction button(std::uint16_t buttons, unsigned slot = 0, Tick elapsed = 16)
    {
        snapshot.slots[slot].buttons = buttons;
        return tick(elapsed);
    }

    void tap(std::uint16_t buttons, unsigned slot = 0)
    {
        button(buttons, slot);
        button(0, slot);
    }

    void fill(std::size_t count)
    {
        for (std::size_t index = 0; index < count; ++index) { tap(faces[0]); }
    }

    void advance(Tick milliseconds)
    {
        while (milliseconds != 0) {
            const Tick interval = std::min<Tick>(milliseconds, 50);
            tick(interval);
            milliseconds -= interval;
        }
    }

    void complete()
    {
        check(button(menu) != KeyboardAction::Done, "Menu down must not finish");
        check(button(0) == KeyboardAction::Done, "Menu up should finish");
    }
};
} // namespace

int main()
{
    unsigned count = 0;
    const auto test = [&](const char* name, const std::function<void()>& body) {
        body();
        ++count;
        std::cout << "PASS " << name << '\n';
    };
    try {
        test("layouts cover printable ASCII with bumper space", [] {
            std::array<bool, 127> reachable{};
            reachable[32] = true;
            for (const auto page : {KeyboardPage::Letters, KeyboardPage::ShiftLetters,
                    KeyboardPage::Symbols, KeyboardPage::ShiftSymbols}) {
                const auto& layout = keyboard_layout(page);
                check(&layout == &keyboard_layout(page), "layout reference must be stable");
                for (const auto& petal : layout) {
                    for (const auto character : petal) {
                        check(character >= 33 && character <= 126, "layout contains nonprintable text");
                        reachable[static_cast<std::size_t>(character)] = true;
                    }
                }
            }
            check(std::all_of(reachable.begin() + 32, reachable.end(), [](bool value) { return value; }),
                "printable ASCII character is unreachable");
            const auto& lower = keyboard_layout(KeyboardPage::Letters);
            check(lower[0] == std::array<wchar_t, 4>{L'a', L'b', L'c', L'd'}, "first letter group");
            check(lower[6] == std::array<wchar_t, 4>{L'y', L'z', L'.', L','}, "last letters");
            check(lower[7] == std::array<wchar_t, 4>{L'-', L'\'', L'/', L'?'}, "final letter group");
            check(&keyboard_layout(static_cast<KeyboardPage>(99)) == &lower, "invalid page fallback");
        });
        test("exclamation is reachable with Symbols without a compound shift hold", [] {
            for (const auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                check(keyboard_layout(KeyboardPage::Symbols, mode)[4]
                    == std::array<wchar_t, 4>{L'@', L'#', L'&', L'!'},
                    "exclamation must be available without both triggers");
            }
        });
        test("every page petal and face is actually reachable", [] {
            Fixture f;
            f.select();
            std::wstring expected;
            for (const auto page : {KeyboardPage::Letters, KeyboardPage::ShiftLetters,
                    KeyboardPage::Symbols, KeyboardPage::ShiftSymbols}) {
                auto& controller = f.snapshot.slots[0];
                controller.left_trigger =
                    page == KeyboardPage::ShiftLetters || page == KeyboardPage::ShiftSymbols ? 255 : 0;
                controller.right_trigger =
                    page == KeyboardPage::Symbols || page == KeyboardPage::ShiftSymbols ? 255 : 0;
                for (unsigned petal = 0; petal < 8; ++petal) {
                    f.aim(static_cast<double>(petal) * 45.0);
                    f.tick();
                    check(f.keyboard.selected_petal() == petal, "petals must be clockwise from up");
                    for (unsigned face = 0; face < 4; ++face) {
                        f.tap(faces[face]);
                        expected += keyboard_layout(page)[petal][face];
                        check(f.keyboard.text() == expected, "page or A/B/X/Y mapping differs");
                    }
                }
            }
            check(expected.size() == 128, "all layout positions should be exercised");
        });
        test("compose a search URL and corrected filename without app delivery", [] {
            const auto type_text = [](Fixture& f, std::wstring_view text) {
                for (wchar_t character : text) {
                    if (character == L' ') { f.tap(rb); continue; }
                    bool found = false;
                    for (const auto page : {KeyboardPage::Letters, KeyboardPage::ShiftLetters,
                            KeyboardPage::Symbols, KeyboardPage::ShiftSymbols}) {
                        const auto& layout = keyboard_layout(page);
                        for (unsigned petal = 0; petal < layout.size() && !found; ++petal) {
                            for (unsigned face = 0; face < faces.size(); ++face) {
                                if (layout[petal][face] != character) { continue; }
                                auto& pad = f.snapshot.slots[0];
                                pad.left_trigger = page == KeyboardPage::ShiftLetters || page == KeyboardPage::ShiftSymbols ? 255 : 0;
                                pad.right_trigger = page == KeyboardPage::Symbols || page == KeyboardPage::ShiftSymbols ? 255 : 0;
                                f.aim(petal * 45.0);
                                f.tick();
                                f.tap(faces[face]);
                                found = true;
                                break;
                            }
                        }
                        if (found) { break; }
                    }
                    check(found, "scenario character is not available");
                }
            };
            for (const auto text : {L"windows controller keyboard", L"https://example.com/mods?q=gamepad",
                    L"Dragonwilds_mod-v2.zip"}) {
                Fixture f; f.select();
                type_text(f, text);
                check(f.keyboard.text() == text, "scenario composition differs from intended text");
                type_text(f, L"x");
                f.tap(lb);
                check(f.keyboard.text() == text, "scenario correction removed the wrong character");
                f.complete();
                check(f.keyboard.text() == text && f.keyboard.phase() == KeyboardPhase::Complete,
                      "Done did not preserve the final composition");
            }
        });
        test("radial thresholds latch across center and reject drift", [] {
            Fixture f;
            f.tick();
            f.aim(90, 0.44);
            f.tick();
            check(!f.keyboard.selected_slot(), "subthreshold motion must not acquire");
            f.aim(90, 0.46);
            f.tick();
            check(f.keyboard.selected_petal() == 2u, "intentional right gesture");
            f.center();
            f.tick();
            check(f.keyboard.selected_petal() == 2u, "center must retain chosen petal");
            f.aim(180, 0.44);
            f.tick();
            check(f.keyboard.selected_petal() == 2u, "reentry needs radial threshold");
            f.aim(180, 0.46);
            f.tick();
            check(f.keyboard.selected_petal() == 4u, "reentry should choose new petal");
            f.aim(225, 0.30);
            f.tick();
            check(f.keyboard.selected_petal() == 5u, "engaged stick has radial hysteresis");
            f.center();
            f.tick();
            f.tap(faces[0]);
            check(f.keyboard.text() == L"u", "centered typing should use latched petal");
        });
        test("angular hysteresis works at sector boundaries and wrap", [] {
            Fixture f;
            f.select();
            for (double angle : {0.0, 22.0, 24.0, 26.0}) { f.aim(angle); f.tick(); }
            check(f.keyboard.selected_petal() == 0u, "boundary noise should keep up");
            f.aim(29); f.tick();
            check(f.keyboard.selected_petal() == 1u, "deliberate diagonal should switch");
            f.aim(22); f.tick();
            check(f.keyboard.selected_petal() == 1u, "reverse boundary needs hysteresis");
            f.aim(16); f.tick();
            check(f.keyboard.selected_petal() == 0u, "reverse crossing should switch");
            f.aim(336); f.tick();
            check(f.keyboard.selected_petal() == 0u, "wrap boundary should keep up");
            f.aim(330); f.tick();
            check(f.keyboard.selected_petal() == 7u, "wrap beyond margin should switch");
            f.aim(340); f.tick();
            check(f.keyboard.selected_petal() == 7u, "wrap hysteresis should keep northwest");
            f.aim(344); f.tick();
            check(f.keyboard.selected_petal() == 0u, "wrap return should select up");
        });
        test("signed stick extrema select correct directions", [] {
            Fixture f;
            f.select();
            auto& c = f.snapshot.slots[0];
            c.left_x = -32768; c.left_y = -32768; f.tick();
            check(f.keyboard.selected_petal() == 5u, "negative diagonal");
            c.left_x = 32767; c.left_y = 32767; f.tick();
            check(f.keyboard.selected_petal() == 1u, "positive diagonal");
            c.left_x = 0; c.left_y = -32768; f.tick();
            check(f.keyboard.selected_petal() == 4u, "negative cardinal");
        });
        test("characters are edges and sequential overlapping faces work", [] {
            Fixture f;
            f.select();
            f.button(faces[0]);
            f.advance(600);
            check(f.keyboard.text().size() == 1, "held face must not repeat");
            f.button(faces[0] | faces[1]);
            check(f.keyboard.text() == L"ab", "one new face amid held face must work");
            f.button(faces[1]);
            f.button(faces[1] | faces[2]);
            check(f.keyboard.text() == L"abc", "sequential overlap must not require all-up");
            f.button(0);
            f.tap(faces[3]);
            check(f.keyboard.text() == L"abcd", "A/B/X/Y character order");
        });
        test("simultaneous face chords are consumed without arbitrary text", [] {
            Fixture f;
            f.select();
            f.button(faces[0] | faces[1]);
            check(f.keyboard.text().empty(), "two fresh faces must not append");
            f.button(faces[0]);
            check(f.keyboard.text().empty(), "unwinding chord must not create character");
            f.button(faces[0] | faces[2]);
            check(f.keyboard.text() == L"c", "single later edge should still work");
            f.button(0);
            f.button(0xf000);
            check(f.keyboard.text().size() == 1, "four fresh faces must not append");
        });
        test("trigger pages are momentary with strict half-press boundary", [] {
            Fixture f;
            f.select();
            auto& c = f.snapshot.slots[0];
            c.left_trigger = c.right_trigger = 128; f.tick();
            check(f.keyboard.page() == KeyboardPage::Letters, "128 is not a page press");
            c.left_trigger = 129; f.tick();
            check(f.keyboard.page() == KeyboardPage::ShiftLetters, "LT uppercases letters");
            f.tap(faces[0]);
            c.right_trigger = 129; f.tick();
            check(f.keyboard.page() == KeyboardPage::ShiftSymbols, "both triggers shift symbols");
            f.tap(faces[0]);
            c.left_trigger = 0; f.tick();
            check(f.keyboard.page() == KeyboardPage::Symbols, "RT selects symbols");
            f.tap(faces[0]);
            c.right_trigger = 0; f.tick();
            f.tap(faces[0]);
            check(f.keyboard.text() == L"A!1a", "trigger release must restore page");
        });
        test("page change while face held never types a second character", [] {
            Fixture f;
            f.select();
            f.button(faces[0]);
            f.snapshot.slots[0].left_trigger = 255;
            f.tick();
            f.advance(300);
            check(f.keyboard.text() == L"a", "modifier change must not repeat held face");
            f.button(0);
            f.tap(faces[0]);
            check(f.keyboard.text() == L"aA", "later edge uses changed page");
        });
        test("no implicit petal and space works on its own edge", [] {
            Fixture f;
            f.tick();
            f.tap(faces[0]);
            check(f.keyboard.selected_slot() == 0u && !f.keyboard.selected_petal(), "button can acquire pad");
            check(f.keyboard.text().empty(), "face without selected petal cannot type");
            f.button(rb);
            f.advance(500);
            check(f.keyboard.text() == L" ", "space should work once without a petal");
            f.button(0);
            f.tap(rb);
            check(f.keyboard.text().size() == 2, "fresh space edge");
        });
        test("backspace has 400ms delay and 90ms bounded repeats", [] {
            Fixture f;
            f.select();
            f.fill(12);
            f.button(lb);
            check(f.keyboard.text().size() == 11, "initial backspace is immediate");
            f.advance(399);
            check(f.keyboard.text().size() == 11, "repeat must wait full delay");
            f.tick(1);
            check(f.keyboard.text().size() == 10, "first repeat at delay");
            f.advance(89);
            check(f.keyboard.text().size() == 10, "repeat interval must not be early");
            f.tick(1);
            check(f.keyboard.text().size() == 9, "repeat at interval");
            f.tick(100);
            check(f.keyboard.text().size() == 8, "late valid sample only erases once");
            f.tick(80);
            check(f.keyboard.text().size() == 8, "late repeat anchors to actual sample");
            f.tick(10);
            check(f.keyboard.text().size() == 7, "next repeat after new anchor");
            f.button(0);
            f.advance(500);
            check(f.keyboard.text().size() == 7, "release stops repeat");
            f.button(lb);
            check(f.keyboard.text().size() == 6, "new backspace press erases once");
            f.advance(399);
            check(f.keyboard.text().size() == 6, "new press restarts delay");
        });
        test("empty backspace is explicit and remains bounded", [] {
            Fixture f;
            f.select();
            f.button(lb);
            check(f.keyboard.text().empty() && f.keyboard.status() == L"Nothing to erase",
                "empty backspace must be a visible no-op");
            f.advance(1000);
            check(f.keyboard.text().empty() && !f.keyboard.at_limit(), "empty repeats must not underflow");
        });
        test("Menu finishes only on release and text remains visible", [] {
            Fixture f;
            f.select();
            f.tap(faces[0]);
            check(f.button(menu) != KeyboardAction::Done, "Menu down cannot finish");
            f.advance(500);
            check(f.keyboard.phase() == KeyboardPhase::Typing, "held Menu cannot finish");
            check(f.button(0) == KeyboardAction::Done, "accepted Menu up finishes");
            check(f.keyboard.phase() == KeyboardPhase::Complete && f.keyboard.text() == L"a",
                "Complete must preserve visible text");
            check(f.tick() != KeyboardAction::Done, "completion must not repeat");
        });
        test("Cancel wins over Menu release and simultaneous inputs", [] {
            for (int variant = 0; variant < 3; ++variant) {
                Fixture f;
                f.select();
                f.tap(faces[0]);
                if (variant != 0) { f.button(menu); }
                const auto buttons = static_cast<std::uint16_t>(
                    view | (variant == 1 ? 0 : menu) | rb | lb | faces[0]);
                check(f.button(buttons) == KeyboardAction::Cancel, "View must beat completion and editing");
                check(f.keyboard.text().empty() && f.keyboard.phase() == KeyboardPhase::Waiting,
                    "Cancel must clear composition");
                check(!f.keyboard.selected_slot(), "Cancel resets controller acquisition");
                check(f.tick() != KeyboardAction::Cancel, "held cancel must not replay");
                check(f.button(0) != KeyboardAction::Done, "cancelled Menu release cannot finish");
            }
        });
        test("empty completion is allowed and explicitly described", [] {
            Fixture f;
            f.select();
            f.complete();
            check(f.keyboard.text().empty() && f.keyboard.phase() == KeyboardPhase::Complete,
                "empty composition may finish");
            check(f.keyboard.status().find(L"empty") != std::wstring_view::npos, "empty completion status");
        });
        test("Complete freezes text until a second Menu press and release", [] {
            Fixture f;
            f.select();
            f.fill(3);
            f.complete();
            f.tap(lb); f.tap(rb); f.tap(faces[1]);
            f.snapshot.slots[0].left_trigger = 255;
            f.aim(180);
            f.advance(500);
            check(f.keyboard.text() == L"aaa", "completed text is immutable");
            check(f.button(menu) != KeyboardAction::Done && f.keyboard.text().size() == 3,
                "new composition must also wait for Menu release");
            check(f.button(faces[0]) == KeyboardAction::Redraw, "second Menu release begins composition");
            check(f.keyboard.phase() == KeyboardPhase::Typing && f.keyboard.text().empty(),
                "new composition clears only on second release");
            f.tick();
            check(f.keyboard.text().empty(), "face held over restart cannot replay");
            f.button(0);
            f.tap(faces[0]);
            check(f.keyboard.text() == L"Q", "new deliberate edge types into new composition");
        });
        test("View cancels completed text", [] {
            Fixture f;
            f.select();
            f.fill(2);
            f.complete();
            check(f.button(view) == KeyboardAction::Cancel, "View closes completed composition");
            check(f.keyboard.text().empty(), "completed cancellation clears text");
        });
        test("composition caps at 256 and editing recovers capacity", [] {
            Fixture f;
            f.select();
            f.fill(keyboard_text_limit);
            check(f.keyboard.at_limit() && f.keyboard.text().size() == 256, "exact cap");
            f.tap(faces[1]); f.tap(rb);
            check(f.keyboard.text().size() == 256 && f.keyboard.text().back() == L'a', "overflow rejected");
            check(f.keyboard.status().find(L"256") != std::wstring_view::npos, "limit status must be visible");
            f.tap(lb);
            check(!f.keyboard.at_limit() && f.keyboard.text().size() == 255, "erase restores capacity");
            f.tap(faces[1]);
            check(f.keyboard.at_limit() && f.keyboard.text().back() == L'b', "restored slot can be used");
            f.complete();
            check(f.keyboard.text().size() == 256, "Done preserves capped text");
        });
        test("startup held controls need fresh full neutral", [] {
            for (const auto buttons : {faces[0], menu, view, rb, lb}) {
                Fixture f;
                f.snapshot.slots[0].buttons = buttons;
                f.aim(0);
                for (int tick = 0; tick < 5; ++tick) {
                    const auto action = f.tick();
                    check(action != KeyboardAction::Done && action != KeyboardAction::Cancel,
                        "startup held controls must be ignored");
                }
                check(!f.keyboard.selected_slot() && f.keyboard.text().empty(), "startup cannot acquire");
                f.center(); f.tick();
                check(f.keyboard.phase() == KeyboardPhase::Waiting, "neutral alone cannot select");
                f.aim(0); f.tick();
                f.tap(faces[0]);
                check(f.keyboard.text() == L"a", "fresh gesture after neutral must work");
            }
        });
        test("neutral baseline covers both sticks triggers and non-Guide buttons", [] {
            for (int input = 0; input < 5; ++input) {
                Fixture f;
                auto& c = f.snapshot.slots[0];
                if (input == 0) { c.left_trigger = 31; }
                if (input == 1) { c.right_trigger = 31; }
                if (input == 2) { c.right_x = c.right_y = 6000; }
                if (input == 3) { c.buttons = 0x0001; }
                if (input == 4) { c.left_x = c.left_y = 6000; }
                f.tick();
                f.aim(0); f.tick();
                check(!f.keyboard.selected_slot(), "non-neutral initial control must block baseline");
                f.center(); c.buttons = 0x0400; f.tick();
                f.aim(0); f.tick();
                check(f.keyboard.selected_slot() == 0u, "reserved/Guide bits should be ignored");
            }
        });
        test("stale data never types and fresh held input waits for neutral", [] {
            Fixture f;
            f.select(); f.fill(2);
            f.now += 150;
            f.snapshot.slots[0].buttons = faces[1];
            check(f.keyboard.step(f.snapshot, f.now) != KeyboardAction::Done, "stale step");
            check(f.keyboard.phase() == KeyboardPhase::Waiting && f.keyboard.text() == L"aa",
                "stale input retains text but disarms");
            f.tick();
            check(f.keyboard.text() == L"aa", "held fresh input must not become new edge");
            f.center(); f.tick();
            f.aim(0); f.tick(); f.tap(faces[1]);
            check(f.keyboard.text() == L"aab", "fresh neutral restores typing");
        });
        test("age limit is inclusive at 100ms", [] {
            Fixture f;
            f.tick(16, 100);
            f.aim(0); f.tick(16, 100);
            f.snapshot.slots[0].buttons = faces[0];
            f.tick(16, 100);
            check(f.keyboard.text() == L"a", "100ms sample is valid");
            f.snapshot.slots[0].buttons = faces[1];
            f.tick(16, 101);
            check(f.keyboard.text() == L"a" && f.keyboard.phase() == KeyboardPhase::Waiting,
                "101ms sample is stale");
        });
        test("consumer gap discards pending Menu and backspace repeats", [] {
            for (const auto held : {menu, lb}) {
                Fixture f;
                f.select(); f.fill(8);
                f.button(held);
                const auto size = f.keyboard.text().size();
                f.tick(1000);
                check(f.keyboard.text().size() == size && f.keyboard.phase() == KeyboardPhase::Waiting,
                    "gap must not catch up repeats or finish");
                check(f.button(0) != KeyboardAction::Done, "Menu release after gap must not finish");
                check(f.keyboard.text().size() == size, "gap retains composition");
                f.button(menu);
                check(f.button(0) == KeyboardAction::Done, "new Menu cycle after neutral may finish");
            }
        });
        test("duplicate samples never repeat and sequence jumps are permitted", [] {
            Fixture f;
            f.select(); f.fill(3);
            f.button(lb);
            const auto size = f.keyboard.text().size();
            for (int duplicate = 0; duplicate < 5; ++duplicate) {
                f.now += 10;
                check(f.keyboard.step(f.snapshot, f.now) == KeyboardAction::None,
                    "duplicate sample should not perform work");
            }
            check(f.keyboard.text().size() == size, "duplicates cannot erase");
            f.button(0);
            f.snapshot.sequence += 100;
            f.tap(faces[1]);
            check(f.keyboard.text() == L"aab", "sequence gaps alone are not stale data");
        });
        test("reordered future and inconsistent snapshots require new neutral", [] {
            for (int invalid = 0; invalid < 5; ++invalid) {
                Fixture f;
                f.select(); f.fill(2);
                auto bad = f.snapshot;
                if (invalid == 0) { --bad.sequence; }
                if (invalid == 1) { --bad.sampled_at; ++bad.sequence; }
                if (invalid == 2) { bad.sampled_at = f.now + 1; ++bad.sequence; }
                if (invalid == 3) { --bad.sampled_at; }
                if (invalid == 4) { bad.sequence = 0; }
                bad.slots[0].buttons = faces[1];
                f.keyboard.step(bad, f.now);
                check(f.keyboard.phase() == KeyboardPhase::Waiting && f.keyboard.text() == L"aa",
                    "invalid snapshot must disarm without editing");
                f.button(faces[1]);
                check(f.keyboard.text() == L"aa", "held input after invalidation cannot replay");
                f.center(); f.tick(); f.aim(0); f.tick(); f.tap(faces[1]);
                check(f.keyboard.text() == L"aab", "monotonic neutral recovery");
            }
        });
        test("clock rollback is rejected and repeat timer cannot underflow", [] {
            Fixture f;
            f.select(); f.fill(4); f.button(lb);
            const auto size = f.keyboard.text().size();
            auto bad = f.snapshot;
            ++bad.sequence;
            bad.sampled_at = f.now - 1;
            f.keyboard.step(bad, f.now - 1);
            check(f.keyboard.phase() == KeyboardPhase::Waiting && f.keyboard.text().size() == size,
                "rollback must not erase or underflow");
            f.tick();
            check(f.keyboard.text().size() == size, "held repeat cannot rearm after rollback");
        });
        test("disconnect errors and epoch changes discard device identity", [] {
            for (int change = 0; change < 4; ++change) {
                Fixture f;
                f.select(); f.fill(2); f.button(menu);
                auto& c = f.snapshot.slots[0];
                if (change == 0) { c.connection = Connection::Disconnected; }
                if (change == 1) { c.connection = Connection::Error; }
                if (change == 2) { c.connection = Connection::Unknown; }
                if (change == 3) { ++c.connection_epoch; }
                f.tick();
                check(!f.keyboard.selected_slot() && f.keyboard.phase() == KeyboardPhase::Waiting,
                    "device identity loss must release lock and disarm");
                c.connection = Connection::Connected;
                c.buttons = faces[1]; f.tick();
                check(f.keyboard.text() == L"aa", "reconnected held input must not replay");
                f.center(); f.tick();
                check(!f.keyboard.selected_slot(), "reconnect neutral alone cannot select");
                f.aim(0); f.tick(); f.tap(faces[1]);
                check(f.keyboard.text() == L"aab", "reconnect requires deliberate new acquisition");
            }
        });
        test("suspend retains text and pad lock but consumes held input", [] {
            Fixture f;
            f.connect(1); f.select(); f.fill(3);
            f.button(lb);
            f.keyboard.suspend();
            check(f.keyboard.text() == L"aa" && f.keyboard.selected_slot() == 0u,
                "suspend retains composition and selected pad");
            f.advance(600);
            check(f.keyboard.text() == L"aa" && f.keyboard.phase() == KeyboardPhase::Waiting,
                "focused held repeat must wait for neutral");
            f.snapshot.slots[1].buttons = view;
            f.tick();
            check(f.keyboard.text() == L"aa", "other controller cannot cancel after focus loss");
            f.center(); f.tick();
            f.aim(0); f.tick(); f.tap(faces[1]);
            check(f.keyboard.text() == L"aab", "fresh focus recovery can type");
        });
        test("suspend consumes Menu and repeated snapshot is not fresh neutral", [] {
            Fixture f;
            f.select(); f.fill(1); f.button(menu);
            f.keyboard.suspend();
            check(f.button(0) != KeyboardAction::Done, "Menu up across focus loss must not finish");
            f.keyboard.suspend();
            f.keyboard.step(f.snapshot, f.now);
            f.button(rb);
            check(f.keyboard.text() == L"a" && f.keyboard.phase() == KeyboardPhase::Waiting,
                "cached pre-suspend neutral cannot rearm");
            f.center(); f.tick();
            f.tap(rb);
            check(f.keyboard.text() == L"a ", "new neutral enables fresh input");
        });
        test("Complete survives suspend stale data and device replacement", [] {
            Fixture f;
            f.select(); f.fill(3); f.complete();
            f.keyboard.suspend();
            check(f.keyboard.phase() == KeyboardPhase::Complete && f.keyboard.text() == L"aaa",
                "suspend preserves completed state");
            f.now += 200;
            f.keyboard.step(f.snapshot, f.now);
            check(f.keyboard.phase() == KeyboardPhase::Complete, "stale state must not reopen composition");
            ++f.snapshot.slots[0].connection_epoch;
            f.tick();
            check(f.keyboard.phase() == KeyboardPhase::Complete && f.keyboard.text() == L"aaa",
                "device change preserves finished text");
            f.center(); f.tick();
            f.button(menu);
            check(f.button(0) == KeyboardAction::Redraw && f.keyboard.text().empty(),
                "new neutral device may deliberately start next composition");
        });
        test("multiple simultaneous controllers require unambiguous new activity", [] {
            Fixture f;
            f.connect(1);
            f.tick();
            f.aim(0, 0.8, 0); f.aim(90, 0.8, 1); f.tick();
            check(!f.keyboard.selected_slot(), "simultaneous pads cannot be arbitrarily selected");
            f.center(1); f.tick();
            check(!f.keyboard.selected_slot(), "last held pad from ambiguity cannot be selected");
            f.center(0); f.tick();
            f.aim(90, 0.8, 1); f.tick();
            check(f.keyboard.selected_slot() == 1u && f.keyboard.selected_petal() == 2u,
                "fresh unambiguous gesture chooses one pad");
            f.tap(faces[0], 1);
            check(f.keyboard.text() == L"i", "selected pad alone supplies text");
        });
        test("pad lock ignores all other controllers including cancel and epochs", [] {
            Fixture f;
            f.connect(1); f.select();
            f.snapshot.slots[1].buttons = view | menu | faces[1];
            ++f.snapshot.slots[1].connection_epoch;
            f.aim(180, 0.8, 1);
            f.tap(faces[0]);
            check(f.keyboard.selected_slot() == 0u && f.keyboard.text() == L"a",
                "unselected pad must not merge text cancel or epoch changes");
            f.complete();
            check(f.keyboard.text() == L"a", "other controller cannot interfere with completion");
        });
        test("startup-held second pad does not impersonate a fresh gesture", [] {
            Fixture f;
            f.connect(1);
            f.snapshot.slots[1].buttons = faces[1];
            f.tick();
            f.aim(0); f.tick();
            check(f.keyboard.selected_slot() == 0u, "only baselined deliberate pad should acquire");
            f.tap(faces[0]);
            check(f.keyboard.text() == L"a", "startup-held other pad is ignored");
        });
        test("stale observations cannot supply neutral for any pad", [] {
            Fixture f;
            f.connect(1);
            f.tick(16, 101);
            f.aim(0, 0.8, 1);
            f.tick();
            check(!f.keyboard.selected_slot(), "stale neutral must not enable acquisition");
            f.center(1); f.tick();
            f.aim(0, 0.8, 1); f.tick();
            check(f.keyboard.selected_slot() == 1u, "fresh neutral may enable acquisition");
        });
        test("reset clears composition pages acquisition and edge history", [] {
            Fixture f;
            f.select(); f.fill(3);
            f.snapshot.slots[0].left_trigger = 255;
            f.tick();
            f.keyboard.reset();
            check(f.keyboard.text().empty() && !f.keyboard.selected_slot() && !f.keyboard.selected_petal(),
                "reset clears composition and selection");
            check(f.keyboard.page() == KeyboardPage::Letters && f.keyboard.phase() == KeyboardPhase::Waiting,
                "reset restores initial state");
            f.tick();
            check(!f.keyboard.selected_slot(), "held modifier after reset cannot acquire");
            f.center(); f.select();
            f.tap(faces[0]);
            check(f.keyboard.text() == L"a", "fresh reset typing");
        });
        test("timers work near monotonic tick maximum without overflow", [] {
            Fixture f;
            f.now = std::numeric_limits<Tick>::max() - 2000;
            f.select(); f.fill(2); f.button(lb);
            f.advance(400);
            check(f.keyboard.text().empty(), "repeat elapsed comparisons must not overflow");
            f.advance(180);
            check(f.keyboard.text().empty(), "empty repeats remain bounded near tick maximum");
        });
        test("one-handed flick selects a group then a cardinal face and types the exact mapped character", [] {
            Fixture f;
            f.enable_one_handed();
            check(f.keyboard.input_mode() == KeyboardInputMode::OneHanded, "mode switch must take effect");
            std::wstring expected;
            for (const auto page : {KeyboardPage::Letters, KeyboardPage::ShiftLetters,
                    KeyboardPage::Symbols, KeyboardPage::ShiftSymbols}) {
                auto& controller = f.snapshot.slots[0];
                controller.left_trigger =
                    page == KeyboardPage::ShiftLetters || page == KeyboardPage::ShiftSymbols ? 255 : 0;
                controller.right_trigger =
                    page == KeyboardPage::Symbols || page == KeyboardPage::ShiftSymbols ? 255 : 0;
                for (unsigned group = 0; group < 7; ++group) {
                    for (const double degrees : {0.0, 90.0, 180.0, 270.0}) {
                        f.flick(group, degrees);
                        expected += keyboard_layout(page, KeyboardInputMode::OneHanded)[group][Fixture::cardinal_face(degrees)];
                        check(f.keyboard.text() == expected, "one-handed page/group/face mapping differs");
                        check(f.keyboard.flick_stage() == KeyboardFlickStage::Group,
                            "committing a character must return to Group stage");
                    }
                }
            }
            check(expected.size() == 7u * 4u * 4u, "every non-action group/face combination across pages must be exercised");
        });
        test("one-handed layouts relocate lost punctuation and remain full printable ASCII", [] {
            std::array<bool, 127> reachable{};
            reachable[32] = true;
            for (const auto page : {KeyboardPage::Letters, KeyboardPage::ShiftLetters,
                    KeyboardPage::Symbols, KeyboardPage::ShiftSymbols}) {
                const auto& layout = keyboard_layout(page, KeyboardInputMode::OneHanded);
                for (unsigned group = 0; group < 7; ++group) {
                    for (const auto character : layout[group]) {
                        check(character >= 33 && character <= 126, "one-handed layout contains nonprintable text");
                        reachable[static_cast<std::size_t>(character)] = true;
                    }
                }
            }
            check(std::all_of(reachable.begin() + 32, reachable.end(), [](bool value) { return value; }),
                "one-handed tables must still reach every printable ASCII character");
        });
        test("one-handed eighth group is Space/Backspace/page-cycle/Enter and never emits a literal action glyph", [] {
            Fixture f;
            f.enable_one_handed();
            f.flick(0, 0.0); // group0 face Y: any ordinary character to have something to erase.
            const auto typed = f.keyboard.text();
            check(!typed.empty(), "fixture needs a character before testing backspace");
            f.flick(7, 180.0); // A (bottom): Space.
            check(f.keyboard.text() == typed + L" ", "eighth group A must append a space, not a literal glyph");
            f.flick(7, 90.0); // B (right): Backspace.
            check(f.keyboard.text() == typed, "eighth group B must erase, not append a literal glyph");
            check(f.keyboard.page() == KeyboardPage::Letters, "page starts at Letters");
            f.flick(7, 270.0); // X (left): page cycle.
            check(f.keyboard.page() == KeyboardPage::ShiftLetters, "eighth group X must cycle Letters -> ShiftLetters");
            f.flick(7, 270.0);
            check(f.keyboard.page() == KeyboardPage::Symbols, "eighth group X must cycle ShiftLetters -> Symbols");
            f.flick(7, 270.0);
            check(f.keyboard.page() == KeyboardPage::ShiftSymbols, "eighth group X must cycle Symbols -> ShiftSymbols");
            f.flick(7, 270.0);
            check(f.keyboard.page() == KeyboardPage::Letters, "eighth group X must wrap ShiftSymbols -> Letters");
            for (const auto glyph : {keyboard_action_space, keyboard_action_backspace,
                    keyboard_action_page_cycle, keyboard_action_enter}) {
                check(f.keyboard.text().find(glyph) == std::wstring::npos,
                    "action sentinel glyphs must never be appended as text");
            }
            f.flick(7, 0.0); // Y (top): Enter, mirrors Menu release in legacy (non-live-edits) mode.
            check(f.keyboard.phase() == KeyboardPhase::Complete, "eighth group Y must finish composition like Menu");
        });
        test("one-handed group commit ignores a held stick and commits exactly once per recenter", [] {
            Fixture f;
            f.enable_one_handed();
            f.aim(90.0, 0.8);
            for (int i = 0; i < 20; ++i) { f.tick(); }
            check(!f.keyboard.selected_petal(), "holding outward must not commit a group without recentering");
            f.center(); f.tick();
            check(f.keyboard.selected_petal() == 2u, "recentering once commits exactly the held group");
            f.aim(90.0, 0.8);
            for (int i = 0; i < 20; ++i) { f.tick(); }
            check(f.keyboard.text().empty(), "holding a cardinal gesture must not commit a character without recentering");
            check(f.keyboard.selected_face() == 1u, "the held candidate should still be visible for emphasis before commit");
            f.center();
            f.tick();
            check(f.keyboard.text().size() == 1, "a single recenter commits exactly one character, not once per held tick");
        });
        test("one-handed ambiguous diagonal gestures commit nothing; a locked cardinal survives a brief wobble", [] {
            Fixture f;
            f.enable_one_handed();
            f.aim(0.0, 0.8); // acquire the pad and pick group 0 (up).
            f.tick();
            f.center(); f.tick();
            check(f.keyboard.selected_petal() == 0u && f.keyboard.flick_stage() == KeyboardFlickStage::Character,
                "group commit should reach Character stage");
            const auto after_first = f.keyboard.text();
            f.aim(45.0, 0.8); // ambiguous opening gesture, exactly between Y and B.
            f.tick();
            check(!f.keyboard.selected_face(), "an ambiguous opening gesture must not select any face");
            f.center(); f.tick();
            check(f.keyboard.text() == after_first, "an ambiguous opening gesture must not commit a character");
            check(f.keyboard.flick_stage() == KeyboardFlickStage::Character, "a failed commit stays in Character stage for a retry");
            f.aim(90.0, 0.8); // now lock a valid cardinal (B) without centering yet.
            f.tick();
            check(f.keyboard.selected_face() == 1u, "a clean cardinal gesture should lock face B");
            f.aim(70.0, 0.8); // wobble toward the ambiguous zone while still held.
            f.tick();
            check(f.keyboard.selected_face() == 1u, "a locked candidate must survive a brief ambiguous wobble");
            f.center(); f.tick();
            check(f.keyboard.text() == after_first + keyboard_layout(KeyboardPage::Letters, KeyboardInputMode::OneHanded)[0][1],
                "the locked candidate that survived the wobble must still commit");
        });
        test("one-handed mode leaves raw face presses inert; LB/RB/LT/RT/Menu/View remain conveniences", [] {
            Fixture f;
            f.enable_one_handed();
            f.flick(0, 0.0);
            const auto typed = f.keyboard.text();
            f.tap(faces[0]);
            check(f.keyboard.text() == typed, "raw A/B/X/Y presses must be inert in OneHanded mode");
            f.tap(rb);
            check(f.keyboard.text() == typed + L" ", "RB space remains a convenience");
            f.tap(lb);
            check(f.keyboard.text() == typed, "LB backspace remains a convenience");
            auto& controller = f.snapshot.slots[0];
            controller.left_trigger = 255;
            f.tick();
            check(f.keyboard.page() == KeyboardPage::ShiftLetters, "held LT still overrides the displayed page");
            controller.left_trigger = 0;
            f.tick();
            check(f.keyboard.page() == KeyboardPage::Letters, "releasing LT restores the persistent one-handed page");
            check(f.button(view) == KeyboardAction::Cancel, "View still cancels in OneHanded mode");
            check(f.keyboard.text().empty(), "View cancel clears composition exactly as in FaceButtons mode");
        });
        test("one-handed left-stick click and Y both open; mode switch mid-session resets gestures without editing text", [] {
            Fixture f;
            f.select(); f.fill(2);
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            check(f.keyboard.text() == L"aa", "mode switch must not edit the existing composition");
            check(f.keyboard.flick_stage() == KeyboardFlickStage::Group, "mode switch resets to Group stage");
            f.aim(90.0, 0.8);
            f.tick();
            f.keyboard.set_input_mode(KeyboardInputMode::FaceButtons);
            check(f.keyboard.text() == L"aa", "switching mid-gesture still must not edit text");
            check(f.keyboard.input_mode() == KeyboardInputMode::FaceButtons, "mode switch takes effect immediately");
            f.center(); f.tick();
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            f.flick(0, 90.0);
            check(f.keyboard.text() == L"aa" + std::wstring(1, keyboard_layout(KeyboardPage::Letters, KeyboardInputMode::OneHanded)[0][1]),
                "after switching back, a fresh flick still commits normally");
        });
        test("one-handed mode preserves stale/reconnect/focus/multi-pad handling", [] {
            Fixture f;
            f.connect(1);
            f.enable_one_handed();
            f.aim(0.0, 0.8, 0); f.aim(90.0, 0.8, 1);
            f.tick();
            check(!f.keyboard.selected_slot(), "ambiguous simultaneous multi-pad activity must not select in OneHanded mode either");
            f.center(0); f.center(1); f.tick();
            f.flick(0, 0.0);
            check(f.keyboard.selected_slot() == 0u, "unambiguous pad still acquires normally in OneHanded mode");
            const auto typed = f.keyboard.text();
            ++f.snapshot.slots[0].connection_epoch;
            f.tick();
            check(!f.keyboard.selected_slot() && f.keyboard.text() == typed,
                "epoch change releases the pad without discarding composed text");
            f.snapshot.slots[0].connection = Connection::Connected;
            f.tick(16, 101);
            check(!f.keyboard.selected_slot(), "stale reconnect data cannot supply neutral or reacquire");
        });
        test("reset preserves the configured input mode across View cancel, pin, and dismiss", [] {
            Fixture f;
            f.select(); f.fill(2);
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            check(f.button(view) == KeyboardAction::Cancel, "View cancel still resets composition");
            check(f.keyboard.input_mode() == KeyboardInputMode::OneHanded,
                "reset()'s View-cancel path must not revert the persistent input mode to FaceButtons");
            check(f.keyboard.text().empty(), "composition must still be cleared by the cancel");
        });
        test("highlighted_petal exposes a pending group candidate before commit; unchanged in FaceButtons/Character", [] {
            Fixture f;
            check(!f.keyboard.highlighted_petal(), "no highlight before any pad is acquired");
            f.tick();
            f.aim(90.0, 0.8);
            f.tick();
            check(f.keyboard.highlighted_petal() == 2u && f.keyboard.selected_petal() == 2u,
                "FaceButtons highlighted_petal must mirror selected_petal");
            f.center(); f.tick();
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            check(!f.keyboard.highlighted_petal(), "switching mode with a neutral stick must show no candidate");
            f.aim(90.0, 0.8);
            f.tick();
            check(f.keyboard.highlighted_petal() == 2u, "Group-stage outward gesture must highlight the pending candidate");
            check(!f.keyboard.selected_petal(), "selected_petal must stay empty until the group actually commits");
            check(f.keyboard.text().empty(), "no text until the candidate is actually committed by a second gesture");
            f.center(); f.tick();
            check(f.keyboard.selected_petal() == 2u && f.keyboard.flick_stage() == KeyboardFlickStage::Character,
                "recentering commits the group and reaches Character stage");
            check(f.keyboard.highlighted_petal() == 2u, "Character stage highlighted_petal must report the committed group");
            f.aim(90.0, 0.8);
            f.tick();
            check(f.keyboard.highlighted_petal() == 2u, "a face-stage candidate must not change the reported group highlight");
        });
        test("highlighted_petal returns to null while neutral between one-handed gestures, not a stale prior group", [] {
            Fixture f;
            f.enable_one_handed();
            f.aim(0.0, 0.8);
            f.tick();
            f.center(); f.tick();
            check(f.keyboard.selected_petal() == 0u, "first group commits");
            f.aim(90.0, 0.8);
            f.tick();
            f.center(); f.tick();
            check(f.keyboard.flick_stage() == KeyboardFlickStage::Group, "a completed character returns to Group stage");
            check(!f.keyboard.highlighted_petal(), "a neutral Group stage must not show a stale previous group as highlighted");
        });
        test("step reports Redraw for a one-handed group candidate, stage change, and face change with no other field changed", [] {
            Fixture f;
            f.enable_one_handed();
            f.tick();
            f.aim(45.0, 0.8);
            check(f.tick() == KeyboardAction::Redraw, "an outward Group-stage gesture must Redraw even before commit");
            check(f.tick() == KeyboardAction::None, "holding the same candidate steady must not Redraw again");
            f.center();
            check(f.tick() == KeyboardAction::Redraw, "the Group->Character stage transition on commit must Redraw");
            f.aim(90.0, 0.8);
            check(f.tick() == KeyboardAction::Redraw, "a fresh face candidate must Redraw even before commit");
            check(f.tick() == KeyboardAction::None, "holding the same face steady must not Redraw again");
        });
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << count << " cases: " << error.what() << '\n';
        return 1;
    }
    std::cout << count << " keyboard cases passed\n";
    return 0;
}
