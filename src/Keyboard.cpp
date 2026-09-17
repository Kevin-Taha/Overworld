#include "Keyboard.h"

#include <bit>
#include <cmath>
#include <numbers>

namespace gamepad {
namespace {
constexpr std::uint16_t menu_button = 0x0010;
constexpr std::uint16_t view_button = 0x0020;
constexpr std::uint16_t backspace_button = 0x0100;
constexpr std::uint16_t space_button = 0x0200;
constexpr std::uint16_t face_buttons = 0xf000;
constexpr std::uint16_t keyboard_buttons =
    menu_button | view_button | backspace_button | space_button | face_buttons;
constexpr Tick freshness_ms = 100;
constexpr Tick backspace_delay_ms = 400;
constexpr Tick backspace_repeat_ms = 90;
constexpr double select_radius = 0.45;
constexpr double neutral_radius = 0.25;
constexpr double sector_angle = std::numbers::pi / 4.0;
constexpr double angular_margin = 5.0 * std::numbers::pi / 180.0;

constexpr KeyboardPetals letters{{
    {{L'a', L'b', L'c', L'd'}}, {{L'e', L'f', L'g', L'h'}},
    {{L'i', L'j', L'k', L'l'}}, {{L'm', L'n', L'o', L'p'}},
    {{L'q', L'r', L's', L't'}}, {{L'u', L'v', L'w', L'x'}},
    {{L'y', L'z', L'.', L','}}, {{L'-', L'\'', L'/', L'?'}}
}};
constexpr KeyboardPetals shift_letters{{
    {{L'A', L'B', L'C', L'D'}}, {{L'E', L'F', L'G', L'H'}},
    {{L'I', L'J', L'K', L'L'}}, {{L'M', L'N', L'O', L'P'}},
    {{L'Q', L'R', L'S', L'T'}}, {{L'U', L'V', L'W', L'X'}},
    {{L'Y', L'Z', L'.', L','}}, {{L'-', L'\'', L'/', L'?'}}
}};
constexpr KeyboardPetals symbols{{
    {{L'1', L'2', L'3', L'4'}}, {{L'5', L'6', L'7', L'8'}},
    {{L'9', L'0', L'.', L':'}}, {{L'/', L'\\', L'_', L'-'}},
    {{L'@', L'#', L'&', L'!'}}, {{L'=', L'+', L'%', L'$'}},
    {{L'(', L')', L'[', L']'}}, {{L'{', L'}', L',', L';'}}
}};
constexpr KeyboardPetals shift_symbols{{
    {{L'!', L'@', L'#', L'$'}}, {{L'%', L'^', L'&', L'*'}},
    {{L'(', L')', L'<', L'>'}}, {{L'/', L'\\', L'|', L'~'}},
    {{L'"', L'\'', L'`', L'?'}}, {{L'=', L'+', L'_', L'-'}},
    {{L'{', L'}', L'[', L']'}}, {{L':', L';', L'.', L','}}
}};

// OneHanded reassigns the eighth (last) group to stick-only actions (never appended as text; see
// commit_one_handed_face). The removed group-7 punctuation is fully preserved elsewhere except ';',
// which is relocated into Letters/ShiftLetters group 6 slot index 2 (displacing '.', still reachable
// via Symbols group 2).
constexpr std::array<wchar_t, 4> one_handed_actions{
    keyboard_action_space, keyboard_action_backspace, keyboard_action_page_cycle, keyboard_action_enter
};
constexpr KeyboardPetals one_handed_letters{{
    {{L'a', L'b', L'c', L'd'}}, {{L'e', L'f', L'g', L'h'}},
    {{L'i', L'j', L'k', L'l'}}, {{L'm', L'n', L'o', L'p'}},
    {{L'q', L'r', L's', L't'}}, {{L'u', L'v', L'w', L'x'}},
    {{L'y', L'z', L';', L','}}, one_handed_actions
}};
constexpr KeyboardPetals one_handed_shift_letters{{
    {{L'A', L'B', L'C', L'D'}}, {{L'E', L'F', L'G', L'H'}},
    {{L'I', L'J', L'K', L'L'}}, {{L'M', L'N', L'O', L'P'}},
    {{L'Q', L'R', L'S', L'T'}}, {{L'U', L'V', L'W', L'X'}},
    {{L'Y', L'Z', L';', L','}}, one_handed_actions
}};
constexpr KeyboardPetals one_handed_symbols{{
    {{L'1', L'2', L'3', L'4'}}, {{L'5', L'6', L'7', L'8'}},
    {{L'9', L'0', L'.', L':'}}, {{L'/', L'\\', L'_', L'-'}},
    {{L'@', L'#', L'&', L'!'}}, {{L'=', L'+', L'%', L'$'}},
    {{L'(', L')', L'[', L']'}}, one_handed_actions
}};
constexpr KeyboardPetals one_handed_shift_symbols{{
    {{L'!', L'@', L'#', L'$'}}, {{L'%', L'^', L'&', L'*'}},
    {{L'(', L')', L'<', L'>'}}, {{L'/', L'\\', L'|', L'~'}},
    {{L'"', L'\'', L'`', L'?'}}, {{L'=', L'+', L'_', L'-'}},
    {{L'{', L'}', L'[', L']'}}, one_handed_actions
}};

double axis(std::int16_t value)
{
    return static_cast<double>(value) / (value < 0 ? 32768.0 : 32767.0);
}

double radius(std::int16_t x, std::int16_t y)
{
    return std::hypot(axis(x), axis(y));
}

bool deliberately_active(const Controller& controller)
{
    return (controller.buttons & keyboard_buttons) != 0
        || controller.left_trigger > 128 || controller.right_trigger > 128
        || radius(controller.left_x, controller.left_y) >= select_radius;
}

KeyboardPage controller_page(const Controller& controller)
{
    if (controller.right_trigger > 128) {
        return controller.left_trigger > 128 ? KeyboardPage::ShiftSymbols : KeyboardPage::Symbols;
    }
    return controller.left_trigger > 128 ? KeyboardPage::ShiftLetters : KeyboardPage::Letters;
}
} // namespace

bool keyboard_neutral(const Controller& controller)
{
    return (controller.buttons & standard_buttons) == 0
        && controller.left_trigger <= 30 && controller.right_trigger <= 30
        && radius(controller.left_x, controller.left_y) <= neutral_radius
        && radius(controller.right_x, controller.right_y) <= neutral_radius;
}

const KeyboardPetals& keyboard_layout(KeyboardPage page, KeyboardInputMode mode)
{
    if (mode == KeyboardInputMode::OneHanded) {
        switch (page) {
        case KeyboardPage::Letters: return one_handed_letters;
        case KeyboardPage::ShiftLetters: return one_handed_shift_letters;
        case KeyboardPage::Symbols: return one_handed_symbols;
        case KeyboardPage::ShiftSymbols: return one_handed_shift_symbols;
        }
        return one_handed_letters;
    }
    switch (page) {
    case KeyboardPage::Letters: return letters;
    case KeyboardPage::ShiftLetters: return shift_letters;
    case KeyboardPage::Symbols: return symbols;
    case KeyboardPage::ShiftSymbols: return shift_symbols;
    }
    return letters;
}

void Daisywheel::reset()
{
    // input_mode_ is a persistent configuration choice, not transient gesture state, and must
    // survive reset() (used by View-cancel, dismiss, and pin's re-arm) so LS-open/OneHanded
    // selection keeps working across cancel/reopen cycles.
    const auto mode = input_mode_;
    *this = Daisywheel{};
    input_mode_ = mode;
}

bool Daisywheel::pin(unsigned slot, std::uint64_t epoch)
{
    if (slot >= slot_count) { return false; }
    reset();
    pinned_ = slot_ = slot;
    pads_[slot].epoch = epoch;
    live_edits_ = opening_ = true;
    previous_buttons_ = 0x8000;
    return true;
}

std::optional<KeyboardEdit> Daisywheel::take_edit()
{
    if (!edit_count_) { return {}; }
    const auto edit = edits_[0];
    if (--edit_count_) { edits_[0] = edits_[1]; }
    edits_[edit_count_] = {};
    return edit;
}

void Daisywheel::require_neutral(std::wstring_view reason)
{
    for (auto& pad : pads_) { pad.neutral_seen = false; }
    armed_ = false;
    stick_engaged_ = false;
    menu_pending_ = false;
    previous_buttons_ = 0;
    backspace_at_.reset();
    backspace_repeating_ = false;
    petal_.reset();
    page_ = KeyboardPage::Letters;
    flick_stage_ = KeyboardFlickStage::Group;
    oh_group_candidate_.reset();
    oh_group_engaged_ = false;
    selected_face_.reset();
    oh_face_engaged_ = false;
    one_handed_page_ = KeyboardPage::Letters;
    if (phase_ != KeyboardPhase::Complete) { phase_ = KeyboardPhase::Waiting; }
    status_ = reason;
}

void Daisywheel::set_input_mode(KeyboardInputMode mode)
{
    if (mode == input_mode_) { return; }
    input_mode_ = mode;
    // Switching modes never emits a keystroke; only the in-progress gesture/petal is discarded.
    petal_.reset();
    stick_engaged_ = false;
    flick_stage_ = KeyboardFlickStage::Group;
    oh_group_candidate_.reset();
    oh_group_engaged_ = false;
    selected_face_.reset();
    oh_face_engaged_ = false;
    one_handed_page_ = KeyboardPage::Letters;
    update_status();
}

void Daisywheel::suspend()
{
    require_neutral(phase_ == KeyboardPhase::Complete
        ? L"Complete - center controller to continue"
        : L"Center controller to resume; composition retained");
}

void Daisywheel::update_petal(const Controller& controller)
{
    const double magnitude = radius(controller.left_x, controller.left_y);
    if (magnitude <= neutral_radius) {
        stick_engaged_ = false;
        return;
    }
    const bool entering = !stick_engaged_;
    if (entering && magnitude < select_radius) { return; }
    stick_engaged_ = true;
    double angle = std::atan2(axis(controller.left_x), axis(controller.left_y));
    if (angle < 0.0) { angle += 2.0 * std::numbers::pi; }
    const auto candidate = static_cast<unsigned>((angle + sector_angle / 2.0) / sector_angle) % 8;
    if (!petal_ || entering
        || std::abs(std::remainder(angle - *petal_ * sector_angle, 2.0 * std::numbers::pi))
            > sector_angle / 2.0 + angular_margin) {
        petal_ = candidate;
    }
}

KeyboardAction Daisywheel::update_one_handed(const Controller& controller, bool suppress_commit)
{
    const double magnitude = radius(controller.left_x, controller.left_y);
    const bool neutral = magnitude <= neutral_radius;

    if (flick_stage_ == KeyboardFlickStage::Group) {
        if (neutral) {
            if (oh_group_engaged_ && oh_group_candidate_) {
                petal_ = oh_group_candidate_;
                flick_stage_ = KeyboardFlickStage::Character;
            }
            oh_group_engaged_ = false;
            oh_group_candidate_.reset();
            return KeyboardAction::None;
        }
        const bool entering = !oh_group_engaged_;
        if (entering && magnitude < select_radius) { return KeyboardAction::None; }
        oh_group_engaged_ = true;
        double angle = std::atan2(axis(controller.left_x), axis(controller.left_y));
        if (angle < 0.0) { angle += 2.0 * std::numbers::pi; }
        const auto candidate = static_cast<unsigned>((angle + sector_angle / 2.0) / sector_angle) % 8;
        if (!oh_group_candidate_ || entering
            || std::abs(std::remainder(angle - *oh_group_candidate_ * sector_angle, 2.0 * std::numbers::pi))
                > sector_angle / 2.0 + angular_margin) {
            oh_group_candidate_ = candidate;
        }
        return KeyboardAction::None;
    }

    // Character stage: classify against the 4 cardinal directions only (Y up, B right, A down, X left).
    if (neutral) {
        if (oh_face_engaged_ && selected_face_) {
            const auto face = *selected_face_;
            oh_face_engaged_ = false;
            selected_face_.reset();
            flick_stage_ = KeyboardFlickStage::Group;
            // A same-sample physical LB/RB editing shortcut outranks a completing flick: the
            // candidate is still fully consumed (stage returns to Group) so it cannot replay.
            if (suppress_commit) { return KeyboardAction::None; }
            return commit_one_handed_face(face, controller);
        }
        oh_face_engaged_ = false;
        selected_face_.reset();
        return KeyboardAction::None;
    }
    const bool entering = !oh_face_engaged_;
    if (entering && magnitude < select_radius) { return KeyboardAction::None; }
    oh_face_engaged_ = true;
    double angle = std::atan2(axis(controller.left_x), axis(controller.left_y));
    if (angle < 0.0) { angle += 2.0 * std::numbers::pi; }
    constexpr double cardinal_margin = 30.0 * std::numbers::pi / 180.0;
    constexpr std::array<unsigned, 4> cardinal_face{3, 1, 0, 2}; // axis 0..3 -> Y, B, A, X.
    const auto axis_index = static_cast<unsigned>(std::floor(angle / (std::numbers::pi / 2.0) + 0.5)) % 4;
    const double nearest_axis = axis_index * (std::numbers::pi / 2.0);
    const double offset = std::abs(std::remainder(angle - nearest_axis, 2.0 * std::numbers::pi));
    if (offset <= cardinal_margin) {
        selected_face_ = cardinal_face[axis_index];
    } else if (entering) {
        // Ambiguous opening gesture: no candidate. An already-locked candidate survives a brief wobble.
        selected_face_.reset();
    }
    return KeyboardAction::None;
}

KeyboardAction Daisywheel::commit_one_handed_face(unsigned face, const Controller& controller)
{
    if (petal_ == 7u) {
        switch (face) {
        case 0: append(L' '); break;
        case 1: erase(); break;
        case 2:
            one_handed_page_ = static_cast<KeyboardPage>((static_cast<int>(one_handed_page_) + 1) % 4);
            update_status();
            break;
        case 3: return enter_activated(controller);
        default: break;
        }
        return KeyboardAction::None;
    }
    if (petal_) { append(keyboard_layout(page_, input_mode_)[*petal_][face]); }
    return KeyboardAction::None;
}

KeyboardAction Daisywheel::enter_activated(const Controller& controller)
{
    if (live_edits_) {
        edits_.at(edit_count_++) = KeyboardEdit{KeyboardEditKind::Enter};
        return KeyboardAction::None;
    }
    if (phase_ == KeyboardPhase::Complete) {
        text_.clear();
        petal_.reset();
        stick_engaged_ = false;
        page_ = controller_page(controller);
        phase_ = KeyboardPhase::Typing;
        update_status();
        return KeyboardAction::None;
    }
    phase_ = KeyboardPhase::Complete;
    update_status();
    return KeyboardAction::Done;
}

void Daisywheel::update_status()
{
    if (live_edits_) {
        status_ = input_mode_ == KeyboardInputMode::OneHanded
            ? L"Flick out then center: pick a group, then A/B/X/Y; View closes"
            : L"Typing - Menu Enter; View closes";
    } else if (phase_ == KeyboardPhase::Complete) {
        status_ = text_.empty() ? L"Complete (empty) - Menu starts new text; View closes"
                              : L"Complete - preview only; Menu starts new text; View closes";
    } else if (at_limit()) {
        status_ = L"256-character limit; LB erases, Menu finishes";
    } else if (input_mode_ == KeyboardInputMode::OneHanded) {
        status_ = flick_stage_ == KeyboardFlickStage::Character
            ? L"Flick A/B/X/Y then center to pick a character"
            : L"Flick the stick out then center to choose a group";
    } else {
        status_ = petal_ ? L"A/B/X/Y type; RB space; Menu finishes; View cancels"
                        : L"Move the left stick to choose a petal; RB adds space";
    }
}

void Daisywheel::append(wchar_t character)
{
    if (live_edits_) { edits_.at(edit_count_++) = KeyboardEdit{KeyboardEditKind::Character, static_cast<char32_t>(character)}; }
    else if (!at_limit()) { text_.push_back(character); }
    update_status();
}

void Daisywheel::erase()
{
    if (live_edits_) {
        edits_.at(edit_count_++) = KeyboardEdit{KeyboardEditKind::Backspace};
        return;
    }
    if (text_.empty()) {
        status_ = L"Nothing to erase";
    } else {
        text_.pop_back();
        update_status();
    }
}

KeyboardAction Daisywheel::step(const Snapshot& snapshot, Tick now)
{
    edits_ = {};
    edit_count_ = 0;
    const auto old_size = text_.size();
    const auto old_petal = petal_;
    const auto old_highlighted_petal = highlighted_petal();
    const auto old_slot = slot_;
    const auto old_page = page_;
    const auto old_phase = phase_;
    const auto old_status = status_;
    const auto old_flick_stage = flick_stage_;
    const auto old_selected_face = selected_face_;
    const auto result = [&] {
        return edit_count_ != 0 || old_size != text_.size() || old_petal != petal_ || old_slot != slot_
            || old_page != page_ || old_phase != phase_ || old_status != status_
            || old_flick_stage != flick_stage_ || old_selected_face != selected_face_
            || old_highlighted_petal != highlighted_petal()
            ? KeyboardAction::Redraw : KeyboardAction::None;
    };

    if (snapshot.sequence == 0 || snapshot.sampled_at > now
        || now - snapshot.sampled_at > freshness_ms
        || (last_now_ && (now < *last_now_ || snapshot.sequence < last_sequence_
            || snapshot.sampled_at < last_sample_time_
            || (snapshot.sequence == last_sequence_ && snapshot.sampled_at != last_sample_time_)))) {
        require_neutral(L"Waiting for fresh controller data; composition retained");
        return result();
    }

    // A new sample after a consumer/reader gap is not an observed button edge.
    const bool gap = last_now_ && (now - *last_now_ > freshness_ms
        || snapshot.sampled_at - last_sample_time_ > freshness_ms);
    last_now_ = now;
    if (gap) { require_neutral(L"Center controller after input gap; composition retained"); }
    if (snapshot.sequence == last_sequence_) { return result(); }
    last_sequence_ = snapshot.sequence;
    last_sample_time_ = snapshot.sampled_at;

    if (slot_) {
        const auto& controller = snapshot.slots[*slot_];
        auto& history = pads_[*slot_];
        if (controller.connection != Connection::Connected
            || history.epoch != controller.connection_epoch) {
            slot_.reset();
            pads_ = {};
            require_neutral(L"Controller changed; center a controller to resume");
            return result();
        }
        if (!armed_) {
            if (live_edits_ && opening_) {
                if (controller.buttons & 0x8000) {
                    previous_buttons_ = static_cast<std::uint16_t>(controller.buttons & keyboard_buttons);
                    return result();
                }
                // Y is the opening gesture, not a character. Sticks and pages may
                // already be positioned when it releases; new face edges are usable.
                opening_ = false;
                armed_ = true;
                phase_ = KeyboardPhase::Typing;
                update_status();
            } else if (keyboard_neutral(controller)) {
                armed_ = true;
                history.neutral_seen = true;
                if (phase_ != KeyboardPhase::Complete) { phase_ = KeyboardPhase::Typing; }
                update_status();
            }
            if (!live_edits_ || !armed_) { return result(); }
        }
    } else {
        if (pinned_) {
            status_ = L"Invoking controller disconnected; View closes";
            return result();
        }
        std::optional<unsigned> candidate;
        unsigned candidates = 0;
        for (unsigned index = 0; index < slot_count; ++index) {
            const auto& controller = snapshot.slots[index];
            auto& history = pads_[index];
            if (controller.connection != Connection::Connected) {
                history = {};
                continue;
            }
            if (history.epoch != controller.connection_epoch) {
                history = {controller.connection_epoch, false};
            }
            if (keyboard_neutral(controller)) {
                history.neutral_seen = true;
            } else if (history.neutral_seen && deliberately_active(controller)) {
                candidate = index;
                ++candidates;
            }
        }
        if (candidates > 1) {
            // Do not choose the last held member when an ambiguous gesture unwinds.
            for (unsigned index = 0; index < slot_count; ++index) {
                if (deliberately_active(snapshot.slots[index])) { pads_[index].neutral_seen = false; }
            }
            status_ = L"Multiple controllers active; center them, then use only one";
            return result();
        }
        if (!candidate) { return result(); }
        slot_ = candidate;
        armed_ = true;
        if (phase_ != KeyboardPhase::Complete) { phase_ = KeyboardPhase::Typing; }
        update_status();
    }

    const auto& controller = snapshot.slots[*slot_];
    const auto buttons = static_cast<std::uint16_t>(controller.buttons & keyboard_buttons);
    const auto pressed = static_cast<std::uint16_t>(buttons & ~previous_buttons_);
    previous_buttons_ = buttons;

    if ((pressed & view_button) != 0) {
        reset();
        return KeyboardAction::Cancel;
    }
    if ((pressed & menu_button) != 0) {
        menu_pending_ = true;
        backspace_at_.reset();
    }
    if (menu_pending_ && (buttons & menu_button) == 0) {
        menu_pending_ = false;
        const bool was_complete = phase_ == KeyboardPhase::Complete;
        // Enter precedes the ordinary edit edge observed in this same sample.
        if (const auto action = enter_activated(controller); action == KeyboardAction::Done) {
            return action;
        } else if (!live_edits_ && was_complete) {
            return result();
        }
    }
    if (phase_ == KeyboardPhase::Complete || (buttons & menu_button) != 0) { return result(); }

    const auto previous_petal = petal_;
    const auto previous_page = page_;
    KeyboardAction one_handed_result = KeyboardAction::None;
    if (input_mode_ == KeyboardInputMode::OneHanded) {
        // Priority: View/mode cancel (handled above) > Menu Enter (handled above) > physical
        // LB/RB editing shortcut > flick commit. Compute whether LB or RB will actually fire an
        // edit this sample (matching the exact predicates used below) so a same-sample completing
        // flick can be suppressed rather than overflow the bounded 2-edit buffer.
        const bool backspace_would_fire = (pressed & backspace_button) != 0
            || ((buttons & backspace_button) != 0 && backspace_at_
                && now - *backspace_at_ >= (backspace_repeating_ ? backspace_repeat_ms : backspace_delay_ms));
        const bool space_would_fire = (pressed & space_button) != 0;
        one_handed_result = update_one_handed(controller, backspace_would_fire || space_would_fire);
        page_ = (controller.left_trigger > 128 || controller.right_trigger > 128)
            ? controller_page(controller) : one_handed_page_;
    } else {
        update_petal(controller);
        page_ = controller_page(controller);
    }
    if (previous_petal != petal_ || previous_page != page_) { update_status(); }
    if (one_handed_result == KeyboardAction::Done) { return one_handed_result; }

    if ((buttons & backspace_button) == 0) {
        backspace_at_.reset();
        backspace_repeating_ = false;
    } else if ((pressed & backspace_button) != 0) {
        erase();
        backspace_at_ = now;
        backspace_repeating_ = false;
        return result();
    } else if (backspace_at_ && now - *backspace_at_
        >= (backspace_repeating_ ? backspace_repeat_ms : backspace_delay_ms)) {
        erase();
        backspace_at_ = now; // At most one repeat per fresh sample, never a catch-up burst.
        backspace_repeating_ = true;
        return result();
    }

    if ((pressed & space_button) != 0) {
        append(L' ');
    } else if (input_mode_ == KeyboardInputMode::FaceButtons) {
        // OneHanded selects characters via the two-stage flick gesture; raw face presses are inert.
        const auto faces = static_cast<std::uint16_t>(pressed & face_buttons);
        if (std::popcount(faces) > 1) {
            status_ = L"Press one face button at a time";
        } else if (faces != 0 && petal_) {
            const unsigned character = static_cast<unsigned>(std::countr_zero(faces)) - 12;
            append(keyboard_layout(page_, input_mode_)[*petal_][character]);
        }
    }
    return result();
}

const std::wstring& Daisywheel::text() const { return text_; }
std::optional<unsigned> Daisywheel::selected_petal() const { return petal_; }
std::optional<unsigned> Daisywheel::highlighted_petal() const
{
    if (input_mode_ == KeyboardInputMode::OneHanded && flick_stage_ == KeyboardFlickStage::Group) {
        return oh_group_candidate_;
    }
    return petal_;
}
std::optional<unsigned> Daisywheel::selected_slot() const { return slot_; }
KeyboardPage Daisywheel::page() const { return page_; }
KeyboardPhase Daisywheel::phase() const { return phase_; }
std::wstring_view Daisywheel::status() const { return status_; }
bool Daisywheel::at_limit() const { return !live_edits_ && text_.size() >= keyboard_text_limit; }
KeyboardInputMode Daisywheel::input_mode() const { return input_mode_; }
KeyboardFlickStage Daisywheel::flick_stage() const { return flick_stage_; }
std::optional<unsigned> Daisywheel::selected_face() const { return selected_face_; }
} // namespace gamepad
