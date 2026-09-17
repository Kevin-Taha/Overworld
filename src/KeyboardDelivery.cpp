#include "KeyboardDelivery.h"

#include <algorithm>

namespace gamepad {
namespace {
constexpr Tick fresh_ms = 100;
constexpr std::uint16_t y_button = 0x8000;
constexpr std::uint16_t left_stick_click_button = 0x0040;
constexpr std::uint16_t view_button = 0x0020;
constexpr std::uint16_t mode_buttons = 0x0030;
constexpr std::uint16_t voice_button = 0x0080;
constexpr Tick dictation_intent_ms = 90000;

bool all_neutral(const Snapshot& snapshot)
{
    return std::all_of(snapshot.slots.begin(), snapshot.slots.end(), [](const Controller& pad) {
        return pad.connection != Connection::Connected || keyboard_neutral(pad);
    });
}
} // namespace

bool KeyboardDelivery::fresh(const Snapshot& snapshot, Tick now) const
{
    return snapshot.sequence && snapshot.sampled_at <= now && now - snapshot.sampled_at <= fresh_ms
        && (!previous_ || (snapshot.sequence >= previous_->sequence && snapshot.sampled_at >= previous_->sampled_at
            && (snapshot.sequence != previous_->sequence || snapshot.sampled_at == previous_->sampled_at)
            && snapshot.sampled_at - previous_->sampled_at <= fresh_ms));
}

void KeyboardDelivery::observe_invocation(const Snapshot& snapshot, Tick now, KeyboardEligibility eligibility)
{
    if (eligibility.native && !eligibility.desktop) {
        observe_native_invocation(snapshot, now, eligibility);
        return;
    }
    native_candidate_.reset();
    if (consuming_) {
        if (all_neutral(snapshot)) { consuming_ = false; }
        neutral_ = {};
        return;
    }
    unsigned y_count = 0;
    std::optional<unsigned> candidate;
    // OneHanded also accepts a fresh left-stick click as an opener so invoking the keyboard does
    // not require the right-hand Y button; Y remains valid as a fallback in every mode.
    const bool stick_click_opens = keyboard_.input_mode() == KeyboardInputMode::OneHanded;
    const std::uint16_t opener_mask = stick_click_opens
        ? static_cast<std::uint16_t>(y_button | left_stick_click_button) : y_button;
    for (unsigned i = 0; i < slot_count; ++i) {
        const auto& pad = snapshot.slots[i];
        if (pad.connection != Connection::Connected) { neutral_[i] = false; continue; }
        const bool continuity = previous_ && previous_->slots[i].connection == Connection::Connected
            && previous_->slots[i].connection_epoch == pad.connection_epoch;
        if (!continuity) { neutral_[i] = false; }
        if (pad.buttons & opener_mask) {
            ++y_count;
            auto rest = pad;
            rest.buttons &= ~opener_mask;
            if (neutral_[i] && keyboard_neutral(rest)
                && (!eligibility.preferred || *eligibility.preferred == i)) { candidate = i; }
        }
        neutral_[i] = eligibility.desktop && keyboard_neutral(pad);
    }
    if (!y_count) { return; }
    consuming_ = true;
    neutral_ = {};
    reason_ = stick_click_opens
        ? L"Keyboard not opened; release controls and use Y or left-stick click alone on an eligible desktop"
        : L"Keyboard not opened; release controls and use Y alone on an eligible desktop";
    if (!eligibility.desktop || y_count != 1 || !candidate || delivery_.recovery_required()) { return; }
    for (unsigned i = 0; i < slot_count; ++i) {
        if (i != *candidate && snapshot.slots[i].connection == Connection::Connected
            && !keyboard_neutral(snapshot.slots[i])) { return; }
    }
    open(snapshot, now, *candidate, false);
}

void KeyboardDelivery::observe_native_invocation(const Snapshot& snapshot, Tick now, KeyboardEligibility eligibility)
{
    constexpr std::uint16_t invocation = view_button | y_button;
    if (!native_enabled_ || delivery_.recovery_required()) {
        native_candidate_.reset();
        neutral_ = {};
        return;
    }
    if (consuming_) {
        if (all_neutral(snapshot)) { consuming_ = false; }
        neutral_ = {};
        native_candidate_.reset();
        return;
    }
    unsigned active = 0;
    std::optional<unsigned> candidate;
    for (unsigned i = 0; i < slot_count; ++i) {
        const auto& pad = snapshot.slots[i];
        const bool continuity = previous_ && previous_->slots[i].connection == Connection::Connected
            && previous_->slots[i].connection_epoch == pad.connection_epoch;
        if (!continuity || pad.connection != Connection::Connected) { neutral_[i] = false; }
        if (pad.connection != Connection::Connected) { continue; }
        if (!keyboard_neutral(pad)) {
            ++active;
            auto rest = pad;
            rest.buttons &= ~invocation;
            if (continuity && (pad.buttons & invocation) == invocation && keyboard_neutral(rest)
                && (!eligibility.preferred || *eligibility.preferred == i)
                && (neutral_[i] || native_candidate_ == i)) { candidate = i; }
        }
        neutral_[i] = keyboard_neutral(pad);
    }
    if (active == 0) { native_candidate_.reset(); return; }
    if (active != 1 || !candidate) {
        native_candidate_.reset();
        consuming_ = true;
        neutral_ = {};
        return;
    }
    if (native_candidate_ != candidate) {
        native_candidate_ = candidate;
        native_started_ = snapshot.sampled_at;
    }
    if (snapshot.sampled_at - native_started_ < 500) { return; }
    consuming_ = true;
    neutral_ = {};
    native_candidate_.reset();
    open(snapshot, now, *candidate, true);
}

void KeyboardDelivery::open(const Snapshot& snapshot, Tick now, unsigned slot, bool native)
{
    boundary_.native_keyboard_session_ = native;
    struct ScopeGuard {
        KeyboardDelivery& keyboard;
        ~ScopeGuard() {
            if (!keyboard.visible()) { keyboard.boundary_.native_keyboard_session_ = false; }
        }
    } scope{*this};
    const auto interruption = interruption_;
    const auto target = boundary_.observe_text({});
    if (!text_eligible(target) || boundary_.text_now() > snapshot.sampled_at + fresh_ms) {
        reason_ = target.reason;
        return;
    }
    const auto final = boundary_.observe_text({});
    if (!text_eligible(final) || final.target != target.target || interruption != interruption_
        || boundary_.text_now() > snapshot.sampled_at + fresh_ms || boundary_.text_now() < now) {
        reason_ = L"Target changed while opening keyboard; release controls and try again";
        return;
    }
    if (!delivery_.start(final.target)) { reason_ = L"Unresolved text output; use Retry release"; return; }
    slot_ = slot;
    epoch_ = snapshot.slots[slot_].connection_epoch;
    target_ = final.target;
    if (!keyboard_.pin(slot_, epoch_)) {
        reason_ = L"Invoking controller selection invalid; keyboard not opened";
        return;
    }
    phase_ = DesktopKeyboardPhase::Composing;
    prediction_.clear();
    prediction_neutral_ = prediction_appending_ = false;
    prediction_trusted_ = prediction_ready_ = false;
    prediction_caret_.reset();
    prediction_trust_started_.reset();
    prediction_caret_wait_.reset();
    prediction_observe(final, now);
    voice_buttons_ = snapshot.slots[slot_].buttons;
    native_session_ = opening_chord_ = native;
    view_released_ = false;
    reason_.clear();
}

void KeyboardDelivery::step(const Snapshot& snapshot, Tick now, KeyboardEligibility eligibility)
{
    if (stepping_) { return; }
    stepping_ = true;
    const auto run = [&] {
        delivery_.cleanup();
        if (!fresh(snapshot, now)) {
            native_candidate_.reset();
            neutral_ = {};
            consuming_ = consuming_ || std::any_of(snapshot.slots.begin(), snapshot.slots.end(), [](const Controller& pad) {
                return pad.connection == Connection::Connected && (pad.buttons & y_button);
            });
            view_released_ = false;
            if (visible()) { block(L"Controller interrupted. View closes."); }
            // A fresh observation after this discarded baseline can rearm, never the stale gesture.
            previous_.reset();
            return;
        }
        if (previous_ && snapshot.sequence == previous_->sequence) { return; }
        if (!visible()) {
            observe_invocation(snapshot, now, eligibility);
            previous_ = snapshot;
            return;
        }
        const auto& pad = snapshot.slots[slot_];
        const bool same_device = previous_ && previous_->slots[slot_].connection == Connection::Connected
            && previous_->slots[slot_].connection_epoch == pad.connection_epoch;
        const bool view = pad.connection == Connection::Connected && (pad.buttons & view_button);
        const bool cancel = !opening_chord_ && view && view_released_ && same_device;
        view_released_ = pad.connection == Connection::Connected && !view;
        previous_ = snapshot;
        const bool chord = std::any_of(snapshot.slots.begin(), snapshot.slots.end(), [](const Controller& controller) {
            return controller.connection == Connection::Connected && (controller.buttons & mode_buttons) == mode_buttons;
        });
        if (cancel || chord) { dismiss(); return; }
        if (phase_ == DesktopKeyboardPhase::Paused) { return; }
        if (pad.connection != Connection::Connected || pad.connection_epoch != epoch_) {
            block(L"Controller disconnected. Reconnect, then View closes.");
            return;
        }
        if (native_session_ ? (!native_enabled_ || !eligibility.native) : !eligibility.desktop) {
            dismiss(L"Keyboard mode no longer eligible");
            return;
        }
        const auto current = boundary_.observe_text(delivery_.owned_key());
        if (!visible() || phase_ == DesktopKeyboardPhase::Paused) { return; }
        if (!text_context_eligible(current) || current.target != target_) {
            dismiss(current.target.activity != target_.activity ? L"Physical activity changed"
                : current.target.windows != target_.windows ? L"Native foreground or text-field identity changed"
                : current.target.focus_revision != target_.focus_revision ? L"Focused accessibility object changed"
                : current.target.context != target_.context ? L"Unsafe desktop context changed"
                : current.physical_held ? L"Physical key or mouse button held"
                : !current.stable ? L"Text recipient routing changed during validation"
                : L"Text eligibility changed");
            return;
        }
        if (phase_ == DesktopKeyboardPhase::Composing) {
            if (opening_chord_) {
                if ((pad.buttons & (view_button | y_button)) != 0 || !keyboard_neutral(pad)) {
                    view_released_ = false;
                    return;
                }
                opening_chord_ = false;
                view_released_ = true;
            }
            prediction_observe(current, now);
            auto completion_controls = pad;
            completion_controls.buttons &= ~0x0001;
            if (!boundary_.prediction_preview() && prediction_appending_ && delivery_.pending_edits()
                && (!prediction_trusted_ || !keyboard_neutral(completion_controls))) {
                block(L"Word completion cancelled; remaining suffix not sent");
                return;
            }
            if (voice_step(snapshot, now)) { return; }
            const bool prediction_consumed = prediction_step(snapshot);
            if (phase_ != DesktopKeyboardPhase::Composing) { return; }
            const auto action = prediction_consumed ? KeyboardAction::None : keyboard_.step(snapshot, now);
            if (action == KeyboardAction::Cancel) { dismiss(); return; }
            while (const auto edit = keyboard_.take_edit()) {
                if (!delivery_.enqueue(*edit, snapshot.sampled_at, snapshot.sampled_at + fresh_ms)) {
                    block(delivery_.reason());
                    return;
                }
            }
            const auto generation = interruption_;
            const auto completed = delivery_.pump();
            if (boundary_.prediction_enabled() && generation == interruption_) {
                for (unsigned i = 0; i < completed.count; ++i) { prediction_complete(completed.edits[i], now); }
                if (!boundary_.prediction_preview()) {
                    const auto after = boundary_.observe_text(delivery_.owned_key());
                    if (generation == interruption_ && after.target == target_ && text_context_eligible(after)) {
                        prediction_observe(after, boundary_.text_now());
                    } else { prediction_.clear(); prediction_ready_ = false; }
                }
            }
            if (!delivery_.pending_edits()) { prediction_appending_ = false; }
            if (phase_ == DesktopKeyboardPhase::Closed) { return; }
            if (delivery_.phase() == TextPhase::Stopped) {
                if (delivery_.context_lost()) { dismiss(delivery_.reason()); }
                else { block(delivery_.reason()); }
            }
        }
    };
    run();
    stepping_ = false;
}

void KeyboardDelivery::block(std::wstring_view reason)
{
    const bool already_paused = phase_ == DesktopKeyboardPhase::Paused;
    cancel_voice();
    ++interruption_;
    neutral_ = {};
    native_candidate_.reset();
    boundary_.native_keyboard_session_ = false;
    opening_chord_ = false;
    if (!visible() || already_paused) { return; }
    reason_ = reason;
    delivery_.stop(reason);
    phase_ = DesktopKeyboardPhase::Paused;
    keyboard_.suspend();
}

void KeyboardDelivery::dismiss(std::wstring_view reason)
{
    cancel_voice();
    if (visible()) { last_dismissal_ = reason; }
    ++interruption_;
    delivery_.stop(reason);
    boundary_.native_keyboard_session_ = false;
    native_session_ = opening_chord_ = false;
    native_candidate_.reset();
    phase_ = DesktopKeyboardPhase::Closed;
    consuming_ = true;
    neutral_ = {};
    view_released_ = false;
    reason_.clear();
    // Do not mutate the model/text while a native boundary can reenter its caller.
    if (!stepping_) { keyboard_.reset(); }
}

void KeyboardDelivery::recover()
{
    if (delivery_.phase() == TextPhase::Ready) { block(L"Release recovery requested"); }
    delivery_.cleanup(true);
}

void KeyboardDelivery::set_input_mode(KeyboardInputMode mode)
{
    if (keyboard_.input_mode() == mode) { return; }
    // Never glitch an in-progress gesture: close a visible keyboard first, then rearm the model so
    // the next open uses the new mode's opener/layout with no stray keypress.
    if (visible()) { dismiss(L"Input mode changed; keyboard closed. Reopen to use it."); }
    keyboard_.set_input_mode(mode);
}

void KeyboardDelivery::set_native_mode_enabled(bool enabled)
{
    if (native_enabled_ == enabled) { return; }
    ++interruption_;
    native_enabled_ = enabled;
    native_candidate_.reset();
    neutral_ = {};
    if (!enabled) { boundary_.native_keyboard_session_ = false; }
    if (!enabled && native_session_) { dismiss(L"Keyboard in native mode disabled"); }
}

void KeyboardDelivery::set_voice(VoiceBoundary* voice)
{
    cancel_voice();
    voice_ = voice;
}

void KeyboardDelivery::cancel_voice()
{
    if (prediction_appending_ && delivery_.pending_edits()) {
        delivery_.stop(L"Word completion cancelled; remaining suffix not sent");
        if (visible()) { phase_ = DesktopKeyboardPhase::Paused; }
    }
    prediction_.clear();
    prediction_neutral_ = prediction_appending_ = false;
    prediction_trusted_ = prediction_ready_ = false;
    prediction_caret_.reset();
    prediction_trust_started_.reset();
    prediction_caret_wait_.reset();
    boundary_.prediction_closed();
    ++voice_generation_;
    if (voice_) { voice_->cancel(); }
    if (!dictation_.empty() && delivery_.pending_edits()) {
        delivery_.stop(L"Dictation cancelled; remaining characters not sent");
        if (visible()) { phase_ = DesktopKeyboardPhase::Paused; }
    }
    voice_status_ = {};
    dictation_.clear();
    dictation_offset_ = 0;
    voice_released_ = false;
}

bool KeyboardDelivery::voice_step(const Snapshot& snapshot, Tick now)
{
    const auto& pad = snapshot.slots[slot_];
    const bool held = (pad.buttons & voice_button) != 0;
    const bool pressed = held && !(voice_buttons_ & voice_button);
    if (held) {
        prediction_.clear();
        prediction_neutral_ = prediction_trusted_ = prediction_ready_ = false;
        prediction_caret_.reset();
        prediction_trust_started_.reset();
    }
    voice_buttons_ = pad.buttons;
    const bool active = voice_status_.phase == VoiceBackendPhase::Loading || voice_status_.phase == VoiceBackendPhase::Listening
        || voice_status_.phase == VoiceBackendPhase::Transcribing || voice_status_.phase == VoiceBackendPhase::Ready;
    if (!voice_) { return false; }
    if (!active) {
        if (!pressed) { return false; }
        auto rest = pad;
        rest.buttons &= ~voice_button;
        if (!keyboard_neutral(rest) || delivery_.pending_edits() || delivery_.owned()) {
            voice_status_ = {0, VoiceBackendPhase::Error, 0, {}, L"Release other controls before push-to-talk"};
            return true;
        }
        const auto original_interruption = interruption_;
        const auto original_voice_generation = voice_generation_;
        const auto target = boundary_.observe_text(delivery_.owned_key());
        const auto dispatch_now = boundary_.text_now();
        if (original_interruption != interruption_ || original_voice_generation != voice_generation_ || !voice_
            || !visible() || !text_eligible(target) || target.target != target_
            || dispatch_now < snapshot.sampled_at || dispatch_now - snapshot.sampled_at > voice_capture_lease_ms) {
            dismiss(L"Voice authorization expired or target changed");
            return true;
        }
        ++voice_generation_;
        voice_deadline_ = now + dictation_intent_ms;
        voice_released_ = false;
        keyboard_.suspend();
        voice_status_ = {voice_generation_, VoiceBackendPhase::Loading, 0, {}, {}};
        voice_->begin(voice_generation_, snapshot.sampled_at + voice_capture_lease_ms);
        return true;
    }
    if (now > voice_deadline_) {
        cancel_voice();
        voice_status_.phase = VoiceBackendPhase::Error;
        voice_status_.error = L"Dictation intent expired; no remaining text sent";
        keyboard_.suspend();
        return true;
    }
    if (pad.buttons & 0x0010) {
        cancel_voice();
        keyboard_.suspend(); // Menu during dictation is consumed, never a target Enter.
        return true;
    }
    const auto authorized_now = boundary_.text_now();
    if (authorized_now < snapshot.sampled_at || authorized_now - snapshot.sampled_at > voice_capture_lease_ms) {
        dismiss(L"Voice authorization expired");
        return true;
    }
    if (held && !voice_released_) { voice_->renew(voice_generation_, snapshot.sampled_at + voice_capture_lease_ms); }
    if (!held && !voice_released_) {
        voice_released_ = true;
        voice_->finish(voice_generation_);
    }
    if (voice_status_.phase != VoiceBackendPhase::Ready) {
        auto update = voice_->poll();
        if (update.generation == voice_generation_) {
            if (update.phase == VoiceBackendPhase::Error) {
                voice_status_ = std::move(update);
                voice_status_.text.clear();
                voice_->cancel();
                keyboard_.suspend();
                return true;
            }
            if (update.phase == VoiceBackendPhase::Ready) {
                if (!voice_released_ || update.text.empty() || update.text.size() > voice_max_characters
                    || !std::all_of(update.text.begin(), update.text.end(), [](char32_t c) {
                        return edit_events({KeyboardEditKind::Character, c}).has_value();
                    })) {
                    cancel_voice();
                    voice_status_.phase = VoiceBackendPhase::Error;
                    voice_status_.error = L"Recognition returned invalid, empty or excessive text";
                    keyboard_.suspend();
                    return true;
                }
                dictation_ = std::move(update.text);
                dictation_offset_ = 0;
            }
            if (update.phase != VoiceBackendPhase::Idle) {
                update.text.clear();
                voice_status_ = std::move(update);
            }
        }
    }
    if (voice_status_.phase == VoiceBackendPhase::Ready) {
        if (!keyboard_neutral(pad)) {
            cancel_voice();
            voice_status_.phase = VoiceBackendPhase::Error;
            voice_status_.error = L"Controller activity cancelled remaining dictation";
            keyboard_.suspend();
            return true;
        }
        // Dictation is one explicit, bounded multi-frame intent, not old controller
        // samples restamped as fresh: every chunk uses this actual new sample and
        // the original target is checked by step() and again per emitted event.
        if (!delivery_.pending_edits()) {
            for (unsigned i = 0; i < 4 && dictation_offset_ < dictation_.size(); ++i) {
                if (!delivery_.enqueue({KeyboardEditKind::Character, dictation_[dictation_offset_++]},
                    snapshot.sampled_at, snapshot.sampled_at + fresh_ms)) {
                    block(delivery_.reason());
                    return true;
                }
            }
        }
        delivery_.pump();
        if (delivery_.phase() == TextPhase::Stopped) {
            if (delivery_.context_lost()) { dismiss(delivery_.reason()); }
            else { block(delivery_.reason()); }
            return true;
        }
        if (dictation_offset_ == dictation_.size() && !delivery_.pending_edits()) {
            cancel_voice();
            keyboard_.suspend();
        }
    }
    return true;
}

PredictionView KeyboardDelivery::prediction_view() const
{
    if (!boundary_.prediction_enabled() || !accepts_typing() || delivery_.pending_edits()
        || voice_status_.phase != VoiceBackendPhase::Idle
        || keyboard_.flick_stage() != KeyboardFlickStage::Group) { return {}; }
    if (!boundary_.prediction_preview() && (!prediction_trusted_ || !prediction_ready_
        || boundary_.text_now() < prediction_proof_time_ || boundary_.text_now() - prediction_proof_time_ > 100)) { return {}; }
    return prediction_.view();
}

std::wstring_view KeyboardDelivery::prediction_hint() const
{
    if (!boundary_.prediction_enabled()) { return {}; }
    if (boundary_.prediction_preview()) { return L"LOCAL PRACTICE - no external output"; }
    if (voice_status_.phase != VoiceBackendPhase::Idle) { return L"Word suggestions paused for voice"; }
    if (prediction_support_ == PredictionSupport::Protected) { return L"Word suggestions off: protected field"; }
    if (prediction_support_ == PredictionSupport::Unsupported) { return L"Word suggestions: classic multiline Edit only"; }
    if (prediction_support_ != PredictionSupport::PlainEdit) { return L"Word suggestions: field metadata unavailable"; }
    if (!prediction_trusted_) { return L"Hold D-pad Down: trust this plain-text field"; }
    if (!prediction_ready_) { return L"Word suggestions: waiting for caret confirmation"; }
    return L"Local word suggestions on - type two letters";
}

void KeyboardDelivery::prediction_observe(const TextObservation& observation, Tick now)
{
    if (boundary_.prediction_preview()) { return; }
    delivery_.observe_prediction_caret(observation, now);
    prediction_support_ = observation.prediction.support;
    prediction_ready_ = false;
    if (!boundary_.prediction_enabled()) { return; }
    if (prediction_support_ != PredictionSupport::PlainEdit) {
        prediction_.clear();
        prediction_trusted_ = false;
        prediction_trust_started_.reset();
        prediction_caret_.reset();
        prediction_caret_wait_.reset();
        return;
    }
    if (!prediction_proven(observation, now)) {
        if (!prediction_caret_wait_) {
            prediction_.clear(true);
            prediction_caret_.reset();
        }
        if (observation.prediction.sampled_at && observation.prediction.sampled_at <= now
            && now - observation.prediction.sampled_at <= 100
            && observation.prediction.revision == observation.text_revision && !observation.prediction.caret) {
            prediction_.clear();
            prediction_caret_.reset();
            prediction_caret_wait_.reset();
        }
        if (prediction_caret_wait_ && now - *prediction_caret_wait_ > prediction_confirmation_ms) {
            prediction_.clear();
            prediction_caret_.reset();
        }
        return;
    }
    prediction_proof_time_ = observation.prediction.sampled_at;
    if (!prediction_trusted_) {
        prediction_caret_ = observation.prediction.caret;
        prediction_ready_ = true;
        return;
    }
    if (prediction_caret_ != observation.prediction.caret) {
        if (prediction_caret_wait_ && now >= *prediction_caret_wait_
            && now - *prediction_caret_wait_ <= prediction_confirmation_ms) { return; }
        prediction_.clear(true);
        prediction_caret_ = observation.prediction.caret;
    }
    prediction_caret_wait_.reset();
    prediction_ready_ = true;
}

void KeyboardDelivery::prediction_complete(KeyboardEdit edit, Tick now)
{
    if (boundary_.prediction_preview()) {
        if (!prediction_appending_) { prediction_.completed(edit); }
        return;
    }
    if (!prediction_trusted_ || prediction_support_ != PredictionSupport::PlainEdit || !prediction_caret_) { return; }
    if (edit.kind == KeyboardEditKind::Backspace) {
        if (!*prediction_caret_) { prediction_.clear(); }
        else { --*prediction_caret_; }
    } else {
        const unsigned units = edit.kind == KeyboardEditKind::Enter || edit.character > 0xffff ? 2 : 1;
        if (*prediction_caret_ > 65534 - units) {
            prediction_.clear(); prediction_caret_.reset(); return;
        }
        *prediction_caret_ += units;
    }
    if (!prediction_appending_) {
        prediction_.completed(edit);
        if (edit.kind == KeyboardEditKind::Character
            && std::wstring_view(L".,!?;:-'\"/()[]{}").find(static_cast<wchar_t>(edit.character)) != std::wstring_view::npos) {
            prediction_.clear(true);
        }
    }
    if (!prediction_caret_wait_) { prediction_caret_wait_ = now; }
    prediction_ready_ = false;
}

bool KeyboardDelivery::prediction_step(const Snapshot& snapshot)
{
    if (!boundary_.prediction_enabled()) { return false; }
    const auto& pad = snapshot.slots[slot_];
    constexpr std::uint16_t dpad = 0x000f;
    const bool armed = prediction_neutral_;
    prediction_neutral_ = keyboard_neutral(pad);
    if (!boundary_.prediction_preview() && !prediction_trusted_) {
        auto rest = pad;
        rest.buttons &= ~0x0002;
        if (pad.buttons == 0x0002 && keyboard_neutral(rest) && accepts_typing()
            && keyboard_.flick_stage() == KeyboardFlickStage::Group && prediction_ready_ && !delivery_.pending_edits()) {
            if (armed && !prediction_trust_started_) { prediction_trust_started_ = snapshot.sampled_at; }
            if (prediction_trust_started_ && snapshot.sampled_at - *prediction_trust_started_ >= 500) {
                prediction_trusted_ = true;
                prediction_.clear(true);
                prediction_trust_started_.reset();
            }
            return true;
        }
        prediction_trust_started_.reset();
    }
    if (!(pad.buttons & dpad)) { return false; }
    auto rest = pad;
    rest.buttons &= ~dpad;
    const bool isolated = keyboard_neutral(rest)
        && (pad.buttons == 0x0001 || pad.buttons == 0x0004 || pad.buttons == 0x0008);
    // Never complete a pending one-handed recenter, Menu release, or face edge as
    // a side effect of choosing a suggestion. Existing edits win mixed samples.
    if (!isolated || !armed || keyboard_.flick_stage() != KeyboardFlickStage::Group
        || !accepts_typing()) { return false; }
    const auto view = prediction_view();
    if (!view.count) { return false; }
    if (pad.buttons == 0x0004 || pad.buttons == 0x0008) {
        prediction_.select(pad.buttons == 0x0008);
        return true;
    }
    const auto suffix = prediction_.accept(view.generation);
    if (!suffix) { block(L"Suggestion expired; no completion sent"); return true; }
    if (delivery_.pending_edits() || suffix->size() > keyboard_edit_queue_limit) {
        block(L"Suggestion queue unavailable; no completion sent");
        return true;
    }
    prediction_appending_ = true;
    if (!boundary_.prediction_preview() && (!prediction_caret_ || *prediction_caret_ > 65534 - suffix->size())) {
        block(L"Word completion exceeds supported caret range; no suffix sent");
        return true;
    }
    auto caret = prediction_caret_;
    for (auto c : *suffix) {
        if (!delivery_.enqueue({KeyboardEditKind::Character, c}, snapshot.sampled_at,
            snapshot.sampled_at + (boundary_.prediction_preview() ? fresh_ms : 1000),
            boundary_.prediction_preview() ? std::nullopt : caret)) {
            block(delivery_.reason());
            return true;
        }
        if (caret) { ++*caret; }
    }
    keyboard_.suspend();
    return true;
}

std::wstring KeyboardDelivery::status() const
{
    if (delivery_.recovery_required()) { return L"Key release needs recovery. View closes; use Retry release."; }
    if (!reason_.empty()) { return reason_; }
    switch (voice_status_.phase) {
    case VoiceBackendPhase::Loading: return L"Loading local voice - keep holding right-stick click";
    case VoiceBackendPhase::Listening: return L"Listening locally - release to transcribe; View cancels";
    case VoiceBackendPhase::Transcribing: return L"Transcribing locally - View cancels";
    case VoiceBackendPhase::Ready: return L"Inserting dictation - View stops remaining text";
    case VoiceBackendPhase::Error: return voice_status_.error;
    case VoiceBackendPhase::Idle: break;
    }
    if (phase_ == DesktopKeyboardPhase::Composing) {
        if (opening_chord_) { return L"Release View and Y to type"; }
        if (keyboard_.phase() == KeyboardPhase::Waiting) { return L"Release Y to type"; }
        return native_session_ ? L"Typing - controller input still reaches the game; View closes"
            : L"Typing - Menu Enter; View closes";
    }
    return {};
}
} // namespace gamepad
