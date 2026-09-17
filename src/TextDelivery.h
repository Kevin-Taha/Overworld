#pragma once

#include "Keyboard.h"

#include <string>

namespace gamepad {
struct TextWindow {
    std::uintptr_t window = 0;
    std::uint32_t process = 0;
    std::uint32_t thread = 0;
    bool operator==(const TextWindow&) const = default;
};

struct TextTarget {
    std::array<TextWindow, 3> windows{}; // Foreground, active, focused native window.
    std::uint64_t context = 0;
    std::uint64_t activity = 0;
    std::uint64_t focus_revision = 0;
    bool operator==(const TextTarget&) const = default;
};

enum class PredictionSupport { Off, Unsupported, Protected, Unavailable, PlainEdit };
struct PredictionEvidence {
    PredictionSupport support = PredictionSupport::Off;
    std::optional<std::uint32_t> caret;
    std::uint64_t revision = 0;
    Tick sampled_at = 0;
};

struct TextObservation {
    TextTarget target;
    bool allowed = false;
    bool cleanup_allowed = false;
    bool physical_held = true;
    bool stable = false;
    std::wstring_view reason = L"Text target unavailable";
    bool settling = false;
    PredictionEvidence prediction;
    std::uint64_t text_revision = 0;
};
[[nodiscard]] inline bool prediction_proven(const TextObservation& observation, Tick now)
{
    return observation.prediction.support == PredictionSupport::PlainEdit && observation.prediction.caret
        && observation.prediction.revision == observation.text_revision
        && observation.prediction.sampled_at <= now && now - observation.prediction.sampled_at <= 100;
}

enum class TextKeyKind { Unicode, Backspace, Enter };
struct TextKey {
    TextKeyKind kind = TextKeyKind::Unicode;
    std::uint16_t unit = 0;
    bool operator==(const TextKey&) const = default;
};

struct TextEvent {
    TextKey key;
    bool up = false;
    bool operator==(const TextEvent&) const = default;
};

struct EditEvents {
    std::array<TextEvent, 4> events{};
    unsigned count = 0;
};
[[nodiscard]] std::optional<EditEvents> edit_events(KeyboardEdit edit);

class TextBoundary {
public:
    virtual ~TextBoundary() = default;
    virtual TextObservation observe_text(std::optional<TextKey> owned) = 0;
    virtual Tick text_now() const = 0;
    // Exactly one Unicode or virtual-key down/up. Only 0 and 1 are valid counts.
    virtual unsigned send_text(TextEvent event) = 0;
    // Only the in-memory practice sink opts in. Native/unknown/password targets never do.
    virtual bool prediction_preview() const { return false; }
    virtual bool prediction_enabled() const { return prediction_preview(); }
    virtual void prediction_closed() {}
    [[nodiscard]] bool native_keyboard_session() const { return native_keyboard_session_; }
private:
    friend class KeyboardDelivery;
    bool native_keyboard_session_ = false;
};

[[nodiscard]] bool text_target_valid(const TextTarget& target);
[[nodiscard]] bool text_context_eligible(const TextObservation& observation);
[[nodiscard]] bool text_eligible(const TextObservation& observation);
enum class TextPhase { Idle, Ready, Stopped };
constexpr std::size_t keyboard_edit_queue_limit = 32;
constexpr Tick prediction_confirmation_ms = 100;

struct CompletedTextEdits {
    std::array<KeyboardEdit, 4> edits{};
    unsigned count = 0;
};

class TextDelivery {
public:
    explicit TextDelivery(TextBoundary& boundary) : boundary_(boundary) {}
    bool start(const TextTarget& target);
    bool enqueue(KeyboardEdit edit, Tick sampled_at, Tick expires,
        std::optional<std::uint32_t> prediction_caret = {});
    CompletedTextEdits pump(unsigned budget = 8);
    void observe_prediction_caret(const TextObservation& observation, Tick now);
    void stop(std::wstring_view reason);
    void cleanup(bool explicit_retry = false);
    [[nodiscard]] TextPhase phase() const { return phase_; }
    [[nodiscard]] bool owned() const { return down_.has_value(); }
    [[nodiscard]] std::optional<TextKey> owned_key() const { return down_; }
    [[nodiscard]] bool uncertain() const { return uncertain_; }
    [[nodiscard]] bool context_lost() const { return context_lost_; }
    // Monotonic across sessions; consumers revoke authorization once per new failure.
    [[nodiscard]] std::uint64_t fault_generation() const { return fault_generation_; }
    [[nodiscard]] bool busy() const { return in_flight_; }
    [[nodiscard]] bool recovery_required() const { return owned() || uncertain_; }
    [[nodiscard]] std::size_t accepted_events() const { return accepted_events_; }
    [[nodiscard]] std::size_t completed_edits() const { return completed_edits_; }
    [[nodiscard]] std::size_t pending_edits() const { return size_; }
    [[nodiscard]] Tick last_dispatch_ms() const { return last_dispatch_ms_; }
    [[nodiscard]] Tick maximum_dispatch_ms() const { return maximum_dispatch_ms_; }
    [[nodiscard]] const std::wstring& reason() const { return reason_; }
    [[nodiscard]] std::wstring status() const;

private:
    void fault(std::wstring_view reason);
    bool prediction_caret_ready(const TextObservation& observation, std::uint32_t expected, Tick now);
    TextBoundary& boundary_;
    TextPhase phase_ = TextPhase::Idle;
    TextTarget target_;
    std::wstring reason_;
    struct PendingEdit {
        EditEvents input;
        KeyboardEdit edit;
        unsigned next = 0;
        Tick sampled_at = 0;
        Tick expires = 0;
        std::optional<std::uint32_t> prediction_caret;
    };
    std::array<PendingEdit, keyboard_edit_queue_limit> queue_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::optional<TextKey> down_;
    struct PendingCaret {
        std::uint32_t before = 0, after = 0;
        Tick accepted_at = 0, expires = 0;
    };
    std::optional<PendingCaret> pending_caret_;
    std::size_t accepted_events_ = 0;
    std::size_t completed_edits_ = 0;
    Tick last_dispatch_ms_ = 0;
    Tick maximum_dispatch_ms_ = 0;
    bool in_flight_ = false;
    bool uncertain_ = false;
    bool context_lost_ = false;
    std::uint64_t fault_generation_ = 0;
    unsigned cleanup_attempts_ = 0;
    Tick cleanup_at_ = 0;
};
} // namespace gamepad
