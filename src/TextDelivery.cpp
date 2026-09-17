#include "TextDelivery.h"

#include <algorithm>

namespace gamepad {
bool text_target_valid(const TextTarget& target)
{
    return std::all_of(target.windows.begin(), target.windows.end(), [](const TextWindow& window) {
        return window.window && window.process && window.thread;
    });
}

bool text_context_eligible(const TextObservation& observation)
{
    return observation.allowed && observation.stable && !observation.physical_held
        && text_target_valid(observation.target);
}

bool text_eligible(const TextObservation& observation)
{
    return text_context_eligible(observation) && !observation.settling;
}

std::optional<EditEvents> edit_events(KeyboardEdit edit)
{
    EditEvents result;
    const auto pair = [&](TextKey key) {
        result.events[result.count++] = {key, false};
        result.events[result.count++] = {key, true};
    };
    if (edit.kind == KeyboardEditKind::Character) {
        const auto scalar = edit.character;
        if (scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f) || scalar > 0x10ffff
            || (scalar >= 0xd800 && scalar <= 0xdfff)) { return {}; }
        if (scalar <= 0xffff) { pair({TextKeyKind::Unicode, static_cast<std::uint16_t>(scalar)}); }
        else {
            pair({TextKeyKind::Unicode, static_cast<std::uint16_t>(0xd800 + ((scalar - 0x10000) >> 10))});
            pair({TextKeyKind::Unicode, static_cast<std::uint16_t>(0xdc00 + ((scalar - 0x10000) & 0x3ff))});
        }
    } else if (edit.character != 0) { return {}; }
    else if (edit.kind == KeyboardEditKind::Backspace) { pair({TextKeyKind::Backspace}); }
    else if (edit.kind == KeyboardEditKind::Enter) { pair({TextKeyKind::Enter}); }
    else { return {}; }
    return result;
}

bool TextDelivery::start(const TextTarget& target)
{
    if (in_flight_ || phase_ == TextPhase::Ready || recovery_required()) { return false; }
    if (!text_target_valid(target)) { stop(L"Text target unavailable"); return false; }
    target_ = target;
    phase_ = TextPhase::Ready;
    context_lost_ = false;
    reason_.clear();
    head_ = size_ = accepted_events_ = completed_edits_ = 0;
    last_dispatch_ms_ = maximum_dispatch_ms_ = 0;
    cleanup_attempts_ = 0;
    return true;
}

bool TextDelivery::enqueue(KeyboardEdit edit, Tick sampled_at, Tick expires)
{
    if (phase_ != TextPhase::Ready) { return false; }
    if (in_flight_) { fault(L"Reentrant edit request"); return false; }
    const auto input = edit_events(edit);
    if (!input || sampled_at > expires) { fault(L"Invalid keyboard edit"); return false; }
    if (size_ == queue_.size()) { fault(L"Typing queue full; pending edits cancelled"); return false; }
    queue_[(head_ + size_) % queue_.size()] = {*input, 0, sampled_at, expires};
    ++size_;
    return true;
}

void TextDelivery::stop(std::wstring_view reason)
{
    if (phase_ == TextPhase::Stopped) { return; }
    phase_ = TextPhase::Stopped;
    reason_ = reason;
    size_ = 0; // Never replay queued edits after any interrupted session.
    queue_ = {};
}

void TextDelivery::fault(std::wstring_view reason)
{
    ++fault_generation_;
    stop(reason);
}

void TextDelivery::pump(unsigned budget)
{
    if (in_flight_ || phase_ != TextPhase::Ready) { return; }
    in_flight_ = true;
    for (unsigned i = 0; i < std::min(budget, 8u) && size_ && phase_ == TextPhase::Ready; ++i) {
        const auto pending = queue_[head_];
        const auto current = boundary_.observe_text(down_);
        if (phase_ != TextPhase::Ready) { break; }
        const auto current_time = boundary_.text_now();
        if (!text_context_eligible(current) || current.target != target_
            || current_time < pending.sampled_at || current_time > pending.expires) {
            context_lost_ = true;
            stop(L"Focus, physical input or controller freshness changed");
            break;
        }
        if (current.settling) { break; }
        const auto final = boundary_.observe_text(down_);
        if (phase_ != TextPhase::Ready) { break; }
        const auto final_time = boundary_.text_now();
        if (!text_context_eligible(final) || final.target != target_
            || final_time < pending.sampled_at || final_time > pending.expires) {
            context_lost_ = true;
            stop(L"Target changed during validation");
            break;
        }
        if (final.settling) { break; }
        const auto event = pending.input.events[pending.next];
        if (event.up != owned() || (event.up && event.key != *down_)) {
            fault(L"Keyboard output ownership mismatch");
            break;
        }
        const auto accepted = boundary_.send_text(event);
        // Callbacks can stop the session, but cannot erase an in-flight accepted key.
        if (accepted != 1) {
            uncertain_ = accepted > 1;
            fault(L"Keyboard input rejected; do not replay the edit");
            break;
        }
        ++accepted_events_;
        if (event.up) { down_.reset(); }
        else {
            down_ = event.key;
            cleanup_attempts_ = 0;
        }
        if (pending.next == 0) {
            last_dispatch_ms_ = final_time - pending.sampled_at;
            maximum_dispatch_ms_ = std::max(maximum_dispatch_ms_, last_dispatch_ms_);
        }
        if (pending.next + 1 == pending.input.count) { ++completed_edits_; }
        if (phase_ != TextPhase::Ready) { break; }
        if (++queue_[head_].next == pending.input.count) {
            queue_[head_] = {};
            head_ = (head_ + 1) % queue_.size();
            --size_;
        }
    }
    in_flight_ = false;
}

void TextDelivery::cleanup(bool explicit_retry)
{
    if (in_flight_ || phase_ == TextPhase::Ready || !owned() || uncertain_) { return; }
    if (explicit_retry) { cleanup_attempts_ = 0; }
    const auto now = boundary_.text_now();
    if (cleanup_attempts_ >= 3 || (cleanup_attempts_ && now < cleanup_at_)) { return; }
    ++cleanup_attempts_;
    cleanup_at_ = now + 250;
    in_flight_ = true;
    const auto current = boundary_.observe_text(down_);
    if (current.cleanup_allowed && current.stable && !current.physical_held) {
        const auto final = boundary_.observe_text(down_);
        if (final.cleanup_allowed && final.stable && !final.physical_held && final.target == current.target) {
            const unsigned accepted = boundary_.send_text({*down_, true});
            if (accepted == 1) { down_.reset(); ++accepted_events_; }
            else {
                uncertain_ = accepted > 1;
                cleanup_attempts_ = 3;
                fault(L"Keyboard cleanup rejected");
            }
        }
    }
    in_flight_ = false;
}

std::wstring TextDelivery::status() const
{
    if (phase_ == TextPhase::Idle) { return reason_; }
    if (phase_ == TextPhase::Ready) { return L"Typing - Menu Enter; View closes"; }
    std::wstring result = L"Stopped; no replay. View closes. " + std::to_wstring(completed_edits_)
        + L" edits completed; " + std::to_wstring(accepted_events_) + L" events accepted.";
    if (owned()) { result += L" Key-up pending; use Retry release."; }
    if (uncertain_) { result += L" Acceptance uncertain; restart required."; }
    result += L" App receipt is unconfirmed. " + reason_;
    return result;
}
} // namespace gamepad
