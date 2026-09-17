#pragma once

#include <cstdint>
#include <optional>

namespace gamepad {
enum class CaptionButton { None, Minimize, Maximize, Close };

struct CaptionContext {
    std::uintptr_t window = 0;
    std::uintptr_t foreground = 0;
    std::uintptr_t focus = 0;
    unsigned slot = 0;
    std::uint64_t epoch = 0;
    CaptionButton button = CaptionButton::None;
    bool eligible = false;
    bool primary_owned = false;
    bool primary_down = false;
};

// This is a self-window action, never an input ledger. A processed matching up
// and the real dispatcher's released ownership are both required before commit.
class SelfCaption {
public:
    [[nodiscard]] bool pending() const { return origin_.has_value(); }
    [[nodiscard]] std::uintptr_t window() const { return origin_ ? origin_->window : 0; }
    void cancel() { origin_.reset(); ready_ = false; released_ = false; }

    bool down(const CaptionContext& context, bool tagged, bool double_click = false)
    {
        if (!tagged || !context.window || context.button == CaptionButton::None) {
            cancel();
            return false;
        }
        // Consumption is independent of authorization: even a stale tagged self
        // caption message must not enter Windows' timer-starving mouse tracker.
        if (!context.eligible || !context.foreground || !context.primary_owned) { cancel(); return true; }
        // The accepted-down ledger is the evidence, not a newer reader snapshot:
        // A may already be released while this down is still queued for dispatch.
        if (origin_ || double_click) { cancel(); return true; }
        origin_ = context;
        ready_ = released_ = false;
        return true;
    }

    void observe(const CaptionContext& context)
    {
        if (!origin_) { return; }
        if (!matches(context) || (released_ && (context.primary_owned || context.primary_down))) {
            cancel();
            return;
        }
        if (!context.primary_owned) {
            if (context.primary_down) { cancel(); return; } // Cleanup while A remains down.
            released_ = true;
        }
    }

    bool up(const CaptionContext& context, bool tagged)
    {
        if (!origin_) { return false; }
        observe(context);
        if (!origin_) { return false; }
        if (!tagged || context.primary_owned || context.primary_down || ready_) {
            cancel();
            return false;
        }
        ready_ = true;
        return true;
    }

    std::optional<CaptionContext> take(const CaptionContext& context)
    {
        observe(context);
        if (!origin_ || !ready_ || context.primary_owned || context.primary_down) { return {}; }
        const auto result = origin_;
        cancel();
        return result;
    }

private:
    bool matches(const CaptionContext& current) const
    {
        return current.eligible && current.window == origin_->window
            && current.foreground == origin_->foreground && current.focus == origin_->focus
            && current.slot == origin_->slot && current.epoch == origin_->epoch
            && current.button == origin_->button;
    }
    std::optional<CaptionContext> origin_;
    bool ready_ = false;
    bool released_ = false;
};
} // namespace gamepad
