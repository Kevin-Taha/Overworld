#pragma once

#include "Core.h"

#include <string>

namespace gamepad {
constexpr std::size_t keyboard_text_limit = 256;
enum class KeyboardPage { Letters, ShiftLetters, Symbols, ShiftSymbols };
enum class KeyboardPhase { Waiting, Typing, Complete };
enum class KeyboardAction { None, Redraw, Done, Cancel };
enum class KeyboardEditKind { Character, Backspace, Enter };
struct KeyboardEdit {
    KeyboardEditKind kind = KeyboardEditKind::Character;
    char32_t character = 0;
    bool operator==(const KeyboardEdit&) const = default;
};
using KeyboardPetals = std::array<std::array<wchar_t, 4>, 8>; // A, B, X, Y; petals clockwise from up.

// Opt-in stick-only alternative to face-button character selection. FaceButtons remains default.
enum class KeyboardInputMode { FaceButtons, OneHanded };
// OneHanded selection has two stages: an outward-then-neutral gesture picks a group, then a
// cardinal-then-neutral gesture picks its A/B/X/Y-position character. Neutral is required between them.
enum class KeyboardFlickStage { Group, Character };

// Private-Use-Area display sentinels for the OneHanded eighth group (Space/Backspace/page-cycle/Enter).
// These are never appended as literal text by Daisywheel; renderers must draw their own icon for each
// and must never emit the raw codepoint as if it were typed content.
inline constexpr wchar_t keyboard_action_space = L'\uE001';      // A (bottom)
inline constexpr wchar_t keyboard_action_backspace = L'\uE002';  // B (right)
inline constexpr wchar_t keyboard_action_page_cycle = L'\uE003'; // X (left)
inline constexpr wchar_t keyboard_action_enter = L'\uE004';      // Y (top)

[[nodiscard]] const KeyboardPetals& keyboard_layout(KeyboardPage page,
    KeyboardInputMode mode = KeyboardInputMode::FaceButtons);
[[nodiscard]] bool keyboard_neutral(const Controller& controller);

// Controller edits with a bounded sample buffer only in preview mode; no native output.
class Daisywheel {
public:
    KeyboardAction step(const Snapshot& snapshot, Tick now);
    void reset();
    bool pin(unsigned slot, std::uint64_t epoch);
    std::optional<KeyboardEdit> take_edit();
    void suspend(); // Retain text, but require fresh neutral before accepting more controller input.
    void set_input_mode(KeyboardInputMode mode); // Safe mid-session switch; never leaves a stray keypress.
    bool set_preview_page(KeyboardPage page); // Page state only; refuses live/pinned keyboards and does not arm input.
    [[nodiscard]] const std::wstring& text() const;
    [[nodiscard]] std::optional<unsigned> selected_petal() const;
    // Petal to highlight for the current gesture stage, distinct from selected_petal() (the
    // committed group): FaceButtons and Character stage return the committed group; Group stage
    // returns the pending pre-commit outward candidate, or nullopt while the stick is neutral
    // (never a stale previous candidate).
    [[nodiscard]] std::optional<unsigned> highlighted_petal() const;
    [[nodiscard]] std::optional<unsigned> selected_slot() const;
    [[nodiscard]] KeyboardPage page() const;
    [[nodiscard]] KeyboardPhase phase() const;
    [[nodiscard]] std::wstring_view status() const;
    [[nodiscard]] bool at_limit() const;
    [[nodiscard]] KeyboardInputMode input_mode() const;
    [[nodiscard]] KeyboardFlickStage flick_stage() const;
    // Transient pre-commit cardinal candidate during Character stage; nullopt in Group stage.
    [[nodiscard]] std::optional<unsigned> selected_face() const;

private:
    struct PadHistory {
        std::optional<std::uint64_t> epoch;
        bool neutral_seen = false;
    };

    void require_neutral(std::wstring_view reason);
    void update_petal(const Controller& controller);
    KeyboardAction update_one_handed(const Controller& controller, bool suppress_commit);
    KeyboardAction commit_one_handed_face(unsigned face, const Controller& controller);
    KeyboardAction enter_activated(const Controller& controller);
    void update_status();
    void append(wchar_t character);
    void erase();

    std::wstring text_;
    std::optional<unsigned> petal_;
    std::optional<unsigned> slot_;
    std::optional<unsigned> pinned_;
    std::array<KeyboardEdit, 2> edits_{};
    unsigned edit_count_ = 0;
    bool live_edits_ = false;
    bool opening_ = false;
    std::array<PadHistory, slot_count> pads_{};
    KeyboardPage page_ = KeyboardPage::Letters;
    KeyboardPhase phase_ = KeyboardPhase::Waiting;
    std::wstring_view status_ = L"Center a controller and release its buttons";
    bool armed_ = false;
    bool stick_engaged_ = false;
    bool menu_pending_ = false;
    std::uint16_t previous_buttons_ = 0;
    std::optional<Tick> backspace_at_;
    bool backspace_repeating_ = false;
    std::optional<Tick> last_now_;
    Tick last_sample_time_ = 0;
    std::uint64_t last_sequence_ = 0;
    KeyboardInputMode input_mode_ = KeyboardInputMode::FaceButtons;
    KeyboardFlickStage flick_stage_ = KeyboardFlickStage::Group;
    std::optional<unsigned> oh_group_candidate_; // Pre-commit Group-stage candidate.
    bool oh_group_engaged_ = false;
    std::optional<unsigned> selected_face_; // Pre-commit Character-stage candidate.
    bool oh_face_engaged_ = false;
    KeyboardPage one_handed_page_ = KeyboardPage::Letters; // Stick-only page cycle; LT/RT still override.
};
} // namespace gamepad
