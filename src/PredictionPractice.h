#pragma once

#include "KeyboardDelivery.h"

namespace gamepad {
// An owned, bounded toy field: deliberately no Windows/native delivery dependency.
class PredictionPractice final : public TextBoundary {
public:
    PredictionPractice() : keyboard_(*this)
    {
        observation_.target.windows = {{{1, 1, 1}, {1, 1, 1}, {2, 1, 1}}};
        observation_.stable = observation_.cleanup_allowed = true;
        observation_.physical_held = false;
    }
    void step(const Snapshot& snapshot, Tick now)
    {
        now_ = now;
        keyboard_.step(snapshot, now, {observation_.allowed, {}});
    }
    void activate()
    {
        observation_.allowed = true;
    }
    void clear()
    {
        ++observation_.target.activity;
        observation_.allowed = false;
        keyboard_.dismiss(L"Practice reset; release controls and reopen");
        field_.clear();
    }
    void set_input_mode(KeyboardInputMode mode) { keyboard_.set_input_mode(mode); }
    [[nodiscard]] const KeyboardDelivery& keyboard() const { return keyboard_; }
    [[nodiscard]] std::wstring_view sample() const { return field_; }
    TextObservation observe_text(std::optional<TextKey>) override { return observation_; }
    Tick text_now() const override { return now_; }
    bool prediction_preview() const override { return true; }
    unsigned send_text(TextEvent event) override
    {
        if (event.up) { return 1; }
        if (!observation_.allowed) { return 0; }
        if (event.key.kind == TextKeyKind::Backspace) {
            if (!field_.empty()) { field_.pop_back(); }
            return 1;
        }
        if (field_.size() == keyboard_text_limit) { return 0; }
        // Enter is a visible local boundary, never a command or an external key.
        field_.push_back(event.key.kind == TextKeyKind::Enter ? L' ' : static_cast<wchar_t>(event.key.unit));
        return 1;
    }

private:
    TextObservation observation_;
    Tick now_ = 0;
    std::wstring field_;
    KeyboardDelivery keyboard_;
};

inline void seed_prediction_demo(PredictionPractice& practice, KeyboardInputMode mode)
{
    Snapshot snapshot;
    auto& pad = snapshot.slots[0];
    pad.connection = Connection::Connected;
    pad.connection_epoch = 1;
    practice.activate();
    practice.set_input_mode(mode);
    const auto tick = [&] {
        ++snapshot.sequence;
        snapshot.sampled_at = snapshot.sequence * 16;
        practice.step(snapshot, snapshot.sampled_at);
    };
    tick(); tick();
    pad.buttons = 0x8000; tick();
    pad.buttons = 0; tick();
    pad.buttons = 0x0200; tick(); // Explicit space, then "he" from this keyboard.
    pad.buttons = 0; tick();
    if (mode == KeyboardInputMode::FaceButtons) {
        pad.left_x = pad.left_y = 26000; tick();
        pad.buttons = 0x8000; tick();
        pad.buttons = 0; tick();
        pad.buttons = 0x1000; tick();
        pad.buttons = 0; pad.left_x = pad.left_y = 0; tick();
    } else {
        for (bool first : {true, false}) {
            pad.left_x = pad.left_y = 26000; tick();
            pad.left_x = pad.left_y = 0; tick();
            pad.left_y = first ? 28000 : -28000; tick();
            pad.left_y = 0; tick();
        }
    }
}
} // namespace gamepad
