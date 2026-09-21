#pragma once

#include "Keyboard.h"
#include "Pointer.h"

#include <bitset>

namespace overworld::practice {
constexpr int world_width = 620;
constexpr int world_height = 380;
constexpr int canvas_width = 1000, canvas_height = 740;
constexpr int scene_left = 30, scene_top = 150;
constexpr unsigned lesson_count = 5;
constexpr Tick reader_lease_ms = 50;
constexpr bool reader_lease_current(Tick deadline, Tick now)
{
    return now < deadline && deadline - now <= reader_lease_ms;
}
enum class Lesson : unsigned { Aim, Click, Drag, Scroll, Type };
enum class InputState { Background, NoController, Neutral, Centered, Ready, Physical, Multiple, Stale, ReaderError };

struct LessonText {
    std::wstring_view title, instruction, tip;
};
struct LocalClick {
    live::Point down, up;
};
[[nodiscard]] const LessonText& lesson_text(Lesson lesson);
[[nodiscard]] std::wstring_view input_text(InputState state);
[[nodiscard]] live::Point aim_target(unsigned index);
inline constexpr live::Rect click_card{205, 116, 415, 246};
inline constexpr live::Rect drag_start{65, 148, 225, 244};
inline constexpr live::Rect drop_zone{385, 118, 585, 280};
inline constexpr live::Rect scroll_area{120, 40, 500, 340};

// Only local scene state. No native handles, delivery objects, settings, or output callbacks.
class Model {
public:
    void focus(bool focused, Tick now = 0);
    void discard_samples_through(Tick now);
    void physical_takeover();
    void cancel_local();
    void step(const Snapshot& snapshot, Tick now);
    void select(Lesson lesson);
    void reset();
    void retry();
    void next();
    void open_keyboard();
    void close_keyboard();
    void dismiss_options();
    void one_handed(bool enabled);
    void show_help(bool visible);
    bool display_scale(double scale);
    void move_local(live::Point point);
    void primary_local(bool down);
    void secondary_local();
    void scroll_local(int units);
    void keyboard_local(unsigned group, unsigned face, Tick now);
    std::optional<LocalClick> take_click();

    [[nodiscard]] Lesson lesson() const { return lesson_; }
    [[nodiscard]] InputState input() const { return input_; }
    [[nodiscard]] live::Point pointer() const { return point_; }
    [[nodiscard]] live::Rect card() const;
    [[nodiscard]] unsigned targets() const { return targets_; }
    [[nodiscard]] bool primary_clicked() const { return primary_clicked_; }
    [[nodiscard]] bool context_open() const { return context_open_; }
    [[nodiscard]] bool dragging() const { return dragging_; }
    [[nodiscard]] bool local_hold() const { return local_hold_; }
    [[nodiscard]] int scroll_offset() const { return scroll_; }
    [[nodiscard]] bool keyboard_open() const { return keyboard_open_; }
    [[nodiscard]] bool help() const { return help_; }
    [[nodiscard]] const Daisywheel& keyboard() const { return keyboard_; }
    [[nodiscard]] bool complete() const { return completed_[static_cast<unsigned>(lesson_)]; }
    [[nodiscard]] const std::array<bool, lesson_count>& completed() const { return completed_; }
    [[nodiscard]] unsigned completed_count() const;
    [[nodiscard]] bool exit_requested() const { return exit_requested_; }
    [[nodiscard]] std::wstring_view feedback() const { return feedback_; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }

private:
    void suspend(InputState reason);
    void reset_scene();
    void move(live::Point point);
    void primary(bool down);
    void secondary();
    void scroll(int units);
    void keyboard_step(const Controller& controller, Tick now);
    void finish(std::wstring_view feedback);

    Lesson lesson_ = Lesson::Aim;
    InputState input_ = InputState::Background;
    std::array<bool, lesson_count> completed_{};
    std::array<bool, slot_count> neutral_{};
    std::array<std::uint64_t, slot_count> epochs_{};
    std::optional<unsigned> slot_;
    std::optional<Tick> last_now_;
    std::optional<Tick> discarded_through_;
    Tick last_sample_ = 0;
    std::uint64_t last_sequence_ = 0, keyboard_sequence_ = 0, revision_ = 1;
    std::uint16_t previous_buttons_ = 0;
    bool focused_ = false, armed_ = false, local_hold_ = false, menu_pending_ = false;
    bool primary_down_ = false, primary_clicked_ = false, context_open_ = false;
    bool dragging_ = false, keyboard_open_ = false, exit_requested_ = false, help_ = false;
    unsigned targets_ = 0;
    int scroll_ = 0;
    live::Point point_{70, 300}, pressed_at_{}, card_point_{65, 148}, drag_offset_{};
    live::Pointer pointer_;
    live::PointerSettings pointer_settings_;
    std::optional<LocalClick> click_;
    Daisywheel keyboard_;
    KeyboardPage local_page_ = KeyboardPage::Letters;
    bool local_keyboard_ = false;
    std::wstring_view feedback_ = L"Move the pointer onto each numbered target.";
};

// Own-window keyboard messages only; never a global hook or asynchronous key observer.
class LocalKeys {
public:
    bool observe(Model& model, unsigned key, bool down);
    void reset() { held_.reset(); }
    [[nodiscard]] bool held() const { return held_.any(); }
private:
    std::bitset<256> held_;
};
} // namespace overworld::practice
