#include "Audio.h"
#include "KeyboardDelivery.h"
#include "VoiceViewAdapter.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace overworld;
namespace {
void check(bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } }
struct FakeVoice final : VoiceBoundary {
    unsigned starts = 0, finishes = 0, cancels = 0, renewals = 0;
    std::uint64_t generation = 0;
    VoiceUpdate update;
    Tick expires = 0;
    Tick renewal_time = 0;
    std::function<Tick()> clock;
    bool cancelled = false, released = false;
    void begin(std::uint64_t value, Tick deadline) override {
        ++starts; generation = value; expires = deadline; cancelled = released = false;
        update = {value, VoiceBackendPhase::Loading};
    }
    void renew(std::uint64_t value, Tick deadline) override {
        if (value != generation || cancelled || released) { return; }
        const auto previous = expires;
        const auto now = clock ? clock() : renewal_time;
        if (!voice_capture_renewable(previous, deadline, now)) {
            cancelled = true; ++cancels;
            update = {generation, VoiceBackendPhase::Error, 0, {}, L"Microphone authorization expired"};
            return;
        }
        ++renewals; expires = deadline;
    }
    void finish(std::uint64_t value) override { check(value == generation, "finish generation"); ++finishes; released = true; }
    void cancel() override { ++cancels; cancelled = true; update = {}; }
    VoiceUpdate poll() override { return update; }
    void result(std::u32string text) { update = {generation, VoiceBackendPhase::Ready, 0, std::move(text), {}}; }
};
struct FakeText final : TextBoundary {
    TextObservation observation;
    Tick time = 1000;
    std::u16string typed;
    std::vector<TextEvent> sent;
    std::deque<unsigned> acceptance;
    std::function<void()> during_send;
    std::function<void()> during_observe;
    FakeText() {
        observation.allowed = observation.cleanup_allowed = observation.stable = true;
        observation.physical_held = false;
        observation.target.windows = {{{10, 1, 2}, {10, 1, 2}, {11, 1, 2}}};
    }
    TextObservation observe_text(std::optional<TextKey>) override { if (during_observe) { during_observe(); } return observation; }
    Tick text_now() const override { return time; }
    unsigned send_text(TextEvent event) override {
        const auto count = acceptance.empty() ? 1u : acceptance.front();
        if (!acceptance.empty()) { acceptance.pop_front(); }
        sent.push_back(event);
        if (count == 1 && !event.up) {
            check(event.key.kind == TextKeyKind::Unicode, "dictation must never emit Enter or commands");
            typed.push_back(event.key.unit);
        }
        if (during_send) { during_send(); }
        return count;
    }
};
struct Fixture {
    FakeText text;
    FakeVoice voice;
    KeyboardDelivery keyboard{text};
    Snapshot snapshot;
    KeyboardEligibility eligibility{true, {}};
    Fixture() {
        snapshot.slots[0].connection = Connection::Connected;
        snapshot.slots[0].connection_epoch = 1;
        voice.clock = [&] { return text.time; };
        keyboard.set_voice(&voice);
    }
    Controller& pad() { return snapshot.slots[0]; }
    void tick(Tick elapsed = 16) {
        text.time += elapsed; snapshot.sampled_at = text.time; ++snapshot.sequence;
        keyboard.step(snapshot, text.time, eligibility);
    }
    void buttons(std::uint16_t buttons) { pad().buttons = buttons; tick(); }
    void open() { tick(); tick(); buttons(0x8000); buttons(0); check(keyboard.visible(), "keyboard opens"); }
    void start() { open(); buttons(0x0080); check(voice.starts == 1, "fresh RS starts one job"); }
    void recognized(std::u32string transcript) { buttons(0); voice.result(std::move(transcript)); tick(); }
};
}
int main()
{
    unsigned count = 0;
    const auto test = [&](const char* name, const std::function<void()>& body) { body(); ++count; std::cout << "PASS " << name << '\n'; };
    try {
        test("expired capture cannot revive from a still-fresh controller sample during model loading", [] {
            FakeVoice worker;
            worker.begin(7, 1100);
            worker.renewal_time = 1120;
            constexpr Tick sampled_at = 1080;
            check(worker.renewal_time - sampled_at == 40
                && voice_capture_authorized(sampled_at + voice_capture_lease_ms, worker.renewal_time),
                "new sample alone is fresh in the reported race");
            worker.renew(7, sampled_at + voice_capture_lease_ms);
            check(worker.cancelled && worker.expires == 1100 && worker.renewals == 0
                && worker.update.phase == VoiceBackendPhase::Error, "old expired lease must cancel rather than become 1180");
            check(!voice_capture_authorized(worker.expires, 1130), "model completion at 1130 cannot start capture");
            worker.renewal_time = 1130; worker.renew(7, 1230);
            check(worker.cancelled && worker.expires == 1100 && worker.renewals == 0, "later fresh renewal cannot revive cancelled generation");

            Fixture f; f.start();
            const auto previous = f.voice.expires;
            f.text.during_observe = [&] { f.text.time += 40; };
            f.tick(80); // Sample gap 80, validation finishes after the old 100ms authorization.
            check(f.voice.cancelled && f.voice.expires == previous && f.voice.renewals == 0
                && f.keyboard.voice_status().phase == VoiceBackendPhase::Error && f.text.sent.empty(),
                "actual controller routing must expose renewal failure without output or capture revival");
        });
        test("capture renewal validates both deadlines and preserves boundaries and generation isolation", [] {
            check(voice_capture_renewable(1100, 1200, 1100), "inclusive expiry boundary can renew on time");
            check(!voice_capture_renewable(1100, 1201, 1101), "one millisecond expiry cannot be renewed");
            check(!voice_capture_renewable(1100, 0, 1050), "missing replacement rejected");
            check(!voice_capture_renewable(1100, 1049, 1050), "expired replacement rejected");
            check(!voice_capture_renewable(1100, 1151, 1050), "replacement cannot exceed fixed 100ms lease");
            check(!voice_capture_renewable(1100, 1099, 1050), "reordered replacement cannot shorten or rewind authorization");
            check(!voice_capture_renewable(1100, 1100, 999), "clock rollback remains rejected");
            FakeVoice worker; worker.begin(9, 1100); worker.renewal_time = 1080;
            worker.renew(8, 1180);
            check(!worker.cancelled && worker.expires == 1100 && worker.renewals == 0, "old generation cannot modify current lease");
            worker.renew(9, 1180);
            check(!worker.cancelled && worker.expires == 1180 && worker.renewals == 1, "matching live generation renews");
            worker.finish(9); worker.renewal_time = 1100; worker.renew(9, 1200);
            check(worker.released && worker.expires == 1180 && worker.renewals == 1, "released capture cannot renew");
            worker.begin(10, 1200); worker.renewal_time = 1120; worker.renew(9, 1220);
            check(worker.expires == 1200 && !worker.cancelled, "late cancelled generation cannot affect a new explicit job");
            worker.renew(10, 1220);
            check(worker.expires == 1220 && !worker.cancelled, "new explicit generation remains usable");
            worker.renew(10, 1221); // Same time 1120: invalid replacement is now 101ms ahead.
            check(worker.cancelled && worker.expires == 1220 && worker.update.phase == VoiceBackendPhase::Error,
                "invalid replacement permanently cancels instead of extending authorization");
            worker.renewal_time = 1121; worker.renew(10, 1221);
            check(worker.cancelled && worker.expires == 1220, "a later-valid replacement cannot revive the cancelled generation");
        });
        test("voice dispatch rechecks time and target after native observation", [] {
            Fixture f; f.open();
            f.text.during_observe = [&] { f.text.time += 101; };
            f.buttons(0x0080);
            check(f.voice.starts == 0 && !f.keyboard.visible(), "101 ms observation delay cannot authorize microphone startup");
            Fixture g; g.open();
            unsigned observations = 0;
            g.text.during_observe = [&] { if (++observations == 2) { ++g.text.observation.target.focus_revision; } };
            g.buttons(0x0080);
            check(g.voice.starts == 0, "target must be revalidated immediately before voice dispatch");
        });
        test("capture lease bounds deferred startup and recording independently of UI cancellation", [] {
            check(!voice_capture_authorized(0, 0), "missing lease is never authorization");
            check(voice_capture_authorized(1100, 1000) && voice_capture_authorized(1100, 1100), "100ms lease boundary");
            check(!voice_capture_authorized(1100, 1101), "deferred model load cannot start microphone after expiry");
            check(!voice_capture_authorized(1100, 999), "future/other-clock lease cannot authorize");
            Fixture f; f.start();
            const auto initial = f.voice.expires;
            f.tick(50);
            check(f.voice.renewals == 1 && f.voice.expires > initial, "fresh eligible held RS renews the same capture intent");
            const auto renewed = f.voice.expires;
            const auto renewals = f.voice.renewals;
            f.keyboard.step(f.snapshot, f.text.time, f.eligibility);
            check(f.voice.renewals == renewals, "duplicate snapshot must not renew capture");
            check(!voice_capture_authorized(renewed, renewed + 1), "UI stall expires worker capture even without a cancel callback");
            f.text.during_observe = [&] { f.text.time += 101; };
            f.tick();
            check(f.voice.renewals == renewals && f.keyboard.voice_status().phase == VoiceBackendPhase::Idle,
                "slow validation cannot refresh expired capture evidence");
        });
        test("release stops capture renewal but a valid utterance may finish offline", [] {
            Fixture f; f.start(); f.tick(); const auto renewals = f.voice.renewals;
            f.buttons(0);
            check(f.voice.finishes == 1 && f.voice.renewals == renewals, "release sends finish without renewing microphone lease");
            for (unsigned i = 0; i < 30; ++i) { f.tick(); }
            check(!voice_capture_authorized(f.voice.expires, f.text.time), "capture authorization intentionally expires during inference");
            f.voice.result(U"finished speech"); f.tick();
            for (unsigned i = 0; i < 10; ++i) { f.tick(); }
            check(f.text.typed == u"finished speech", "released utterance retains separate original-target dictation intent");
        });
        test("presentation distinguishes stages and never exposes transcript content", [] {
            VoiceUpdate state;
            state.text = U"private dictated words";
            check(voice_presentation(state).phase == VoicePhase::Idle && voice_presentation(state).hint.empty(),
                "idle view stays empty");
            state.phase = VoiceBackendPhase::Loading;
            check(voice_presentation(state).phase == VoicePhase::Loading, "loading cannot masquerade as listening");
            state.phase = VoiceBackendPhase::Listening; state.level = std::numeric_limits<float>::infinity();
            check(voice_presentation(state).level == 0, "nonfinite level must be safe");
            state.level = 2;
            check(voice_presentation(state).level == 1, "normalized meter clamps");
            state.phase = VoiceBackendPhase::Ready;
            check(voice_presentation(state).phase == VoicePhase::Transcribing
                && voice_presentation(state).hint == L"Inserting dictation", "ready text is a private bounded insertion phase");
            state.phase = VoiceBackendPhase::Error; state.error = std::wstring(200, L'e');
            check(voice_presentation(state).phase == VoicePhase::Error && voice_presentation(state).hint.size() == 120,
                "visible error is bounded");
        });
        test("real capture PCM and float frames decode to bounded mono without microphone", [] {
            float mono = 0;
            check(decode_audio_frame(std::vector<std::uint8_t>{0, 0x80, 0, 0x40}, 2, 2, false, mono)
                && mono == -0.25f, "signed PCM16 stereo downmix");
            check(decode_audio_frame(std::vector<std::uint8_t>{0, 0, 0x80}, 1, 3, false, mono)
                && mono == -1.0f, "signed PCM24 minimum");
            check(decode_audio_frame(std::vector<std::uint8_t>{0, 0, 0, 0x80}, 1, 4, false, mono)
                && mono == -1.0f, "signed PCM32 minimum");
            const float floats[]{0.5f, -0.25f};
            std::vector<std::uint8_t> frame(sizeof(floats)); std::memcpy(frame.data(), floats, sizeof(floats));
            check(decode_audio_frame(frame, 2, 4, true, mono) && mono == 0.125f, "shared-mode float stereo downmix");
            const float invalid = std::numeric_limits<float>::infinity();
            std::memcpy(frame.data(), &invalid, sizeof(invalid));
            check(!decode_audio_frame(frame, 2, 4, true, mono), "invalid float must not reach inference");
            check(!decode_audio_frame({}, 0, 4, true, mono)
                && !decode_audio_frame(frame, 3, 4, true, mono), "invalid/short frames rejected");
        });
        test("UTF8 dictation is normalized to printable characters without commands", [] {
            std::u32string text; std::wstring error;
            check(normalize_dictation("  hello\r\nworld\t \xce\xa9 \xf0\x9f\x98\x80  ", text, error)
                && text == U"hello world \u03a9 \U0001f600", "UTF8 scalar decode and whitespace normalization");
            for (const std::string invalid : {std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3),
                std::string("\xf4\x90\x80\x80", 4), std::string("a\0b", 3), std::string("\x1b", 1), std::string(1025, 'a')}) {
                check(!normalize_dictation(invalid, text, error) && text.empty() && !error.empty(), "invalid text fails explicitly/atomically");
            }
            check(!normalize_dictation(" \t\r\n", text, error), "silence/empty text isn't success");
        });
        test("resampling preserves packet-independent duration and bounds", [] {
            AudioResampler resampler(48000);
            std::vector<float> output;
            for (unsigned i = 0; i < 48000; ++i) { check(resampler.append(0.5f, output), "valid frame"); }
            check(output.size() == 16000 && output.front() == 0.5f && output.back() == 0.5f, "48k mono resamples exactly one second");
            check(voice_has_signal(output), "audible signal accepted");
            AudioResampler invalid(0); check(!invalid.append(0, output), "invalid source rate");
            check(!resampler.append(std::numeric_limits<float>::quiet_NaN(), output), "nonfinite sample rejected");
            output.assign(voice_max_samples, 0);
            AudioResampler full(16000); check(!full.append(0, output), "audio limit enforced without growth");
            check(!voice_has_signal(output), "silence rejected");
        });
        test("fixture parser rejects malformed or nonbounded WAV", [] {
            std::vector<float> samples; std::wstring error;
            for (const std::vector<std::uint8_t> bytes : {std::vector<std::uint8_t>{}, std::vector<std::uint8_t>(44, 0),
                std::vector<std::uint8_t>(voice_max_samples * 2 + 65537, 0)}) {
                check(!decode_voice_wav(bytes, samples, error) && !error.empty(), "invalid fixture shape rejected");
            }
        });
        test("microphone job never begins before fresh RS in open keyboard", [] {
            Fixture f;
            f.buttons(0x0080);
            check(f.voice.starts == 0, "closed keyboard can't capture");
            f.buttons(0); f.open();
            check(f.voice.starts == 0, "opening and settings attachment cannot capture");
            f.buttons(0x0080); f.tick(); f.tick();
            check(f.voice.starts == 1 && f.text.sent.empty(), "held gesture starts once and never types");
            f.buttons(0);
            check(f.voice.finishes == 1, "release submits audio once");
        });
        test("voice loading listening and transcribing are distinct non-text states", [] {
            Fixture f; f.start();
            check(f.keyboard.voice_status().phase == VoiceBackendPhase::Loading, "loading feedback");
            f.voice.update = {f.voice.generation, VoiceBackendPhase::Listening, 0.5f}; f.tick();
            check(f.keyboard.voice_status().phase == VoiceBackendPhase::Listening && f.keyboard.voice_status().level == 0.5f, "level feedback");
            f.buttons(0); f.voice.update = {f.voice.generation, VoiceBackendPhase::Transcribing}; f.tick();
            check(f.keyboard.voice_status().phase == VoiceBackendPhase::Transcribing && f.text.sent.empty(), "transcription no provisional output");
        });
        test("dictation beyond queue size streams in bounded fresh chunks", [] {
            Fixture f; f.start(); f.recognized(std::u32string(130, U'a'));
            check(f.text.typed.size() == 4, "same-step result starts with bounded four characters");
            for (unsigned i = 0; i < 40; ++i) { f.tick(); }
            check(f.text.typed == std::u16string(130, u'a') && f.keyboard.voice_status().phase == VoiceBackendPhase::Idle,
                "long dictation delivered once without 32-operation overflow");
            const auto count = f.text.sent.size(); f.tick();
            check(f.text.sent.size() == count, "completed result cannot replay");
        });
        test("stale returned generation never types into a later session", [] {
            Fixture f; f.start(); const auto old = f.voice.generation;
            f.keyboard.dismiss(); f.buttons(0); f.tick(); f.buttons(0x8000); f.buttons(0);
            f.voice.update = {old, VoiceBackendPhase::Ready, 0, U"old target", {}};
            f.tick();
            check(f.text.sent.empty(), "late cancelled result ignored");
            f.buttons(0x0080); f.buttons(0);
            f.voice.update = {old, VoiceBackendPhase::Ready, 0, U"old generation", {}}; f.tick();
            check(f.text.sent.empty(), "old generation cannot contaminate new capture");
        });
        test("physical focus session device and manual-mode loss cancel capture", [] {
            for (unsigned gate = 0; gate < 6; ++gate) {
                Fixture f; f.start(); const auto generation = f.voice.generation;
                if (gate == 0) { ++f.text.observation.target.activity; }
                if (gate == 1) { ++f.text.observation.target.windows[2].window; }
                if (gate == 2) { ++f.text.observation.target.context; }
                if (gate == 3) { f.pad().connection = Connection::Disconnected; }
                if (gate == 4) { f.eligibility.desktop = false; }
                if (gate == 5) { f.text.observation.allowed = false; }
                const auto cancelled = f.voice.cancels; f.tick();
                check(f.voice.cancels > cancelled, "context loss must cancel backend immediately on observation");
                f.voice.update = {generation, VoiceBackendPhase::Ready, 0, U"do not send", {}}; f.tick();
                check(f.text.sent.empty(), "lost target cannot receive stale dictation");
            }
        });
        test("View Menu and mode chord cancel without stray Enter or characters", [] {
            for (const std::uint16_t buttons : {std::uint16_t{0x0020}, std::uint16_t{0x0030}, std::uint16_t{0x0010}}) {
                Fixture f; f.start(); const auto cancelled = f.voice.cancels;
                f.buttons(buttons); f.buttons(0); f.tick();
                check(f.voice.cancels > cancelled && f.text.sent.empty(), "cancel control must not emit model edits");
            }
        });
        test("held opening RS and simultaneous faces cannot start capture", [] {
            Fixture f; f.tick(); f.tick(); f.buttons(0x8080); f.buttons(0x0080);
            check(f.voice.starts == 0, "unsafe opening gesture not capture");
            Fixture g; g.open(); g.buttons(0x1080);
            check(g.voice.starts == 0 && g.text.sent.empty(), "RS+A is consumed, never capture or text");
        });
        test("speech error silence excessive and invalid results are visible", [] {
            for (const auto& text : {std::u32string{}, std::u32string(1025, U'a'), std::u32string{U'\n'}, std::u32string{0xd800}}) {
                Fixture f; f.start(); f.recognized(text);
                check(f.text.sent.empty() && f.keyboard.voice_status().phase == VoiceBackendPhase::Error, "invalid speech result not success");
            }
            Fixture f; f.start(); f.voice.update = {f.voice.generation, VoiceBackendPhase::Error, 0, {}, L"Microphone unavailable"}; f.tick();
            check(f.keyboard.voice_status().error == L"Microphone unavailable" && f.text.sent.empty(), "backend error preserved without transcript");
        });
        test("disabled voice detaches and cancels ongoing microphone job", [] {
            Fixture f; f.start(); const auto cancelled = f.voice.cancels;
            f.keyboard.set_voice(nullptr);
            check(f.voice.cancels > cancelled && f.keyboard.voice_status().phase == VoiceBackendPhase::Idle, "disable cancels");
            f.buttons(0); f.buttons(0x0080); check(f.voice.starts == 1, "disabled cannot restart");
        });
        test("a stale actual controller sample cannot authorize dictation delivery", [] {
            Fixture f; f.start(); f.buttons(0);
            f.voice.result(U"stale");
            f.text.time += 101;
            f.keyboard.step(f.snapshot, f.text.time, f.eligibility);
            check(f.text.sent.empty() && f.keyboard.voice_status().phase == VoiceBackendPhase::Idle, "stale current control cancels instead of restamping");
        });
        test("partial speech output never retries and preserves pending key-up", [] {
            Fixture f; f.start(); f.text.acceptance = {1, 0}; f.recognized(U"abcdef");
            check(f.text.typed == u"a" && f.keyboard.recovery_required(), "partial failure retains exact accepted down");
            const auto before = f.text.sent.size();
            f.tick(); f.tick();
            check(f.text.sent.size() <= before + 1 && f.text.typed == u"a", "only pending up may be retried");
            check(f.keyboard.voice_status().phase == VoiceBackendPhase::Idle, "failed output cancels recognition state");
        });
        test("focus switch during speech chunk cancels all remaining characters", [] {
            Fixture f; f.start();
            f.text.during_send = [&] { ++f.text.observation.target.focus_revision; };
            f.recognized(U"do not redirect");
            check(f.text.typed.size() == 1 && !f.keyboard.visible(), "per-event target revalidation stops same chunk");
        });
        test("cancel during queued settling clears dictation without delayed replay", [] {
            Fixture f; f.start(); f.text.observation.settling = true; f.recognized(U"queued");
            check(f.text.sent.empty() && f.keyboard.delivery().pending_edits(), "settling queues bounded chunk");
            f.buttons(0x0010); f.text.observation.settling = false; f.buttons(0); f.tick();
            check(f.text.sent.empty() && f.keyboard.delivery().pending_edits() == 0, "Menu cancels pending dictation queue");
        });
        test("bounded dictation intent expires while controller remains fresh", [] {
            Fixture f; f.start(); f.buttons(0);
            for (unsigned i = 0; i < 901; ++i) { f.tick(100); }
            check(f.keyboard.voice_status().phase == VoiceBackendPhase::Error && f.text.sent.empty(), "overall intent deadline enforced without stale data");
        });
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
    std::cout << count << " pure voice cases passed; no native capture linked\n";
}
