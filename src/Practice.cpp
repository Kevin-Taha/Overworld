#include "Practice.h"
#include "InputClassification.h"

#include <algorithm>
#include <cmath>

namespace gamepad::practice {
bool LocalKeys::observe(Model& model, unsigned key, bool down)
{
    const auto category = live::input_category(key);
    if (category == live::InputCategory::ReservedGamepad || category == live::InputCategory::ReservedNavigation) {
        return false;
    }
    if (key < held_.size()) { held_.set(key, down); }
    if (model.input() != InputState::Physical) { model.physical_takeover(); }
    return true;
}
namespace {
constexpr std::uint16_t button_a = 0x1000, button_b = 0x2000, button_x = 0x4000;
constexpr std::uint16_t button_y = 0x8000, button_menu = 0x0010, button_view = 0x0020;
constexpr live::Rect pointer_bounds{-scene_left + 15, -scene_top + 15,
    canvas_width - scene_left - 16, canvas_height - scene_top - 16};
constexpr std::array<LessonText, lesson_count> lessons{{
    {L"Find your aim", L"Move the left stick to visit all three targets.",
        L"Hold LB for precision. Small stick movements are slower; let go to stop."},
    {L"Two kinds of click", L"Point at the card. Tap A to select it, then X to see its options.",
        L"A is a primary click. X is a right click, not the B button. B dismisses the local menu."},
    {L"Pick up and place", L"Point at the small card. Hold A, move it into the outline, then release A.",
        L"Keep A held while moving. Releasing outside the outline lets you try again."},
    {L"Scroll to discover", L"Point inside the list. Push the right stick down to find the final card, then tap A.",
        L"Scrolling follows the pointer. Center the right stick to stop; push up to go back."},
    {L"Type without a keyboard", L"Press Y, center the controls, then point the left stick up and press A to type a.",
        L"Try more groups and face buttons. LB erases; RB adds a space. View closes the keyboard."}
}};
}

const LessonText& lesson_text(Lesson lesson) { return lessons.at(static_cast<unsigned>(lesson)); }
std::wstring_view input_text(InputState state)
{
    switch (state) {
    case InputState::Background: return L"Paused - select this window to practice.";
    case InputState::NoController: return L"Connect a controller, or use the mouse and keyboard.";
    case InputState::Neutral: return L"Center both sticks and release every button to begin.";
    case InputState::Centered: return L"Centered - move a stick or press a button to begin.";
    case InputState::Ready: return L"Controller ready - only this practice scene can change.";
    case InputState::Physical: return L"Mouse / keyboard in use. Release, then center the controller to resume.";
    case InputState::Multiple: return L"More than one controller moved. Center them, then use only one.";
    case InputState::Stale: return L"Input interrupted. Center the controller to resume safely.";
    case InputState::ReaderError: return L"Controller read failed. Reconnect it or use the local controls.";
    }
    return L"";
}
live::Point aim_target(unsigned index)
{
    constexpr std::array<live::Point, 3> points{{{165, 115}, {468, 258}, {330, 100}}};
    return points.at(std::min(index, 2u));
}
unsigned Model::completed_count() const
{
    return static_cast<unsigned>(std::count(completed_.begin(), completed_.end(), true));
}
live::Rect Model::card() const
{
    return {card_point_.x, card_point_.y, card_point_.x + 160, card_point_.y + 96};
}
void Model::suspend(InputState reason)
{
    input_ = reason;
    armed_ = false;
    slot_.reset();
    neutral_.fill(false);
    previous_buttons_ = 0;
    primary_down_ = dragging_ = menu_pending_ = false;
    click_.reset();
    keyboard_.suspend();
    ++revision_;
}
void Model::discard_samples_through(Tick now)
{
    discarded_through_ = std::max(discarded_through_.value_or(0), now);
}
void Model::focus(bool focused, Tick now)
{
    if (focused_ == focused) { return; }
    discard_samples_through(now);
    focused_ = focused;
    local_hold_ = false;
    suspend(focused ? InputState::Neutral : InputState::Background);
}
void Model::physical_takeover() { suspend(focused_ ? InputState::Physical : InputState::Background); }
void Model::cancel_local() { local_hold_ = false; physical_takeover(); }
void Model::reset_scene()
{
    targets_ = 0;
    point_ = {70, 300};
    card_point_ = {65, 148};
    scroll_ = 0;
    primary_clicked_ = context_open_ = keyboard_open_ = local_hold_ = false;
    help_ = false;
    keyboard_.reset();
    feedback_ = lesson_text(lesson_).instruction;
    suspend(focused_ ? InputState::Neutral : InputState::Background);
}
void Model::select(Lesson lesson)
{
    if (static_cast<unsigned>(lesson) >= lesson_count) { return; }
    lesson_ = lesson;
    reset_scene();
}
void Model::reset()
{
    completed_.fill(false);
    exit_requested_ = false;
    select(Lesson::Aim);
}
void Model::retry()
{
    completed_[static_cast<unsigned>(lesson_)] = false;
    reset_scene();
}
void Model::next()
{
    select(static_cast<Lesson>((static_cast<unsigned>(lesson_) + 1) % lesson_count));
}
void Model::finish(std::wstring_view feedback)
{
    completed_[static_cast<unsigned>(lesson_)] = true;
    feedback_ = feedback;
    ++revision_;
}
void Model::move(live::Point point)
{
    point_ = live::clamp(point, pointer_bounds);
    if (dragging_) {
        card_point_ = live::clamp({point_.x - drag_offset_.x, point_.y - drag_offset_.y},
            {0, 0, world_width - 161, world_height - 97});
    }
    if (!help_ && lesson_ == Lesson::Aim && targets_ < 3) {
        const auto target = aim_target(targets_);
        if (std::hypot(static_cast<double>(point_.x - target.x), static_cast<double>(point_.y - target.y)) <= 32) {
            ++targets_;
            if (targets_ == 3) { finish(L"Nice aim! You reached all three targets. Try holding LB on your next lap."); }
            else { feedback_ = L"On target. Move to the next numbered circle."; }
        }
    }
    ++revision_;
}
void Model::primary(bool down)
{
    if (primary_down_ == down || (keyboard_open_ && !help_)) { return; }
    primary_down_ = down;
    if (!down) { click_ = LocalClick{pressed_at_, point_}; }
    if (down) {
        pressed_at_ = point_;
        if (!help_ && lesson_ == Lesson::Drag && live::contains(card(), point_)) {
            dragging_ = true;
            drag_offset_ = {point_.x - card_point_.x, point_.y - card_point_.y};
            feedback_ = L"Picked up! Keep A held, move right, then release inside the outline.";
        }
    } else if (dragging_) {
        dragging_ = false;
        const auto bounds = card();
        if (live::contains(drop_zone, {bounds.left, bounds.top})
            && live::contains(drop_zone, {bounds.right, bounds.bottom})) {
            finish(L"Placed! Holding A moves the card; releasing A drops it.");
        } else { feedback_ = L"Almost. Fit the whole card inside the outline, then release."; }
    } else if (!help_ && lesson_ == Lesson::Click && live::contains(click_card, pressed_at_) && live::contains(click_card, point_)) {
        primary_clicked_ = true;
        feedback_ = L"Selected with A. Now press X on this card to open its options.";
    } else if (!help_ && lesson_ == Lesson::Scroll && scroll_ >= 480
        && live::contains({140, 264, 480, 334}, pressed_at_) && live::contains({140, 264, 480, 334}, point_)) {
        finish(L"Found it! Right-stick scrolling revealed a card that was out of view.");
    }
    ++revision_;
}
void Model::secondary()
{
    if (!help_ && lesson_ == Lesson::Click && live::contains(click_card, point_)) {
        context_open_ = true;
        if (primary_clicked_) { finish(L"Options opened with X. A selects; X opens a right-click menu. B closes it."); }
        else { feedback_ = L"That is a right click. Press B to dismiss, then tap A to select first."; }
        ++revision_;
    }
}
void Model::scroll(int units)
{
    if (help_ || lesson_ != Lesson::Scroll || !live::contains(scroll_area, point_)) { return; }
    scroll_ = static_cast<int>(std::clamp(static_cast<long long>(scroll_) - units, 0LL, 480LL));
    if (scroll_ == 480) { feedback_ = L"The final card is visible! Move onto it and tap A."; }
    ++revision_;
}
void Model::move_local(live::Point point) { move(point); }
void Model::primary_local(bool down) { local_hold_ = down; primary(down); }
void Model::secondary_local() { secondary(); }
void Model::scroll_local(int units) { scroll(units); }
void Model::open_keyboard()
{
    if (keyboard_open_) { return; }
    if (lesson_ != Lesson::Type) { select(Lesson::Type); }
    keyboard_open_ = true;
    suspend(focused_ ? InputState::Neutral : InputState::Background);
    feedback_ = keyboard_.input_mode() == KeyboardInputMode::OneHanded
        ? L"Center first. Flick UP and center, then DOWN and center to type a."
        : L"Center first, then left stick UP + A types a. This sample stays here.";
}
void Model::close_keyboard()
{
    keyboard_open_ = false;
    suspend(focused_ ? InputState::Neutral : InputState::Background);
}
void Model::dismiss_options()
{
    context_open_ = false;
    ++revision_;
}
void Model::one_handed(bool enabled)
{
    keyboard_.set_input_mode(enabled ? KeyboardInputMode::OneHanded : KeyboardInputMode::FaceButtons);
    suspend(focused_ ? InputState::Neutral : InputState::Background);
}
void Model::show_help(bool visible)
{
    help_ = visible;
    suspend(focused_ ? InputState::Neutral : InputState::Background);
}
bool Model::display_scale(double scale)
{
    if (!std::isfinite(scale) || scale < .4 || scale > 4) { return false; }
    pointer_settings_.speed = live::pointer_speed / scale;
    return true;
}
std::optional<LocalClick> Model::take_click()
{
    const auto click = click_;
    click_.reset();
    return click;
}
void Model::keyboard_step(const Controller& controller, Tick now)
{
    Snapshot local;
    local.sequence = ++keyboard_sequence_;
    local.sampled_at = now;
    local.slots[0] = controller;
    local.slots[0].connection = Connection::Connected;
    local.slots[0].connection_epoch = 1;
    const auto action = keyboard_.step(local, now);
    if (!complete() && (keyboard_.text().find(L'a') != std::wstring::npos || keyboard_.text().find(L'A') != std::wstring::npos)) {
        finish(L"You typed a! Try more letters, then View to close. Nothing was sent to another app.");
    }
    if (action == KeyboardAction::Cancel) { close_keyboard(); }
    if (action == KeyboardAction::Done) { feedback_ = L"Local sample complete. View closes; no Enter was sent anywhere."; }
    ++revision_;
}
void Model::keyboard_local(unsigned group, unsigned face, Tick now)
{
    if (!keyboard_open_ || group >= 8 || face >= 4) { return; }
    // Local accessible fallback exercises the same selection model, not native keystroke delivery.
    constexpr std::array<live::Point, 8> directions{{{0,32767},{23000,23000},{32767,0},{23000,-23000},
        {0,-32767},{-23000,-23000},{-32767,0},{-23000,23000}}};
    Controller pad;
    keyboard_.suspend();
    keyboard_step(pad, now);
    pad.left_x = static_cast<std::int16_t>(directions[group].x);
    pad.left_y = static_cast<std::int16_t>(directions[group].y);
    keyboard_step(pad, now);
    if (keyboard_.input_mode() == KeyboardInputMode::OneHanded) {
        pad.left_x = pad.left_y = 0;
        keyboard_step(pad, now);
        const auto direction = directions[std::array<unsigned, 4>{4, 2, 6, 0}[face]];
        pad.left_x = static_cast<std::int16_t>(direction.x);
        pad.left_y = static_cast<std::int16_t>(direction.y);
        keyboard_step(pad, now);
    } else {
        pad.buttons = static_cast<std::uint16_t>(button_a << face);
        keyboard_step(pad, now);
    }
    pad = {};
    keyboard_step(pad, now);
    suspend(focused_ ? InputState::Physical : InputState::Background);
}
void Model::step(const Snapshot& snapshot, Tick now)
{
    if (!focused_) { return; }
    if (local_hold_) { return; }
    if (discarded_through_ && snapshot.sampled_at <= *discarded_through_) { return; }
    if (!snapshot.sequence || snapshot.sampled_at > now || now - snapshot.sampled_at > live::live_sample_age_ms
        || (last_now_ && (now < *last_now_ || snapshot.sequence < last_sequence_ || snapshot.sampled_at < last_sample_
            || (snapshot.sequence == last_sequence_ && snapshot.sampled_at != last_sample_)))) {
        suspend(InputState::Stale);
        return;
    }
    const bool gap = last_now_ && (now - *last_now_ > live::live_sample_age_ms
        || snapshot.sampled_at - last_sample_ > live::live_sample_age_ms);
    last_now_ = now;
    if (gap) { suspend(InputState::Stale); }
    if (snapshot.sequence == last_sequence_) { return; }
    last_sequence_ = snapshot.sequence;
    last_sample_ = snapshot.sampled_at;
    unsigned connected = 0, active = 0;
    bool error = false, changed = false;
    for (unsigned i = 0; i < slot_count; ++i) {
        const auto& pad = snapshot.slots[i];
        error = error || pad.connection == Connection::Error;
        if (epochs_[i] != pad.connection_epoch) {
            epochs_[i] = pad.connection_epoch;
            neutral_[i] = false;
            changed = changed || slot_ == i;
        }
        if (pad.connection == Connection::Connected) {
            ++connected;
            if (!live::pointer_neutral(pad)) { ++active; }
        } else {
            neutral_[i] = false;
            changed = changed || slot_ == i;
        }
    }
    if (changed || error || connected == 0 || active > 1) {
        suspend(error ? InputState::ReaderError : connected == 0 ? InputState::NoController
            : active > 1 ? InputState::Multiple : InputState::Neutral);
        return;
    }
    for (unsigned i = 0; i < slot_count; ++i) {
        const auto& pad = snapshot.slots[i];
        if (pad.connection != Connection::Connected) { continue; }
        if (live::pointer_neutral(pad)) { neutral_[i] = true; }
        else if (!slot_ && neutral_[i]) {
            slot_ = i;
            armed_ = true;
            pointer_.reset(now);
        }
    }
    if (!slot_ || !armed_) {
        if (active == 0 && std::any_of(neutral_.begin(), neutral_.end(), [](bool value) { return value; })) {
            input_ = InputState::Centered;
        } else if (input_ != InputState::Physical && input_ != InputState::Multiple && input_ != InputState::Stale) {
            input_ = InputState::Neutral;
        }
        if (keyboard_open_ && !help_ && active == 0) { keyboard_step(Controller{}, now); }
        ++revision_;
        return;
    }
    const auto& pad = snapshot.slots[*slot_];
    input_ = InputState::Ready;
    const auto pressed = static_cast<std::uint16_t>(pad.buttons & ~previous_buttons_);
    const auto released = static_cast<std::uint16_t>(previous_buttons_ & ~pad.buttons);
    previous_buttons_ = pad.buttons;
    if (keyboard_open_ && !help_) {
        keyboard_step(pad, now);
        return;
    }
    if (pressed & button_view) {
        if (help_) { show_help(false); } else { exit_requested_ = true; }
        return;
    }
    if (pressed & button_b) { dismiss_options(); primary_down_ = dragging_ = false; return; }
    if (pressed & button_menu) { menu_pending_ = true; }
    if (released & button_menu) {
        if (menu_pending_) { next(); }
        return;
    }
    if (menu_pending_) { pointer_.reset(now); return; }
    if ((pressed & button_y) && !help_) { open_keyboard(); return; }
    const auto motion = pointer_.step(pad, point_, pointer_bounds, now, true, pointer_settings_);
    if (motion.point != point_) { move(motion.point); }
    if (motion.wheel) { scroll(motion.wheel); }
    if (pressed & button_a) { primary(true); }
    if (released & button_a) { primary(false); }
    if (pressed & button_x) { secondary(); }
}
} // namespace gamepad::practice
