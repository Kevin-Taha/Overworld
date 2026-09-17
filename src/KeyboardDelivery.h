#pragma once

#include "Keyboard.h"
#include "TextDelivery.h"
#include "Voice.h"
#include "WordPrediction.h"

namespace gamepad {
enum class DesktopKeyboardPhase { Closed, Composing, Paused };

struct KeyboardEligibility {
    bool desktop = false;
    std::optional<unsigned> preferred;
    bool native = false;
};

// UI-thread controller routing and delivery; no HWND, renderer, or pointer dependency.
class KeyboardDelivery {
public:
    explicit KeyboardDelivery(TextBoundary& boundary) : boundary_(boundary), delivery_(boundary) {}
    void step(const Snapshot& snapshot, Tick now, KeyboardEligibility eligibility);
    void block(std::wstring_view reason);
    void dismiss(std::wstring_view reason = L"Keyboard closed; queued edits cancelled");
    void recover();
    // Safe mid-session mode switch: dismisses/rearms the visible keyboard rather than corrupting
    // an in-progress gesture; a fresh open uses the new mode's opener/layout.
    void set_input_mode(KeyboardInputMode mode);
    void set_native_mode_enabled(bool enabled);
    void set_voice(VoiceBoundary* voice);
    [[nodiscard]] const VoiceUpdate& voice_status() const { return voice_status_; }
    [[nodiscard]] bool native_session() const { return native_session_; }
    [[nodiscard]] bool navigation_filter_eligible(const TextObservation& observation) const {
        return phase_ == DesktopKeyboardPhase::Composing && delivery_.phase() == TextPhase::Ready
            && !delivery_.recovery_required() && text_eligible(observation) && observation.target == target_;
    }
    [[nodiscard]] bool visible() const { return phase_ != DesktopKeyboardPhase::Closed; }
    [[nodiscard]] bool accepts_typing() const {
        return phase_ == DesktopKeyboardPhase::Composing && keyboard_.phase() == KeyboardPhase::Typing
            && delivery_.phase() == TextPhase::Ready;
    }
    [[nodiscard]] bool blocks_pointer() const { return visible() || consuming_ || delivery_.recovery_required(); }
    [[nodiscard]] bool recovery_required() const { return delivery_.recovery_required(); }
    [[nodiscard]] bool busy() const { return stepping_ || delivery_.busy(); }
    [[nodiscard]] DesktopKeyboardPhase phase() const { return phase_; }
    [[nodiscard]] const Daisywheel& model() const { return keyboard_; }
    [[nodiscard]] const TextDelivery& delivery() const { return delivery_; }
    [[nodiscard]] PredictionView prediction_view() const;
    [[nodiscard]] std::wstring status() const;
    [[nodiscard]] const std::wstring& last_dismissal() const { return last_dismissal_; }

private:
    bool fresh(const Snapshot& snapshot, Tick now) const;
    void observe_invocation(const Snapshot& snapshot, Tick now, KeyboardEligibility eligibility);
    void observe_native_invocation(const Snapshot& snapshot, Tick now, KeyboardEligibility eligibility);
    void open(const Snapshot& snapshot, Tick now, unsigned slot, bool native);
    bool voice_step(const Snapshot& snapshot, Tick now);
    void cancel_voice();
    bool prediction_step(const Snapshot& snapshot);
    TextBoundary& boundary_;
    TextDelivery delivery_;
    Daisywheel keyboard_;
    TextTarget target_;
    DesktopKeyboardPhase phase_ = DesktopKeyboardPhase::Closed;
    std::optional<Snapshot> previous_;
    std::array<bool, slot_count> neutral_{};
    unsigned slot_ = 0;
    std::uint64_t epoch_ = 0;
    std::uint64_t interruption_ = 0;
    bool view_released_ = false;
    bool consuming_ = false;
    bool stepping_ = false;
    bool native_enabled_ = false;
    bool native_session_ = false;
    bool opening_chord_ = false;
    std::optional<unsigned> native_candidate_;
    Tick native_started_ = 0;
    std::wstring reason_;
    std::wstring last_dismissal_;
    VoiceBoundary* voice_ = nullptr;
    VoiceUpdate voice_status_;
    std::uint64_t voice_generation_ = 0;
    std::uint16_t voice_buttons_ = 0;
    bool voice_released_ = false;
    Tick voice_deadline_ = 0;
    std::u32string dictation_;
    std::size_t dictation_offset_ = 0;
    WordPrediction prediction_;
    bool prediction_neutral_ = false;
    bool prediction_appending_ = false;
};
} // namespace gamepad
