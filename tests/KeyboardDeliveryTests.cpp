#include "KeyboardDelivery.h"
#include "Desktop.h"
#include "PredictionPractice.h"
#include "KeyboardPreviewInput.h"
#include "KeyboardRouting.h"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <set>
#include <vector>

using namespace gamepad;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
constexpr KeyboardEdit letter(char32_t value) { return {KeyboardEditKind::Character, value}; }
constexpr KeyboardEdit backspace{KeyboardEditKind::Backspace};
constexpr KeyboardEdit enter{KeyboardEditKind::Enter};

struct Fake final : TextBoundary {
    Tick now = 1000;
    TextObservation observation;
    std::vector<TextEvent> attempted, accepted;
    std::vector<std::optional<TextKey>> owners;
    std::deque<unsigned> returns;
    std::function<void()> during_observe;
    std::function<void(TextEvent)> during_send;
    std::wstring field;
    std::optional<gamepad::live::Observation> route;
    bool preview = false;
    Tick application_delay = 0;
    std::deque<std::pair<Tick, TextEvent>> pending_application;
    bool prediction_preview() const override { return preview; }
    Fake()
    {
        observation.allowed = observation.cleanup_allowed = observation.stable = true;
        observation.physical_held = false;
        observation.target.windows = {{{10, 1, 2}, {10, 1, 2}, {11, 1, 2}}};
        observation.target.context = 1;
    }
    TextObservation observe_text(std::optional<TextKey> owned) override
    {
        owners.push_back(owned);
        while (!pending_application.empty() && pending_application.front().first <= now) {
            apply(pending_application.front().second);
            pending_application.pop_front();
        }
        if (during_observe) { during_observe(); }
        auto result = observation;
        if (route) {
            result.allowed = result.allowed && route->available && route->baseline_known
                && route->mode == gamepad::live::Mode::Pointer && gamepad::live::keyboard_route_allowed(*route, native_keyboard_session());
            result.physical_held = result.physical_held || route->physical_held != 0;
            result.stable = result.stable && route->routing_stable;
        }
        return result;
    }
    Tick text_now() const override { return now; }
    void apply(TextEvent event)
    {
        if (event.key.kind == TextKeyKind::Unicode) { field.push_back(static_cast<wchar_t>(event.key.unit)); }
        if (event.key.kind == TextKeyKind::Backspace && !field.empty()) { field.pop_back(); }
        if (event.key.kind == TextKeyKind::Enter) { field += L"\r\n"; }
    }
    unsigned send_text(TextEvent event) override
    {
        attempted.push_back(event);
        const unsigned count = returns.empty() ? 1 : returns.front();
        if (!returns.empty()) { returns.pop_front(); }
        if (count == 1) {
            accepted.push_back(event);
            if (!event.up) {
                if (application_delay) { pending_application.emplace_back(now + application_delay, event); }
                else { apply(event); }
            }
        }
        if (during_send) { during_send(event); }
        return count;
    }
};
struct Fixture {
    Fake fake;
    KeyboardDelivery keyboard{fake};
    Snapshot snapshot;
    KeyboardEligibility eligibility{true, {}};
    Fixture()
    {
        snapshot.slots[0].connection = Connection::Connected;
        snapshot.slots[0].connection_epoch = 1;
    }
    Controller& pad() { return snapshot.slots[0]; }
    void tick(Tick elapsed = 16)
    {
        fake.now += elapsed;
        snapshot.sampled_at = fake.now;
        ++snapshot.sequence;
        keyboard.step(snapshot, fake.now, eligibility);
    }
    void button(std::uint16_t buttons) { pad().buttons = buttons; tick(); }
    void tap(std::uint16_t buttons) { button(buttons); button(0); }
    void native_context(bool enabled = true)
    {
        eligibility = {false, {}, true};
        keyboard.set_native_mode_enabled(enabled);
    }
    void hold_native()
    {
        tick(); tick(); button(0x8020);
        for (unsigned i = 0; i < 5; ++i) { tick(100); }
    }
    // Deflects the left stick toward `degrees` (0=up, clockwise) without changing buttons or ticking.
    void aim(double degrees, double magnitude = 0.8)
    {
        const double radians = degrees * std::numbers::pi / 180.0;
        pad().left_x = static_cast<std::int16_t>(std::lround(std::sin(radians) * magnitude * 32767.0));
        pad().left_y = static_cast<std::int16_t>(std::lround(std::cos(radians) * magnitude * 32767.0));
    }
    void center_stick() { pad().left_x = pad().left_y = 0; }
    // Performs one full OneHanded two-stage flick: outward+neutral toward `group_degrees` commits
    // the 8-way group, then outward+neutral toward `face_degrees` commits the character/action.
    void flick(double group_degrees, double face_degrees)
    {
        aim(group_degrees); tick();
        center_stick(); tick();
        aim(face_degrees); tick();
        center_stick(); tick();
    }
    void open()
    {
        tick(); tick(); button(0x8000);
        check(keyboard.visible() && fake.accepted.empty(), "safe Y opens without output");
        pad().left_y = 28000;
        button(0);
        check(keyboard.accepts_typing(), "Y release permits typing with the stick already positioned");
    }
    void prediction_word(std::u32string_view word = U"he")
    {
        tick(); tick(); button(0x8000); button(0);
        center_stick(); tick(); tap(0x0200);
        for (auto c : word) {
            bool found = false;
            for (unsigned g = 0; g < 8 && !found; ++g) {
                for (unsigned face = 0; face < 4 && !found; ++face) {
                    if (keyboard_layout(KeyboardPage::Letters)[g][face] != c) { continue; }
                    if (keyboard.model().input_mode() == KeyboardInputMode::OneHanded) {
                        constexpr std::array<double, 4> directions{180, 90, 270, 0};
                        flick(g * 45.0, directions[face]);
                    } else {
                        constexpr std::array<std::uint16_t, 4> buttons{0x1000, 0x2000, 0x4000, 0x8000};
                        aim(g * 45.0); tick(); tap(buttons[face]);
                    }
                    found = true;
                }
            }
            check(found, "fixture letter must be reachable");
        }
        center_stick(); tick();
    }
    void open_prediction()
    {
        tick(); tick(); button(0x8000); button(0); tick();
    }
    void type_he()
    {
        if (keyboard.model().input_mode() == KeyboardInputMode::OneHanded) {
            flick(45, 0); flick(45, 180);
        } else {
            aim(45); tick(); tap(0x8000); tap(0x1000); center_stick(); tick();
        }
    }
};
struct CompletionFixture {
    Fake fake;
    TextDelivery delivery{fake};
    explicit CompletionFixture(Tick delay = 80, Tick intent_ms = 1000)
    {
        fake.field = L"he";
        fake.application_delay = delay;
        check(delivery.start(fake.observation.target), "start native completion fixture");
        check(delivery.enqueue(letter(U'r'), fake.now, fake.now + intent_ms, true)
            && delivery.enqueue(letter(U'e'), fake.now, fake.now + intent_ms, true), "queue literal suffix with fixed deadline");
    }
    CompletedTextEdits pump(Tick elapsed, unsigned budget = 8)
    {
        fake.now += elapsed;
        return delivery.pump(budget);
    }
};
struct PointerFake final : gamepad::live::OutputBoundary {
    gamepad::live::Observation observation;
    Tick time = 1000;
    unsigned observations = 0;
    std::function<void()> during_observe;
    std::vector<gamepad::live::Output> events;
    PointerFake()
    {
        observation.allowed = observation.available = observation.baseline_known = observation.cleanup_allowed = true;
        observation.mode = gamepad::live::Mode::Pointer;
        observation.desktop = observation.bounds = {0, 0, 1919, 1079};
        observation.context = 1;
    }
    gamepad::live::Observation observe(unsigned, const gamepad::live::Output* = nullptr) override
    {
        ++observations;
        if (during_observe) { during_observe(); }
        auto result = observation;
        result.app = observe_activation({10, 100});
        apply_auto_policy(result);
        return result;
    }
    Tick now() const override { return time; }
    unsigned send(const gamepad::live::Output& output, gamepad::live::Rect) override { events.push_back(output); return 1; }
};
}

int main()
{
    unsigned count = 0;
    const auto test = [&](const char* name, const std::function<void()>& body) {
        body(); ++count; std::cout << "PASS " << name << '\n';
    };
    try {
        test("embedded vocabulary has ten thousand ranked unique words and bounded lookup cost", [] {
            const auto vocabulary = prediction_vocabulary();
            check(vocabulary.size() == 10000, "embedded dataset count");
            std::set<std::string_view> unique;
            std::size_t bytes = vocabulary.size_bytes();
            for (auto word : vocabulary) {
                check(word.size() >= 3 && word.size() <= prediction_word_limit
                    && std::all_of(word.begin(), word.end(), [](char c) { return c >= 'a' && c <= 'z'; }),
                    "vocabulary contains only bounded lowercase ASCII words");
                check(unique.insert(word).second, "vocabulary cannot repeat words");
                bytes += word.size() + 1;
            }
            check(bytes < 300000, "static vocabulary payload and view table stay below 300 KB");
            WordPrediction prediction;
            prediction.clear(true);
            prediction.completed(letter(U'h')); prediction.completed(letter(U'e'));
            check(prediction.view().words == std::array<std::wstring, 3>{L"here", L"her", L"hey"},
                "he candidates follow source frequency rather than alphabetical order");
            prediction.clear(true);
            for (auto c : U"tha") { if (c) { prediction.completed(letter(c)); } }
            check(prediction.view().words == std::array<std::wstring, 3>{L"that", L"thank", L"than"}, "common prefix ordering");
            const auto started = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 1000; ++i) {
                prediction.clear(true);
                prediction.completed(letter(U'z')); prediction.completed(letter(U'q'));
                check(!prediction.view().count, "worst-case unmatched prefix has no suggestions");
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            check(elapsed < 2000, "1000 bounded full-list lookups must complete within two seconds");
            std::cout << "Vocabulary bytes " << bytes << "; 1000 worst-case lookups " << elapsed << " ms\n";
        });
        test("generic native prediction is enabled by default without consent or leading space", [] {
            for (auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                Fixture f; f.keyboard.set_input_mode(mode);
                f.tick(); f.tick(); f.button(0x8000); f.button(0); f.type_he();
                check(f.keyboard.prediction_enabled() && f.keyboard.prediction_view().count == 3 && f.fake.field == L"he",
                    "two accepted own letters suffice from the first session");
                Fixture g; g.keyboard.set_input_mode(mode); g.open_prediction();
                check(g.keyboard.prediction_hint() == L"Word suggestions on - Down hides",
                    "simple status without a per-field prompt");
                g.type_he();
                check(g.keyboard.prediction_view().count == 3 && g.fake.field == L"he", "two letters show native suggestions without Space");
                g.tap(0x0001);
                check(g.fake.field == L"here", "native acceptance uses the existing fake transport and suffix only");
                g.tap(0x0001); check(g.fake.field == L"here", "native completion is not replayable");
                g.tap(0x0020);
                check(!g.keyboard.prediction_view().count, "close clears the current fragment");
                g.button(0); g.tick(); g.button(0x8000); g.button(0); g.type_he();
                check(g.keyboard.prediction_view().count == 3, "every new session starts enabled without a consent gesture");
            }
        });
        test("generic browser and RichEdit-like targets share broad literal completion without class or value APIs", [] {
            for (auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                for (unsigned target = 0; target < 3; ++target) {
                    Fixture f;
                    f.keyboard.set_input_mode(mode);
                    // Only opaque native identities cross the delivery boundary;
                    // no Edit class, value length, selection or caret capability exists.
                    f.fake.observation.target.windows[2] = {100 + target, 10 + target, 20 + target};
                    const auto existing = target == 0 ? std::wstring{} : target == 1
                        ? std::wstring(L"browser content before this session: ") : std::wstring(70000, L'x');
                    f.fake.field = existing;
                    f.open_prediction(); f.type_he();
                    check(f.keyboard.prediction_view().words[0] == L"here", "own he fragment is independent of target's preceding content");
                    const auto before = f.fake.accepted.size();
                    f.tap(0x0001);
                    check(f.fake.field == existing + L"here" && f.fake.accepted.size() == before + 4,
                        "generic, browser and large RichEdit-like field all receive only the requested re suffix");
                    check(f.keyboard.model().text().empty(), "native keyboard must never retain an app-sized composition or echo sample");
                }
            }
        });
        test("quick hide is immediate for this session only and never erases text", [] {
            Fixture f; f.open_prediction(); f.type_he();
            check(f.keyboard.prediction_view().count == 3, "suggestions already enabled");
            f.button(0x0002);
            check(!f.keyboard.prediction_view().count && f.fake.field == L"he", "fresh Down hides immediately without deleting output");
            for (unsigned i = 0; i < 8; ++i) { f.tick(100); }
            check(f.keyboard.prediction_hint() == L"Word suggestions off for this session",
                "held hide button cannot automatically reenable collection");
            f.button(0); f.type_he();
            check(!f.keyboard.prediction_view().count && f.fake.field == L"hehe", "typing remains available with suggestions hidden");
            Fixture onehand; onehand.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            onehand.open_prediction(); onehand.type_he();
            onehand.aim(45); onehand.tick(); onehand.center_stick(); onehand.tick();
            onehand.button(0x0002);
            check(onehand.keyboard.prediction_hint() == L"Word suggestions off for this session"
                && onehand.fake.field == L"he", "quick hide during one-handed character stage does not commit a glyph");
            f.tap(0x0020); f.tick(); f.button(0x8000); f.button(0); f.type_he();
            check(f.keyboard.prediction_view().count == 3, "reopening restores the saved default without reenable friction");
        });
        test("recognized protection during an ordinary edit revokes hints even if the final observation is unknown", [] {
            for (unsigned guarded_observation : {2u, 3u}) {
                Fixture f; f.open_prediction(); f.type_he();
                f.aim(45); f.tick();
                unsigned observations = 0;
                f.fake.during_observe = [&] {
                    f.fake.observation.prediction_privacy = ++observations == guarded_observation
                        ? PredictionPrivacy::Protected : PredictionPrivacy::Unknown;
                };
                f.button(0x1000);
                check(!f.keyboard.prediction_view().count && f.fake.field == L"hee",
                    "manual typing contract is unchanged but any recognized protection suppresses the whole prediction observation");
                f.fake.during_observe = {};
                f.fake.observation.prediction_privacy = PredictionPrivacy::Unknown;
                f.button(0); f.center_stick(); f.tick(); f.tap(0x0001);
                check(f.fake.field == L"hee" && !f.keyboard.prediction_view().count, "unknown afterward cannot revive pre-protection fragments");
            }
        });
        test("privacy suppression clears suggestions without changing ordinary protected-field typing", [] {
            Fixture f; f.open_prediction(); f.type_he();
            f.fake.observation.prediction_privacy = PredictionPrivacy::Protected;
            f.tick(); f.type_he();
            check(f.fake.field == L"hehe" && !f.keyboard.prediction_view().count
                && f.keyboard.prediction_hint().empty(),
                "recognized password suppression is quiet and manual typing remains unchanged");
            f.fake.observation.prediction_privacy = PredictionPrivacy::Unknown; f.tick();
            check(f.keyboard.prediction_hint() == L"Word suggestions on - Down hides"
                && !f.keyboard.prediction_view().count, "unmasking cannot inherit protected-field input");
            f.type_he();
            check(f.keyboard.prediction_view().count == 3, "only new accepted letters form the next fragment");
        });
        test("protected and unavailable privacy metadata cannot arm prediction", [] {
            for (auto privacy : {PredictionPrivacy::Protected, PredictionPrivacy::Unavailable}) {
                Fixture f; f.fake.observation.prediction_privacy = privacy;
                f.open_prediction(); f.type_he(); f.tap(0x0001);
                check(f.fake.field == L"he" && !f.keyboard.prediction_view().count,
                    "ineligible fields retain ordinary typing but never collect or append suggestions");
            }
        });
        test("ordinary navigation, focus, context and physical takeover invalidate native suggestions", [] {
            for (unsigned invalid = 0; invalid < 4; ++invalid) {
                Fixture f; f.open_prediction(); f.type_he();
                switch (invalid) {
                case 0: ++f.fake.observation.target.activity; break;
                case 1: ++f.fake.observation.target.focus_revision; break;
                case 2: ++f.fake.observation.target.context; break;
                default: f.fake.observation.physical_held = true; break;
                }
                f.button(0x0001);
                check(f.fake.field == L"he" && !f.keyboard.prediction_view().count,
                    "existing navigation, field/context and physical-input boundaries reject stale choices");
            }
        });
        test("native suffix preserves original target and privacy checks throughout literal delivery", [] {
            for (unsigned failure = 0; failure < 4; ++failure) {
                Fixture f; f.open_prediction(); f.type_he();
                f.fake.during_send = [&](TextEvent event) {
                    if (event.up && f.fake.field == L"her") {
                        if (failure == 0) { f.fake.observation.physical_held = true; }
                        else if (failure == 1) { f.fake.observation.prediction_privacy = PredictionPrivacy::Protected; }
                        else if (failure == 2) { f.fake.observation.prediction_privacy = PredictionPrivacy::Unavailable; }
                        else { ++f.fake.observation.target.focus_revision; }
                    }
                };
                f.button(0x0001); f.button(0);
                for (unsigned i = 0; i < 11; ++i) { f.tick(100); }
                check(f.fake.field == L"her" && !f.keyboard.prediction_view().count && !f.keyboard.delivery().pending_edits(),
                    "mid-suffix privacy/focus/physical changes cancel unsent text without replay");
            }
        });
        test("native completion tolerates bounded delayed application without replaying accepted text", [] {
            for (auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                Fixture f; f.keyboard.set_input_mode(mode); f.open_prediction(); f.type_he();
                const auto before = f.fake.accepted.size();
                f.fake.application_delay = 80;
                f.button(0x0001);
                check(f.fake.field == L"he" && f.fake.accepted.size() == before + 4
                    && !f.keyboard.delivery().pending_edits() && f.keyboard.delivery().phase() == TextPhase::Ready,
                    "literal suffix is delivered like manual/voice input without awaiting application receipt");
                f.button(0);
                for (unsigned i = 0; i < 3; ++i) { f.tick(); }
                check(f.fake.field == L"he" && f.fake.accepted.size() == before + 4,
                    "64ms application lag cannot replay either character");
                f.tick();
                check(f.fake.field == L"here" && f.fake.accepted.size() == before + 4,
                    "80ms applied result confirms only test fixture behavior, not a delivery requirement");
                for (unsigned i = 0; i < 5; ++i) { f.tick(); }
                f.tap(0x0001);
                check(f.fake.field == L"here" && f.fake.accepted.size() == before + 4
                    && !f.keyboard.delivery().pending_edits() && f.keyboard.delivery().phase() == TextPhase::Ready,
                    "both delayed characters eventually form exact here without duplication or pause");
            }
        });
        test("literal suggestions share manual delivery behavior without caret or receipt requirements", [] {
            for (Tick delay : {0u, 80u, 101u, 1500u}) {
                CompletionFixture f(delay);
                Fake manual; manual.field = L"he"; manual.application_delay = delay;
                TextDelivery ordinary(manual);
                check(ordinary.start(manual.observation.target)
                    && ordinary.enqueue(letter(U'r'), manual.now, manual.now + 1000)
                    && ordinary.enqueue(letter(U'e'), manual.now, manual.now + 1000), "manual literal baseline");
                f.pump(0); ordinary.pump();
                check(f.fake.accepted == manual.accepted && f.fake.field == manual.field
                    && !f.delivery.pending_edits(), "same literal events independent of delayed app receipt");
                f.fake.now += delay; manual.now += delay;
                f.fake.observe_text({}); manual.observe_text({});
                check(f.fake.field == L"here" && f.fake.field == manual.field, "application owns eventual receipt");
                f.pump(2000);
                check(f.fake.accepted.size() == 4, "late application receipt never causes replay");
            }
            CompletionFixture ignored;
            ignored.pump(0);
            ignored.fake.pending_application.clear(); // Simulate an app that accepts transport but discards its text.
            ignored.pump(2000);
            check(ignored.fake.field == L"he" && ignored.fake.accepted.size() == 4,
                "no app-receipt claim or retry when the application discards accepted input");
            CompletionFixture bounded(80, 1000);
            bounded.pump(0, 2);
            bounded.fake.observation.settling = true;
            bounded.pump(1000);
            check(bounded.delivery.phase() == TextPhase::Ready && bounded.fake.accepted.size() == 2,
                "settling may defer until the exact original completion deadline");
            bounded.fake.observation.settling = false;
            bounded.pump(1);
            check(bounded.delivery.phase() == TextPhase::Stopped && !bounded.delivery.pending_edits()
                && bounded.fake.accepted.size() == 2, "pending suffix expiry is fixed, never restamped");
            CompletionFixture release;
            release.pump(0, 1); release.pump(1001);
            check(release.delivery.owned() && release.fake.accepted.size() == 1, "expiry retains accepted down obligation");
            release.delivery.cleanup();
            check(!release.delivery.owned() && release.fake.accepted.size() == 2 && release.fake.accepted.back().up,
                "cleanup releases accepted down independently without replaying characters");
        });
        test("privacy and original target checks reject changes at both literal dispatch guards", [] {
            for (unsigned guarded_observation : {1u, 2u}) {
                for (unsigned failure = 0; failure < 5; ++failure) {
                    CompletionFixture f;
                    f.pump(0, 2);
                    unsigned observations = 0;
                    f.fake.during_observe = [&] {
                        if (++observations != guarded_observation) { return; }
                        auto& value = f.fake.observation;
                        switch (failure) {
                        case 0: value.prediction_privacy = PredictionPrivacy::Unavailable; break;
                        case 1: value.allowed = false; break;
                        case 2: value.prediction_privacy = PredictionPrivacy::Protected; break;
                        case 3: ++value.target.focus_revision; break;
                        default: value.physical_held = true; break;
                        }
                    };
                    f.pump(guarded_observation == 1 ? 16 : 80);
                    check(f.delivery.phase() == TextPhase::Stopped && f.fake.accepted.size() == 2
                        && !f.delivery.pending_edits(), "explicit suffix still revalidates privacy and original route at either guard");
                }
            }
            CompletionFixture restarted;
            restarted.pump(0, 2);
            restarted.delivery.stop(L"Cancelled");
            check(restarted.delivery.start(restarted.fake.observation.target), "restart after cancellation");
            restarted.pump(16);
            check(!restarted.delivery.pending_edits() && restarted.fake.accepted.size() == 2,
                "new session cannot inherit or replay cancelled suffix");
        });
        test("every partially accepted native suffix stops once and retains only independent release obligations", [] {
            for (unsigned accepted = 0; accepted < 4; ++accepted) {
                CompletionFixture f(0);
                for (unsigned i = 0; i < accepted; ++i) { f.fake.returns.push_back(1); }
                f.fake.returns.push_back(0);
                f.pump(0);
                check(f.delivery.phase() == TextPhase::Stopped && !f.delivery.pending_edits()
                    && f.fake.attempted.size() == accepted + 1, "partial native suffix is stopped explicitly");
                const auto field = f.fake.field;
                f.pump(100); f.delivery.cleanup();
                check(f.fake.field == field && !f.delivery.owned(),
                    "cleanup can release an accepted key, never repeat any suffix character");
                check(f.fake.accepted.size() == accepted + (accepted % 2), "only the matching accepted-up obligation survives");
            }
            CompletionFixture uncertain;
            uncertain.fake.returns = {2};
            uncertain.pump(0); uncertain.delivery.cleanup();
            check(uncertain.delivery.uncertain() && uncertain.fake.attempted.size() == 1,
                "ambiguous acceptance remains explicit and cannot be retried");
        });
        test("native ordinary partial edits, voice intent and metadata loss revoke predictions", [] {
            Fixture f; f.open_prediction(); f.type_he();
            f.fake.returns = {1, 0}; f.aim(45); f.tick(); f.button(0x1000);
            check(!f.keyboard.prediction_view().count, "partial transport acceptance never authorizes native prediction");
            Fixture g; g.open_prediction(); g.type_he(); g.tap(0x0080); g.tap(0x0001);
            check(g.fake.field == L"he" && !g.keyboard.prediction_view().count, "voice intent clears the fragment even without backend");
            Fixture h; h.open_prediction(); h.type_he();
            h.fake.observation.prediction_privacy = PredictionPrivacy::Unavailable;
            h.tick(); h.fake.observation.prediction_privacy = PredictionPrivacy::Unknown; h.tick(); h.tap(0x0001);
            check(h.fake.field == L"he" && !h.keyboard.prediction_view().count, "restored metadata cannot revive the cleared fragment");
        });
        test("native pending suffix cancels stick activity even while acceptance Up remains held", [] {
            Fixture f; f.open_prediction(); f.type_he();
            f.fake.during_send = [&](TextEvent event) {
                if (event.up) {
                    f.fake.observation.settling = true;
                }
            };
            f.button(0x0001);
            check(f.fake.field == L"her" && f.keyboard.delivery().pending_edits() == 1, "one suffix character awaits ordinary input settling");
            f.aim(90); f.tick();
            check(f.fake.field == L"her" && !f.keyboard.delivery().pending_edits() && !f.keyboard.prediction_view().count,
                "held Up cannot mask other controller activity or authorize remaining suffix");
        });
        test("reserved controller window messages preserve practice before and after XInput actions", [] {
            constexpr std::array<UINT, 4> messages{WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP};
            constexpr std::array<std::pair<unsigned, unsigned>, 2> ranges{{
                {VK_NAVIGATION_VIEW, VK_NAVIGATION_CANCEL}, {VK_GAMEPAD_A, VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT}}};
            for (auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                for (const auto [first, last] : ranges) {
                    for (unsigned key = first; key <= last; ++key) {
                        for (auto message : messages) {
                            Fixture f; f.fake.preview = true; f.keyboard.set_input_mode(mode); f.prediction_word();
                            unsigned closed = 0, suspended = 0;
                            const auto dispatch = [&] {
                                return dispatch_keyboard_preview_input(message, key, true, [] { return true; },
                                    [&] { ++closed; }, [&] {
                                        ++suspended;
                                        f.keyboard.dismiss();
                                        f.fake.field.clear();
                                    });
                            };
                            const auto original = f.keyboard.prediction_view();
                            check(!dispatch() && f.keyboard.prediction_view() == original && f.fake.field == L" he",
                                "reserved message before XInput preserves current prefix and generation");
                            f.tap(0x0008);
                            const auto selected = f.keyboard.prediction_view();
                            check(selected.selected == 1 && !dispatch() && f.keyboard.prediction_view() == selected,
                                "reserved message after selection preserves the selected suggestion");
                            f.tap(0x0001);
                            check(f.fake.field == L" her" && !dispatch() && f.fake.field == L" her",
                                "reserved message after acceptance cannot erase completed output");
                            f.tap(0x0001);
                            check(f.fake.field == L" her" && !closed && !suspended,
                                "message dispatch never cancels practice or replays controller acceptance");
                        }
                    }
                }
            }
        });
        test("ordinary window key messages still clear practice and focused Escape still closes", [] {
            constexpr std::array<UINT, 4> messages{WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP};
            for (unsigned key = 1; key < 256; ++key) {
                if (!live::physical_async_key(key)) { continue; }
                for (auto message : messages) {
                    PredictionPractice practice; seed_prediction_demo(practice, KeyboardInputMode::FaceButtons);
                    unsigned closed = 0, suspended = 0;
                    const bool handled = dispatch_keyboard_preview_input(message, key, true, [] { return true; },
                        [&] { ++closed; }, [&] { ++suspended; practice.clear(); });
                    if (message == WM_KEYDOWN && key == VK_ESCAPE) {
                        check(handled && closed == 1 && !suspended, "focused Escape down exits without falling through");
                    } else {
                        check(!handled && !closed && suspended == 1 && practice.sample().empty()
                            && !practice.keyboard().visible() && !practice.keyboard().prediction_view().count,
                            "ordinary down/up/system key message resets visible practice and prediction");
                    }
                }
            }
            for (bool active : {false, true}) {
                for (bool practice : {false, true}) {
                    for (auto message : messages) {
                        unsigned closed = 0, suspended = 0;
                        const bool handled = dispatch_keyboard_preview_input(message, VK_ESCAPE, practice,
                            [&] { return active; }, [&] { ++closed; }, [&] { ++suspended; });
                        const bool exits = message == WM_KEYDOWN && active;
                        check(handled == exits && closed == static_cast<unsigned>(exits)
                            && suspended == static_cast<unsigned>(practice && !exits),
                            "Escape focus and non-practice demo/normal-preview behavior is preserved");
                    }
                }
            }
            for (auto message : {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_MOUSEWHEEL}) {
                PredictionPractice practice; seed_prediction_demo(practice, KeyboardInputMode::FaceButtons);
                bool closed = false;
                check(!dispatch_keyboard_preview_input(message, 0, true, [] { return true; },
                    [&] { closed = true; }, [&] { practice.clear(); }) && !closed && practice.sample().empty(),
                    "ordinary mouse button and wheel messages retain practice reset");
            }
            unsigned suspended = 0;
            check(!dispatch_keyboard_preview_input(WM_KEYDOWN, 'A', false, [] { return true; },
                [] {}, [&] { ++suspended; }) && !suspended, "normal keyboard preview and synthetic demo do not gain practice reset");
            check(!dispatch_keyboard_preview_input(WM_PAINT, 'A', true, [] { return true; },
                [] {}, [&] { ++suspended; }) && !suspended, "non-input messages do not reset practice");
        });
        test("ranked suggestions are bounded case-preserving and require a known word boundary", [] {
            for (const auto prefix : {U"he", U"He", U"HE"}) {
                WordPrediction prediction;
                for (auto c : std::u32string_view(prefix)) { prediction.completed(letter(c)); }
                check(prediction.view().count == 0, "session start cannot infer existing word boundary");
                prediction.completed(letter(U' '));
                for (auto c : std::u32string_view(prefix)) { prediction.completed(letter(c)); }
                const auto view = prediction.view();
                check(view.count == 3 && view.words[0].size() == 4, "ranked he family offers exactly three choices");
                check(view.words[0].starts_with(std::wstring(prediction.prefix())), "suggestion preserves prefix case");
                const auto suffix = prediction.accept(view.generation);
                check(suffix && (suffix == U"re" || suffix == U"RE"), "only the remaining suffix is returned");
                check(!prediction.accept(view.generation), "acceptance is consumed exactly once");
            }
            WordPrediction prediction;
            const auto prefix = [&](std::u32string_view value) {
                prediction.completed(letter(U' '));
                for (auto c : value) { prediction.completed(letter(c)); }
            };
            prefix(U"hE"); check(!prediction.view().count, "mixed case is not rewritten");
            prefix(U"hello"); check(!prediction.view().count, "exact word is not offered again");
            prefix(U"zz"); check(!prediction.view().count, "unknown vocabulary has no fallback");
            prefix(U"h"); check(!prediction.view().count, "at least two letters are required");
            prefix(U"th"); check(prediction.view().count == prediction_choice_limit, "candidate list is truncated");
            const auto old = prediction.view().generation;
            prediction.select(true);
            check(!prediction.accept(old), "selection invalidates stale acceptance");
            prediction.select(false);
            check(prediction.view().selected == 0, "selection wraps both directions");
            prefix(std::u32string(prediction_prefix_limit, U'a'));
            check(prediction.prefix().size() == prediction_prefix_limit, "exact maximum prefix retained");
            prediction.completed(letter(U'a'));
            check(prediction.prefix().empty(), "overflow discards entire unknown word, never truncates into a new prefix");
            prediction.completed(backspace);
            prediction.completed(letter(U'h')); prediction.completed(letter(U'e'));
            check(!prediction.view().count, "backspace cannot revive discarded overflow");
            prefix(U"he"); prediction.completed(backspace);
            check(prediction.prefix() == L"h" && !prediction.view().count, "known backspace removes one hint character");
            prediction.completed(backspace); prediction.completed(backspace);
            prediction.completed(letter(U'h')); prediction.completed(letter(U'e'));
            check(!prediction.view().count, "backspace beyond the known prefix invalidates the boundary");
            for (auto separator : {U'1', U'\'', U'-', U'.', U'\u00e9', U'\U0001f600'}) {
                prefix(U"he"); prediction.completed(letter(separator));
                prediction.completed(letter(U'h')); prediction.completed(letter(U'e'));
                check(!prediction.view().count, "nonletters suppress until explicit whitespace, not inferred caret text");
            }
            prefix(U"he");
            const auto generation = prediction.view().generation;
            prediction.clear();
            check(!prediction.accept(generation) && prediction.prefix().empty(), "clear revokes old suggestions");
            prediction.completed(enter); prediction.completed(letter(U'h')); prediction.completed(letter(U'e'));
            check(prediction.view().count == 3, "explicit Enter establishes a new boundary without generating Enter");
        });
        test("Settings opt-out disables hints and completion but not ordinary typing", [] {
            Fixture f; f.keyboard.set_prediction_enabled(false); f.prediction_word();
            check(!f.keyboard.prediction_view().count && f.keyboard.prediction_hint().empty(), "opt-out has no hints");
            const auto before = f.fake.field;
            f.tap(0x0001);
            check(f.fake.field == before, "D-pad does not add external output");
            f.keyboard.set_prediction_enabled(true); f.tick();
            check(!f.keyboard.prediction_view().count, "opt-in never restores the old fragment");
            f.type_he();
            check(f.keyboard.prediction_view().count == 3, "new letters collect without another consent gesture");
            f.keyboard.set_prediction_enabled(false);
            check(!f.keyboard.prediction_view().count && f.keyboard.prediction_hint().empty(), "disable clears immediately");
        });
        test("Settings opt-out cancels partial completion but preserves accepted-up ownership", [] {
            for (bool after_down : {false, true}) {
                Fixture f; f.open_prediction(); f.type_he();
                bool disabled = false;
                f.fake.during_send = [&](TextEvent event) {
                    if (!disabled && event.up != after_down) {
                        disabled = true;
                        f.keyboard.set_prediction_enabled(false);
                    }
                };
                f.button(0x0001);
                check(disabled && f.fake.field == L"her" && !f.keyboard.delivery().pending_edits()
                    && !f.keyboard.prediction_view().count && f.keyboard.prediction_hint().empty(),
                    "only accepted first suffix character remains; unsent suffix and hints are cleared");
                check(f.keyboard.delivery().owned() == after_down, "accepted down survives reentrant disable");
                f.fake.during_send = {}; f.button(0); f.tick();
                check(!f.keyboard.delivery().owned() && f.fake.field == L"her", "cleanup sends only matching up, never remaining text");
                f.keyboard.set_prediction_enabled(true); f.tap(0x0001);
                check(f.fake.field == L"her", "reenabling cannot replay interrupted completion");
            }
        });
        test("Settings disable at either dispatch guard cannot start a completion", [] {
            for (unsigned guard : {2u, 3u}) {
                Fixture f; f.open_prediction(); f.type_he();
                unsigned observations = 0;
                f.fake.during_observe = [&] {
                    if (++observations == guard) { f.keyboard.set_prediction_enabled(false); }
                };
                f.button(0x0001);
                check(f.fake.field == L"he" && !f.keyboard.delivery().owned()
                    && !f.keyboard.delivery().pending_edits() && f.keyboard.prediction_hint().empty(),
                    "disable before first suffix dispatch cancels without external output");
            }
        });
        test("held voice intent cannot repopulate prediction even without a voice backend", [] {
            Fixture f; f.open_prediction(); f.aim(45); f.tick();
            f.button(0x0080);
            f.button(0x8080); f.button(0x0080);
            f.button(0x1080); f.button(0x0080);
            f.center_stick(); f.tick();
            check(f.fake.field == L"he" && !f.keyboard.prediction_view().count,
                "manual fallback edits during voice hold are not a prediction fragment");
            f.button(0); f.tick();
            check(!f.keyboard.prediction_view().count, "released voice intent cannot restore held fragment");
            f.type_he();
            check(f.keyboard.prediction_view().count == 3, "fresh post-voice manual letters work without consent");
        });
        test("face and one-handed prediction require neutral deliberate D-pad acceptance without spaces", [] {
            for (auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                Fixture f; f.fake.preview = true; f.keyboard.set_input_mode(mode); f.prediction_word();
                check(f.fake.field == L" he", mode == KeyboardInputMode::FaceButtons
                    ? "face fixture must emit he" : "one-handed fixture must emit he");
                check(f.keyboard.prediction_view().count == 3, mode == KeyboardInputMode::FaceButtons
                    ? "face mode builds suggestions from completed edits" : "one-handed mode builds suggestions from completed edits");
                f.button(0x0008);
                check(f.keyboard.prediction_view().selected == 1, "right chooses her");
                f.tick(); check(f.keyboard.prediction_view().selected == 1, "held D-pad never repeats");
                f.button(0x0001);
                check(f.fake.field == L" he", "rolling right into up without neutral cannot accept");
                f.button(0); f.button(0x0001);
                check(f.fake.field == L" her", "deliberate up appends suffix only, no automatic space");
                for (unsigned i = 0; i < 8; ++i) { f.tick(); }
                f.button(0); f.tap(0x0001);
                check(f.fake.field == L" her" && !f.keyboard.prediction_view().count, "held and repeated acceptance cannot replay");
                f.tap(0x0100); check(f.fake.field == L" he", "LB still edits real preview output without inferring suffix prefix");
                check(!f.keyboard.prediction_view().count, "deleting a completion does not reconstruct its prefix");
                f.tap(0x0200); check(f.fake.field == L" he ", "RB remains explicit space");
                f.tap(0x0010); check(f.fake.field == L" he \r\n", "Menu remains Enter");
                f.tap(0x0020); check(!f.keyboard.visible(), "View remains cancellation");
            }
        });
        test("prediction does not steal face or one-handed action gestures", [] {
            Fixture f; f.fake.preview = true; f.prediction_word();
            f.button(0x1001);
            check(f.fake.field == L" hee", "mixed D-pad plus face is the existing face action only");
            Fixture g; g.fake.preview = true; g.keyboard.set_input_mode(KeyboardInputMode::OneHanded); g.prediction_word();
            g.aim(315); g.tick(); g.center_stick(); g.tick();
            check(!g.keyboard.prediction_view().count, "suggestions hide until the one-handed action gesture finishes");
            g.button(0x0001);
            check(g.fake.field == L" he", "D-pad does not accept inside one-handed action group");
            g.button(0); g.aim(90); g.tick(); g.center_stick(); g.tick();
            check(g.fake.field == L" h", "one-handed Backspace still works");
        });
        test("partial edits and failed completion events never become prediction hints or replay", [] {
            for (unsigned accepted = 0; accepted < 4; ++accepted) {
                Fixture f; f.fake.preview = true; f.prediction_word();
                for (unsigned i = 0; i < accepted; ++i) { f.fake.returns.push_back(1); }
                f.fake.returns.push_back(0);
                f.button(0x0001);
                check(!f.keyboard.prediction_view().count, "partial completion clears all suggestions");
                const auto output = f.fake.field;
                f.button(0); f.tap(0x0001);
                check(f.fake.field == output, "neither rejected nor partially accepted completion is retried");
            }
            Fixture f; f.fake.preview = true; f.prediction_word();
            f.fake.returns = {1, 0}; f.aim(45); f.tick(); f.button(0x1000);
            check(!f.keyboard.prediction_view().count, "down without accepted up must never update prefix");
        });
        test("target context takeover disconnect and mode changes clear prediction", [] {
            for (unsigned reason = 0; reason < 8; ++reason) {
                Fixture f; f.fake.preview = true; f.prediction_word();
                switch (reason) {
                case 0: ++f.fake.observation.target.focus_revision; break;
                case 1: ++f.fake.observation.target.activity; break;
                case 2: ++f.fake.observation.target.context; break;
                case 3: ++f.fake.observation.target.windows[2].window; break;
                case 4: f.pad().connection = Connection::Disconnected; break;
                case 5: f.keyboard.set_input_mode(KeyboardInputMode::OneHanded); break;
                case 6: f.eligibility.desktop = false; break;
                default: f.fake.observation.physical_held = true; break;
                }
                f.button(0x0001);
                check(!f.keyboard.prediction_view().count && f.fake.field == L" he", "interruption prevents suffix and clears hint");
            }
        });
        test("stale pending completion retains original deadlines and cancellation generation", [] {
            Fixture f; f.fake.preview = true; f.prediction_word(U"so"); f.tap(0x0008);
            check(f.keyboard.prediction_view().words[1] == L"something", "fixture selects a multiframe suffix");
            f.button(0x0001);
            check(f.keyboard.delivery().pending_edits() == 3 && f.fake.field == L" someth", "only eight events pumped per frame");
            f.fake.observation.settling = true;
            f.button(0); f.tick(90);
            check(!f.keyboard.prediction_view().count && !f.keyboard.visible(), "expired settling intent is never restamped");
            const auto partial = f.fake.field;
            f.fake.observation.settling = false; f.tap(0x0001);
            check(f.fake.field == partial, "expired suffix never replays");
            Fixture g; g.fake.preview = true; g.prediction_word();
            g.fake.during_send = [&](TextEvent) { g.keyboard.dismiss(); };
            g.button(0x0001);
            check(!g.keyboard.prediction_view().count && !g.keyboard.visible(), "reentrant cancellation cannot republish completion");
            Fixture h; h.fake.preview = true; h.prediction_word();
            h.fake.now += 101;
            h.keyboard.step(h.snapshot, h.fake.now, h.eligibility);
            check(!h.keyboard.prediction_view().count && h.fake.field == L" he", "stale sampled controller cannot accept");
        });
        test("prediction cancels across fake voice transitions and never learns dictation", [] {
            struct VoiceFake final : VoiceBoundary {
                std::uint64_t generation = 0;
                void begin(std::uint64_t value, Tick) override { generation = value; }
                void renew(std::uint64_t, Tick) override {}
                void finish(std::uint64_t) override {}
                void cancel() override {}
                VoiceUpdate poll() override { return {generation, VoiceBackendPhase::Ready, 0, U" he", {}}; }
            } voice;
            Fixture f; f.fake.preview = true; f.keyboard.set_voice(&voice); f.prediction_word();
            f.button(0x0080);
            check(!f.keyboard.prediction_view().count, "voice start removes suggestion bar");
            f.button(0);
            check(f.fake.field == L" he he" && !f.keyboard.prediction_view().count, "dictation is not a prediction source");
            f.tick(); f.tap(0x0001);
            check(f.fake.field == L" he he", "stale pre-dictation suggestion never inserts");
        });
        test("completed edit batches exclude partials stops and queue overflow", [] {
            Fake f; TextDelivery delivery(f); check(delivery.start(f.observation.target), "start delivery");
            check(delivery.enqueue(letter(U'h'), f.now, f.now + 100), "enqueue");
            check(!delivery.pump(1).count, "accepted down is not a completed edit");
            const auto batch = delivery.pump(1);
            check(batch.count == 1 && batch.edits[0] == letter(U'h'), "accepted pair reports its original edit once");
            check(!delivery.pump().count, "completed edit is consumed");
            for (std::size_t i = 0; i < keyboard_edit_queue_limit; ++i) { check(delivery.enqueue(letter(U'e'), f.now, f.now + 100), "fill queue"); }
            check(!delivery.enqueue(letter(U'e'), f.now, f.now + 100), "overflow rejects atomically before output");
            check(!delivery.pump().count, "cancelled overflow reports no completed hints");
        });
        test("synthetic prediction demo uses only bounded owned memory in both modes", [] {
            for (auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                PredictionPractice practice; seed_prediction_demo(practice, mode);
                check(practice.sample() == L" he" && practice.keyboard().prediction_view().count == 3,
                    "demo drives actual keyboard and fake delivery to a visible suggestion");
                for (std::size_t i = practice.sample().size(); i < keyboard_text_limit; ++i) {
                    check(practice.send_text({{TextKeyKind::Unicode, L'a'}, false}) == 1, "bounded practice capacity");
                }
                check(practice.send_text({{TextKeyKind::Unicode, L'a'}, false}) == 0, "practice overflow explicitly rejected");
                practice.clear();
                check(practice.sample().empty() && !practice.keyboard().prediction_view().count, "practice reset drops sample and suggestions");
            }
        });
        test("navigation filtering requires the active keyboard's still-valid original target", [] {
            Fixture f;
            check(!f.keyboard.navigation_filter_eligible(f.fake.observation), "closed keyboard cannot authorize filtering");
            f.open();
            check(f.keyboard.navigation_filter_eligible(f.fake.observation), "valid active desktop keyboard can authorize filtering");
            auto changed = f.fake.observation;
            ++changed.target.focus_revision;
            check(!f.keyboard.navigation_filter_eligible(changed), "changed focus cannot inherit keyboard filtering");
            changed = f.fake.observation; changed.physical_held = true;
            check(!f.keyboard.navigation_filter_eligible(changed), "physical overlap prevents keyboard filter policy");
            changed = f.fake.observation; changed.allowed = false;
            check(!f.keyboard.navigation_filter_eligible(changed), "exclusion/hard block prevents keyboard filter policy");
            f.keyboard.block(L"Output failure");
            check(!f.keyboard.navigation_filter_eligible(f.fake.observation), "failed keyboard cannot keep filter authorization");
            Fixture native; native.native_context(); native.hold_native(); native.button(0);
            check(native.keyboard.navigation_filter_eligible(native.fake.observation),
                "deliberate valid native keyboard may authorize only its active text session");
            native.keyboard.dismiss();
            check(!native.keyboard.navigation_filter_eligible(native.fake.observation), "native keyboard close clears filter authorization");
        });
        test("native keyboard is default off and ignores ordinary gameplay openers", [] {
            for (const auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                Fixture f; f.keyboard.set_input_mode(mode); f.native_context(false); f.hold_native();
                check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.fake.accepted.empty(), "default off must never invoke");
                Fixture g; g.keyboard.set_input_mode(mode); g.native_context(); g.tick(); g.tick();
                g.button(0x8000); g.tick(100); g.button(0); g.tick(); g.button(0x0040);
                check(!g.keyboard.visible() && g.fake.accepted.empty(), "native Y/LS alone must remain gameplay input");
            }
        });
        test("visible desktop keyboard never probes an unrelated mouse recipient for a native opener", [] {
            using namespace gamepad::live;
            for (const auto block : {BlockReason::Recipient, BlockReason::Exclusion, BlockReason::Surface}) {
                Fixture f; f.keyboard.set_native_mode_enabled(true); f.open();
                PointerFake pointer;
                pointer.observation.allowed = false;
                pointer.observation.block = block;
                // Desktop::observe_route's hard mouse-route failure changes its shared context.
                pointer.during_observe = [&] { ++f.fake.observation.target.context; };
                const auto original = f.fake.observation.target;
                f.eligibility = observe_keyboard_eligibility(pointer, f.keyboard, true, true, false, 0);
                f.tap(0x1000);
                check(f.keyboard.accepts_typing() && f.fake.field == L"a",
                    "native-mode opt-in must not dismiss valid desktop typing because of the mouse recipient");
                check(pointer.observations == 0 && f.fake.observation.target == original && pointer.events.empty(),
                    "visible keyboard must not mutate native routing, context or emit pointer input");
                check(f.eligibility.desktop && !f.eligibility.native && f.eligibility.preferred == 0u,
                    "desktop keyboard retains its mode and controller preference");
                ++f.fake.observation.target.context;
                f.button(0x1000);
                check(!f.keyboard.visible() && f.fake.field == L"a",
                    "an actual text-context invalidation must still cancel without replay");
            }
        });
        test("visible native and paused keyboards do not acquire a new mouse route", [] {
            for (const bool native : {false, true}) {
                Fixture f;
                if (native) { f.native_context(); f.hold_native(); f.button(0); }
                else { f.keyboard.set_native_mode_enabled(true); f.open(); }
                PointerFake pointer;
                for (const bool paused : {false, true}) {
                    if (paused) { f.keyboard.block(L"Explicit paused fixture"); }
                    const auto eligibility = observe_keyboard_eligibility(pointer, f.keyboard, true, true, native, {});
                    check(eligibility.native == native && eligibility.desktop != native && pointer.observations == 0,
                        "visible session keeps its original mode without a mouse observation even while paused");
                }
                const auto unsafe = observe_keyboard_eligibility(pointer, f.keyboard, false, true, native, {});
                check(!unsafe.desktop && !unsafe.native && pointer.observations == 0,
                    "lost host prerequisites never authorize either keyboard mode");
            }
        });
        test("closed keyboard native opener still checks the ordinary desktop safety boundary", [] {
            using namespace gamepad::live;
            for (unsigned kind = 0; kind < 11; ++kind) {
                Fixture f; PointerFake pointer;
                auto& route = pointer.observation;
                if (kind == 1) { route.available = false; }
                if (kind == 2) { route.baseline_known = false; }
                if (kind == 3) { route.routing_stable = false; }
                if (kind == 4) { route.physical_held = Other; }
                if (kind == 5) { route.mode = Mode::Menu; }
                if (kind >= 6) {
                    route.allowed = false;
                    constexpr BlockReason blocks[]{BlockReason::Recipient, BlockReason::Exclusion,
                        BlockReason::DesktopSession, BlockReason::AutoUnavailable, BlockReason::Auto};
                    route.block = blocks[kind - 6];
                    route.automatic = AutoReason::Confinement;
                }
                const auto eligibility = observe_keyboard_eligibility(pointer, f.keyboard, true, true, true, {});
                check(!eligibility.desktop && eligibility.native == (kind == 0 || kind == 10)
                    && pointer.observations == 1 && pointer.events.empty(),
                    "only a fully validated closed-keyboard route or recognized Auto permits native invocation");
            }
            Fixture f; PointerFake pointer;
            const auto option_off = observe_keyboard_eligibility(pointer, f.keyboard, true, false, true, {});
            const auto unsafe = observe_keyboard_eligibility(pointer, f.keyboard, false, true, true, {});
            check(!option_off.native && !unsafe.native && pointer.observations == 0,
                "disabled option and host prerequisites must not query routing");
        });
        test("visible desktop keyboard still cancels on genuine text-route and host failures", [] {
            using namespace gamepad::live;
            for (unsigned kind = 0; kind < 7; ++kind) {
                Fixture f; f.keyboard.set_native_mode_enabled(true); f.open();
                PointerFake pointer;
                if (kind == 0) { ++f.fake.observation.target.focus_revision; }
                if (kind == 1) { f.fake.observation.physical_held = true; }
                if (kind == 2) { f.fake.observation.stable = false; }
                if (kind == 3) { f.fake.observation.target.windows[2].window = 99; }
                if (kind == 4 || kind == 5) {
                    f.fake.route = pointer.observation;
                    f.fake.route->allowed = false;
                    f.fake.route->block = kind == 4 ? BlockReason::Exclusion : BlockReason::Auto;
                    f.fake.route->automatic = AutoReason::Confinement;
                }
                f.eligibility = observe_keyboard_eligibility(pointer, f.keyboard, kind != 6, true, false, {});
                f.button(0x1000);
                check(!f.keyboard.visible() && f.fake.attempted.empty() && pointer.observations == 0,
                    "text focus, physical input, route, exclusion, Auto and host failures still cancel without output");
            }
            Fixture f; f.keyboard.set_native_mode_enabled(true); f.open();
            f.fake.returns = {1, 0}; f.button(0x1000);
            check(f.keyboard.recovery_required() && f.fake.accepted.size() == 1,
                "partially accepted typing retains its own release");
            PointerFake pointer;
            f.eligibility = observe_keyboard_eligibility(pointer, f.keyboard, false, true, false, {});
            check(!f.eligibility.desktop && !f.eligibility.native && pointer.observations == 0
                && f.keyboard.recovery_required(), "routing helper cannot clear accepted-up recovery");
            f.keyboard.dismiss();
            f.keyboard.recover();
            check(!f.keyboard.recovery_required() && f.fake.accepted.size() == 2 && f.fake.accepted.back().up,
                "explicit recovery releases only the accepted key without replaying the edit");
        });
        test("Host eligibility retains native open type close flow for manual and heuristic native modes", [] {
            using namespace gamepad::live;
            for (const bool manual : {false, true}) {
                Fixture f; f.keyboard.set_native_mode_enabled(true);
                PointerFake pointer;
                if (!manual) {
                    pointer.observation.allowed = false;
                    pointer.observation.block = BlockReason::Auto;
                    pointer.observation.automatic = AutoReason::Confinement;
                    f.fake.route = pointer.observation;
                }
                const auto step = [&](std::uint16_t buttons, Tick elapsed = 16) {
                    f.pad().buttons = buttons;
                    f.eligibility = observe_keyboard_eligibility(pointer, f.keyboard, true, true, manual, {});
                    f.tick(elapsed);
                };
                step(0); step(0); step(0x8020);
                for (unsigned i = 0; i < 5; ++i) { step(0x8020, 100); }
                check(f.keyboard.visible() && f.keyboard.native_session() && f.fake.accepted.empty(),
                    "closed native opener still requires its full hold and never types the chord");
                const auto opening_queries = pointer.observations;
                step(0); f.pad().left_y = 28000; step(0); step(0x1000); step(0);
                check(f.fake.field == L"a" && pointer.observations == opening_queries && pointer.events.empty(),
                    "active native typing must use only the original text route without pointer output");
                step(0x0020);
                check(!f.keyboard.visible() && !f.fake.native_keyboard_session(),
                    "fresh View closes native session through unchanged delivery logic");
                f.pad().left_y = 0; step(0);
                check(pointer.observations == opening_queries + 1 && f.fake.field == L"a",
                    "only after closing may the next opener observe mouse routing again, without replay");
            }
        });
        test("native hold threshold consumes opening chord and supports both input modes", [] {
            for (const auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                Fixture f; f.keyboard.set_input_mode(mode); f.native_context(); f.tick(); f.tick(); f.button(0x8020);
                for (unsigned i = 0; i < 4; ++i) { f.tick(100); }
                f.tick(99);
                check(!f.keyboard.visible(), "499 ms must not open");
                f.tick(1);
                check(f.keyboard.visible() && f.keyboard.native_session() && f.fake.native_keyboard_session(), "500 ms opens only keyboard scope");
                f.tick(100); f.button(0x0020); f.button(0);
                check(f.keyboard.visible() && f.fake.accepted.empty(), "Y-first release must not cancel or type");
                if (mode == KeyboardInputMode::FaceButtons) { f.aim(0); f.tick(); f.tap(0x1000); }
                else { f.flick(0, 180); }
                check(f.fake.field == L"a", "actual configured model must type through native session");
                f.button(0x0020);
                check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.fake.field == L"a", "fresh View closes without rollback");
                f.tick(100);
                check(!f.keyboard.visible(), "held closing View cannot reopen");
            }
        });
        test("native controller handoff restarts the full hold for the new controller", [] {
            for (unsigned first = 0; first < 2; ++first) {
                Fixture f; f.native_context();
                f.snapshot.slots[1] = f.pad();
                auto& previous = f.snapshot.slots[first];
                auto& next = f.snapshot.slots[1 - first];
                f.tick(); f.tick();
                previous.buttons = 0x8020; f.tick();
                for (unsigned i = 0; i < 4; ++i) { f.tick(100); }
                previous.buttons = 0;
                next.buttons = 0x8020; f.tick(100);
                check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.fake.attempted.empty(),
                    "new controller must not inherit the previous controller's 500 ms threshold");
                for (unsigned i = 0; i < 4; ++i) { f.tick(100); }
                f.tick(99);
                check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.fake.attempted.empty(),
                    "new controller's own 499 ms hold must not open");
                f.tick(1);
                check(f.keyboard.visible() && f.keyboard.native_session() && f.fake.native_keyboard_session(),
                    "new controller's own 500 ms hold must open");
                next.buttons = 0; f.tick();
                next.left_y = 28000; f.tick();
                next.buttons = 0x1000; f.tick();
                check(f.fake.field == L"a", "new controller must own the opened keyboard");
            }
        });
        test("native chord View-first release and remaining held buttons are consumed", [] {
            Fixture f; f.native_context(); f.hold_native();
            f.button(0x8000); f.button(0x9000); f.button(0x1000);
            check(f.keyboard.visible() && f.fake.accepted.empty(), "View-first release and held A must not leak");
            f.button(0); f.aim(0); f.tick(); f.tap(0x1000);
            check(f.fake.field == L"a", "new edit after full release should work");
        });
        test("native opener rejects partial stale reconnect startup and conflicting controls", [] {
            for (unsigned kind = 0; kind < 7; ++kind) {
                Fixture f; f.native_context();
                if (kind != 0) { f.tick(); f.tick(); }
                if (kind == 1) { f.button(0x0020); }
                f.button(0x8020);
                if (kind == 2) { f.tick(101); }
                if (kind == 3) { ++f.pad().connection_epoch; }
                if (kind == 4) { f.pad().buttons |= 0x1000; }
                if (kind == 5) { f.pad().left_y = 28000; }
                if (kind == 6) { f.snapshot.slots[1] = f.pad(); }
                for (unsigned i = 0; i < 7; ++i) { f.tick(100); }
                check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.fake.accepted.empty(),
                    "invalid native hold must never invoke");
            }
        });
        test("native keyboard Menu View mode chord always wins", [] {
            Fixture f; f.native_context(); f.tick(); f.tick(); f.button(0x8030);
            for (unsigned i = 0; i < 6; ++i) { f.tick(100); }
            check(!f.keyboard.visible(), "Menu View overlap cannot open native keyboard");
            Fixture g; g.native_context(); g.hold_native(); g.button(0x8030);
            check(!g.keyboard.visible() && !g.fake.native_keyboard_session() && g.fake.accepted.empty(),
                "global mode chord must close even before invocation release");
        });
        test("native text allowance is immutable and limited to recognized heuristic Auto", [] {
            using namespace gamepad::live;
            for (const auto automatic : {AutoReason::ExclusiveD3D, AutoReason::Presentation, AutoReason::Confinement}) {
                Observation route; route.available = route.baseline_known = route.cleanup_allowed = true;
                route.mode = Mode::Pointer; route.block = BlockReason::Auto; route.automatic = automatic;
                route.context = 77;
                for (unsigned i = 0; i < 20; ++i) {
                    check(!keyboard_route_allowed(route, false) && keyboard_route_allowed(route, true),
                        "native-session allowance must apply only to text");
                    check(!route.allowed && route.context == 77 && route.block == BlockReason::Auto,
                        "text adaptation must not mutate ordinary allowed/context state");
                }
                Fixture f; f.fake.route = route; f.native_context(); f.hold_native(); f.button(0);
                f.aim(0); f.tick(); f.tap(0x1000);
                check(f.fake.field == L"a", "recognized Auto must permit captured native text edits");
                check(!f.fake.route->allowed, "text scope must not authorize ordinary pointer");
            }
            for (const auto block : {BlockReason::Exclusion, BlockReason::Recipient, BlockReason::DesktopSession,
                BlockReason::AutoUnavailable, BlockReason::AutoNotPresent, BlockReason::Geometry, BlockReason::Routing}) {
                Observation route; route.available = route.baseline_known = true; route.mode = Mode::Pointer;
                route.block = block; route.automatic = AutoReason::Confinement;
                Fixture f; f.fake.route = route; f.native_context(); f.hold_native();
                check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.fake.accepted.empty(), "hard guards must reject and clear scope");
            }
        });
        test("native setting off or target loss cancels scope and preserves pending-up safety", [] {
            Fixture f; f.native_context(); f.hold_native(); f.button(0);
            f.aim(0); f.tick(); f.fake.returns = {1, 0}; f.button(0x1000);
            check(f.keyboard.recovery_required(), "rejected up fixture");
            f.keyboard.set_native_mode_enabled(false);
            check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.keyboard.blocks_pointer(),
                "option off must hide and clear allowance but keep owned-up blocking");
            f.keyboard.recover();
            check(!f.keyboard.recovery_required() && f.fake.accepted.size() == 2 && f.fake.accepted.back().up,
                "only accepted up may be cleaned after option off");
            Fixture g; g.native_context(); g.hold_native(); g.button(0);
            ++g.fake.observation.target.focus_revision; g.tick();
            check(!g.keyboard.visible() && !g.fake.native_keyboard_session() && g.fake.accepted.empty(), "target loss clears native session without replay");
        });
        test("native opener reentrant dismissal cannot retain text allowance", [] {
            Fixture f; f.native_context();
            f.fake.during_observe = [&] { f.keyboard.dismiss(); };
            f.hold_native();
            check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.fake.accepted.empty(), "failed/reentrant open must clear scope");
            Fixture g; g.native_context();
            g.fake.during_observe = [&] { g.keyboard.set_native_mode_enabled(false); };
            g.hold_native();
            check(!g.keyboard.visible() && !g.fake.native_keyboard_session() && g.fake.accepted.empty(),
                "disabling option during target capture must abort the opener");
        });
        test("native typing preserves manual and Auto-native pointer policy without mouse output", [] {
            for (const bool manual : {false, true}) {
                Fixture f; f.native_context();
                PointerFake pointer;
                gamepad::live::Live live(pointer);
                if (manual) { live.suspend_automatic(pointer.time); }
                else {
                    pointer.observation.allowed = false;
                    pointer.observation.block = gamepad::live::BlockReason::Auto;
                    pointer.observation.automatic = gamepad::live::AutoReason::Confinement;
                }
                const auto route = [&] {
                    pointer.time = f.fake.now;
                    live.hold_for_keyboard(f.keyboard.blocks_pointer(), pointer.time);
                    live.automatic_step(f.snapshot, pointer.time, true);
                };
                f.tick(); route(); f.tick(); route(); f.button(0x8020); route();
                for (unsigned i = 0; i < 5; ++i) { f.tick(100); route(); }
                check(f.keyboard.visible() && live.keyboard_held(), "native opener must route only keyboard");
                f.button(0); route(); f.aim(0); f.tick(); route(); f.tap(0x1000); route();
                check(f.fake.field == L"a" && pointer.events.empty() && !live.controller_active()
                    && live.automatically_paused() == manual && !live.auto_override_active()
                    && live.desktop_requests() == 0, "native typing must not toggle mode, grant override or emulate pointer");
                f.button(0x0020); route(); f.center_stick(); f.button(0); route();
                check(!f.keyboard.visible() && live.automatically_paused() == manual
                    && pointer.events.empty() && !live.auto_override_active(), "normal keyboard close preserves native policy");
            }
        });
        test("native physical menu baseline and infrastructure guards remain hard", [] {
            using namespace gamepad::live;
            for (unsigned gate = 0; gate < 4; ++gate) {
                Observation route; route.available = route.baseline_known = route.cleanup_allowed = true;
                route.mode = Mode::Pointer; route.block = BlockReason::Auto; route.automatic = AutoReason::ExclusiveD3D;
                if (gate == 0) { route.physical_held = Other; }
                if (gate == 1) { route.mode = Mode::Menu; }
                if (gate == 2) { route.available = false; }
                if (gate == 3) { route.baseline_known = false; }
                Fixture f; f.fake.route = route; f.native_context(); f.hold_native();
                check(!f.keyboard.visible() && !f.fake.native_keyboard_session() && f.fake.accepted.empty(),
                    "hard native guard cannot be treated as heuristic Auto");
            }
        });
        test("exact Unicode and Backspace Enter native event shape", [] {
            for (const auto edit : {letter(U'a'), letter(0x1f600), backspace, enter}) {
                const auto sequence = edit_events(edit);
                check(sequence.has_value(), "valid scalar or edit has events");
                for (unsigned i = 0; i < sequence->count; ++i) {
                    const auto event = sequence->events[i];
                    const auto input = gamepad::live::keyboard_input(event);
                    check(event.up == ((i % 2) == 1) && input.type == INPUT_KEYBOARD, "events must alternate down/up");
                    check(input.ki.dwExtraInfo == gamepad::live::output_tag && input.ki.time == 0, "ownership tag and time");
                    const bool unicode = event.key.kind == TextKeyKind::Unicode;
                    check(input.ki.wVk == (unicode ? 0 : gamepad::live::text_virtual_key(event.key))
                        && input.ki.wScan == (unicode ? event.key.unit : 0), "scan and VK shape");
                    check(input.ki.dwFlags == static_cast<DWORD>((unicode ? KEYEVENTF_UNICODE : 0)
                        | (event.up ? KEYEVENTF_KEYUP : 0)), "Unicode flag must not leak into virtual keys");
                }
            }
            const auto pair = edit_events(letter(0x1f600));
            check(pair->count == 4 && pair->events[0].key.unit == 0xd83d && pair->events[2].key.unit == 0xde00,
                "supplementary scalar must preserve surrogate pair order");
        });
        test("characters reach target immediately before any Menu gesture", [] {
            Fixture f;
            f.open();
            f.button(0x1000);
            check(f.fake.field == L"a" && f.keyboard.delivery().completed_edits() == 1, "first letter must arrive in same step");
            check(f.keyboard.delivery().last_dispatch_ms() == 0, "no extra UI-step dispatch delay in fake clock");
            f.button(0);
            f.button(0x2000);
            check(f.fake.field == L"ab", "second letter arrives without commit or stick neutrality");
            check(f.keyboard.model().text().empty(), "production must not accumulate a draft");
        });
        test("Backspace edits preexisting field and RB space Menu Enter are immediate", [] {
            Fixture f;
            f.fake.field = L"seed";
            f.open();
            f.button(0x0100);
            check(f.fake.field == L"see", "LB must edit preexisting target text even with empty local buffer");
            f.button(0);
            f.button(0x0200);
            check(f.fake.field == L"see ", "space must arrive without Menu");
            f.button(0);
            f.pad().left_trigger = 255;
            f.button(0x1000);
            check(f.fake.field == L"see A", "page changes do not require fresh full neutral");
            f.button(0);
            f.button(0x0010);
            check(f.fake.field == L"see A", "Menu press alone must wait for release for chord precedence");
            f.button(0);
            check(f.fake.field == L"see A\r\n", "Menu release is Enter despite held stick/trigger");
            check(f.keyboard.accepts_typing(), "Enter must not close or finish the live keyboard");
        });
        test("Menu release preserves fresh face edit after Enter in the same step", [] {
            Fixture f; f.open(); f.button(0x0010); f.button(0x1000);
            check(f.fake.field == L"\r\na" && f.keyboard.delivery().completed_edits() == 2,
                "Menu-up+A must deliver Enter then a without waiting for another sample");
            check(f.fake.accepted == std::vector<TextEvent>{
                {{TextKeyKind::Enter}, false}, {{TextKeyKind::Enter}, true},
                {{TextKeyKind::Unicode, L'a'}, false}, {{TextKeyKind::Unicode, L'a'}, true}},
                "coincident Enter/character event order must be exact");
            f.tick();
            check(f.fake.accepted.size() == 4, "held A must not duplicate the preserved edit");
        });
        test("Menu release preserves fresh space after Enter in the same step", [] {
            Fixture f; f.open(); f.button(0x0010); f.button(0x0200);
            check(f.fake.field == L"\r\n " && f.keyboard.delivery().completed_edits() == 2,
                "Menu-up+RB must deliver Enter then space immediately");
            check(f.fake.accepted[0].key.kind == TextKeyKind::Enter
                && f.fake.accepted[2].key == TextKey{TextKeyKind::Unicode, L' '}, "Enter before space");
        });
        test("Menu release preserves fresh Backspace and arms its repeat timer", [] {
            Fixture f; f.fake.field = std::wstring(30, L'x'); f.open(); f.button(0x0010); f.button(0x0100);
            check(f.keyboard.delivery().completed_edits() == 2 && f.fake.accepted.size() == 4,
                "Menu-up+LB must deliver both edits immediately");
            check(f.fake.accepted[0].key.kind == TextKeyKind::Enter
                && f.fake.accepted[2].key.kind == TextKeyKind::Backspace, "Enter before Backspace");
            for (unsigned i = 0; i < 24; ++i) { f.tick(); }
            check(f.keyboard.delivery().completed_edits() == 2, "Backspace repeat still waits 400 ms");
            f.tick();
            check(f.keyboard.delivery().completed_edits() == 3, "coincident LB edge must arm repeat");
            f.tick(90);
            check(f.keyboard.delivery().completed_edits() == 4
                && f.fake.accepted[6].key.kind == TextKeyKind::Backspace, "continued repeat stays Backspace");
        });
        test("View and mode cancel win over Menu-release edits", [] {
            for (const std::uint16_t edit : {std::uint16_t{0x1000}, std::uint16_t{0x0200}, std::uint16_t{0x0100}}) {
                Fixture f; f.open(); f.button(0x0010);
                f.button(static_cast<std::uint16_t>(edit | 0x0020));
                check(!f.keyboard.visible() && f.fake.accepted.empty(), "View cancels Enter and accompanying edit");
                Fixture g; g.open(); g.button(0x0010);
                g.snapshot.slots[1] = g.pad();
                g.snapshot.slots[1].buttons = 0x0030;
                g.button(edit);
                check(!g.keyboard.visible() && g.fake.accepted.empty(), "global mode chord cancels both edits");
            }
        });
        test("a rejected Enter never replays the accompanying edit", [] {
            Fixture f; f.open(); f.fake.returns = {0}; f.button(0x0010); f.button(0x1000);
            check(f.fake.attempted.size() == 1 && f.fake.accepted.empty()
                && f.keyboard.delivery().pending_edits() == 0, "Enter failure cancels trailing same-sample character");
            f.button(0); f.button(0x1000);
            check(f.fake.attempted.size() == 1, "stopped session cannot replay either edit");
        });
        test("more than 256 live characters have no cap or private draft", [] {
            Fixture f;
            f.open();
            for (unsigned i = 0; i < 600; ++i) { f.tap(0x1000); }
            check(f.fake.field == std::wstring(600, L'a') && f.keyboard.delivery().completed_edits() == 600,
                "live typing must not stop at the preview limit");
            check(f.keyboard.model().text().empty() && !f.keyboard.model().at_limit(), "no lifetime draft/cap");
        });
        test("opening Y consumes held faces but accepts natural release-time input", [] {
            Fixture f;
            f.tick(); f.tick(); f.button(0x8000);
            f.pad().left_y = 28000;
            f.button(0x9000);
            f.button(0x1000);
            check(f.fake.field.empty(), "A held during opening Y must not replay after release");
            f.button(0);
            f.button(0x1000);
            check(f.fake.field == L"a", "a fresh A edge can type while stick remains held");
            Fixture g;
            g.tick(); g.tick(); g.button(0x8000);
            g.pad().left_y = 28000;
            g.button(0x1000);
            check(g.fake.field == L"a", "new face edge on released Y is usable without an extra neutral ceremony");
        });
        test("face holds never repeat and Backspace repeat is bounded", [] {
            Fixture f;
            f.fake.field = std::wstring(30, L'x');
            f.open();
            f.button(0x1000);
            for (unsigned i = 0; i < 30; ++i) { f.tick(); }
            check(f.fake.field.size() == 31, "held face is single edit");
            f.button(0);
            f.button(0x0100);
            check(f.fake.field.size() == 30, "Backspace begins immediately");
            for (unsigned i = 0; i < 24; ++i) { f.tick(); }
            check(f.fake.field.size() == 30, "repeat waits 400 ms");
            f.tick();
            check(f.fake.field.size() == 29, "first repeat at 400 ms");
            f.tick(90);
            check(f.fake.field.size() == 28, "repeat after 90 ms");
            f.tick(500);
            check(f.fake.field.size() == 28 && !f.keyboard.accepts_typing(), "gap cannot catch up missed repeats");
        });
        test("View closes without rollback or pending output", [] {
            Fixture f;
            f.open(); f.tap(0x1000); f.button(0x0020);
            check(!f.keyboard.visible() && f.fake.field == L"a" && !f.keyboard.recovery_required(), "View preserves already sent text");
            f.button(0x1000);
            check(f.keyboard.blocks_pointer(), "held close-time gesture must not replay as a click");
            f.pad().left_y = 0;
            f.button(0);
            check(!f.keyboard.blocks_pointer(), "pointer hold clears after fresh neutral");
        });
        test("queue preserves mixed edit ordering and bounded pump budget", [] {
            Fake f; TextDelivery d(f);
            d.start(f.observation.target);
            for (const auto edit : {letter(U'a'), letter(0x1f600), backspace, letter(U' '), enter}) {
                check(d.enqueue(edit, 1000, 2000), "queue accepts valid operation");
            }
            d.pump(1000);
            check(f.accepted.size() == 8 && d.pending_edits() == 2, "one pump is bounded to eight events");
            d.pump();
            check(d.completed_edits() == 5 && d.pending_edits() == 0, "all queued edits complete in order");
            std::vector<TextEvent> expected;
            for (const auto edit : {letter(U'a'), letter(0x1f600), backspace, letter(U' '), enter}) {
                const auto sequence = edit_events(edit);
                expected.insert(expected.end(), sequence->events.begin(), sequence->events.begin() + sequence->count);
            }
            check(f.accepted == expected, "mixed Unicode and virtual-key sequence cannot reorder");
        });
        test("queue overflow stops explicitly and never silently drops or replays", [] {
            Fake f; TextDelivery d(f); d.start(f.observation.target);
            for (std::size_t i = 0; i < keyboard_edit_queue_limit; ++i) { check(d.enqueue(letter(U'a'), 1000, 2000), "queue slot"); }
            check(!d.enqueue(enter, 1000, 2000) && d.phase() == TextPhase::Stopped, "overflow must be an explicit stop");
            check(d.reason().find(L"full") != std::wstring::npos && d.pending_edits() == 0, "overflow reason and cancellation");
            d.pump();
            check(f.accepted.empty(), "cancelled overflow queue must never replay");
        });
        test("one-event acceptance ledger for every key kind and partial prefix", [] {
            for (const auto edit : {letter(U'a'), letter(0x1f600), backspace, enter}) {
                const auto sequence = edit_events(edit);
                for (unsigned prefix = 0; prefix < sequence->count; ++prefix) {
                    Fake f; TextDelivery d(f); d.start(f.observation.target);
                    for (unsigned i = 0; i < prefix; ++i) { f.returns.push_back(1); }
                    f.returns.push_back(0);
                    d.enqueue(edit, 1000, 2000);
                    d.enqueue(letter(U'z'), 1000, 2000);
                    d.pump();
                    check(d.phase() == TextPhase::Stopped && d.accepted_events() == prefix && d.completed_edits() == 0,
                        "partial or zero acceptance cannot count as an edit");
                    check(d.owned() == ((prefix % 2) != 0), "down/up ownership must follow exact prefix");
                    if (d.owned()) { check(d.owned_key() == sequence->events[prefix - 1].key, "owned key identity is exact"); }
                    d.pump();
                    check(f.attempted.size() == prefix + 1 && !d.enqueue(edit, 1000, 2000), "failed edits cannot retry");
                    d.cleanup();
                    check(f.accepted.size() == prefix + prefix % 2, "only accepted down may be balanced");
                    if (prefix % 2) { check(f.accepted.back().up && f.accepted.back().key == sequence->events[prefix - 1].key,
                        "cleanup must release correct Unicode or virtual key"); }
                    check(d.phase() == TextPhase::Stopped, "cleanup cannot resume typing");
                }
            }
        });
        test("text rejection revokes Auto override once even while keyboard holds routing", [] {
            for (const bool reject_up : {false, true}) {
                PointerFake pointer;
                pointer.observation.allowed = false;
                pointer.observation.block = gamepad::live::BlockReason::Auto;
                pointer.observation.automatic = gamepad::live::AutoReason::Confinement;
                gamepad::live::Live live(pointer);
                check(live.request_desktop(pointer.time) && live.auto_override_active(), "initial explicit override");
                live.hold_for_keyboard(true, pointer.time);
                Fake text; TextDelivery delivery(text);
                delivery.start(text.observation.target);
                if (reject_up) { text.returns.push_back(1); }
                text.returns.push_back(0);
                delivery.enqueue(letter(U'a'), text.now, text.now + 100);
                delivery.pump();
                check(delivery.fault_generation() == 1 && delivery.owned() == reject_up,
                    "each rejected down/up must publish an output fault independent of ownership");
                live.observe_text_fault(delivery.fault_generation(), pointer.time);
                Snapshot snapshot;
                snapshot.sequence = 1; snapshot.sampled_at = pointer.time;
                snapshot.slots[0].connection = Connection::Connected; snapshot.slots[0].connection_epoch = 1;
                live.automatic_step(snapshot, pointer.time, !delivery.recovery_required());
                check(!live.auto_override_active(), "keyboard-held early return must not preserve failed-text override");
                if (reject_up) {
                    check(!live.request_desktop(pointer.time, !delivery.recovery_required()), "pending text up remains hard");
                    delivery.cleanup(true);
                    check(!delivery.owned() && text.accepted.size() == 2 && text.accepted.back().up,
                        "recovery must preserve and balance the accepted text down");
                }
                live.hold_for_keyboard(false, pointer.time);
                pointer.time += 16; snapshot.sampled_at = pointer.time; ++snapshot.sequence;
                live.automatic_step(snapshot, pointer.time, true);
                check(!live.auto_override_active(), "recovery/normal polling must not restore authorization");
                check(live.request_desktop(pointer.time) && live.auto_override_active(), "new explicit request after recovery should work");
                for (unsigned i = 0; i < 4; ++i) { live.observe_text_fault(delivery.fault_generation(), pointer.time); }
                check(live.auto_override_active(), "the same old fault must not revoke a later explicit request every frame");
                check(delivery.start(text.observation.target) && delivery.fault_generation() == 1,
                    "fault generation must survive a new typing session");
                text.returns = {0}; delivery.enqueue(letter(U'b'), text.now, text.now + 100); delivery.pump();
                live.observe_text_fault(delivery.fault_generation(), pointer.time);
                check(delivery.fault_generation() == 2 && !live.auto_override_active(), "a genuinely new failure must revoke again");
            }
        });
        test("uncertain text acceptance revokes override without discarding hard recovery", [] {
            PointerFake pointer;
            pointer.observation.allowed = false;
            pointer.observation.block = gamepad::live::BlockReason::Auto;
            pointer.observation.automatic = gamepad::live::AutoReason::ExclusiveD3D;
            gamepad::live::Live live(pointer);
            check(live.request_desktop(pointer.time), "initial override");
            Fake text; TextDelivery delivery(text);
            delivery.start(text.observation.target);
            delivery.stop(L"Normal View cancellation");
            live.observe_text_fault(delivery.fault_generation(), pointer.time);
            check(delivery.fault_generation() == 0 && live.auto_override_active(), "ordinary cancellation must not become an output fault");
            delivery.start(text.observation.target); delivery.enqueue(enter, text.now, text.now + 100);
            text.returns = {2}; delivery.pump();
            live.observe_text_fault(delivery.fault_generation(), pointer.time);
            check(delivery.uncertain() && delivery.fault_generation() == 1 && !live.auto_override_active(),
                "uncertain acceptance must signal and revoke without inventing a known down");
            delivery.cleanup(true);
            check(!live.request_desktop(pointer.time, !delivery.recovery_required())
                && delivery.recovery_required(), "explicit request cannot bypass unresolved uncertainty");
        });
        test("full edit completes only after every accepted event", [] {
            Fake f; TextDelivery d(f); d.start(f.observation.target); d.enqueue(letter(0x1f600), 1000, 2000);
            for (unsigned i = 0; i < 4; ++i) {
                d.pump(1);
                check(d.accepted_events() == i + 1 && d.completed_edits() == (i == 3 ? 1u : 0u), "exact completion");
                check(d.owned() == (i % 2 == 0), "surrogate ownership");
            }
            d.pump();
            check(f.accepted.size() == 4, "completed edit cannot repeat");
        });
        test("physical overlap never masked by accepted synthetic async state", [] {
            for (const auto kind : {TextKeyKind::Unicode, TextKeyKind::Backspace, TextKeyKind::Enter}) {
                const TextKey owned{kind, kind == TextKeyKind::Unicode ? std::uint16_t{L'a'} : std::uint16_t{0}};
                const auto key = gamepad::live::text_virtual_key(owned);
                check(!gamepad::live::text_key_conflict(key, true, false, owned), "only exact accepted synthetic state may be masked");
                check(gamepad::live::text_key_conflict(key, true, true, owned), "real physical overlap remains conflicting");
                check(gamepad::live::text_key_conflict(key, false, true, owned), "raw physical evidence must survive async clearing");
                check(gamepad::live::text_key_conflict(VK_SHIFT, true, false, owned), "other keys never masked");
                check(gamepad::live::text_key_conflict(key, true, false, {}), "unowned keys never masked");
            }
        });
        test("all foreground focus identity and revision changes reject queued edits", [] {
            for (unsigned part = 0; part < 12; ++part) {
                Fake f; TextDelivery d(f); d.start(f.observation.target); d.enqueue(backspace, 1000, 2000);
                if (part < 9) {
                    auto& w = f.observation.target.windows[part / 3];
                    if (part % 3 == 0) { ++w.window; }
                    if (part % 3 == 1) { ++w.process; }
                    if (part % 3 == 2) { ++w.thread; }
                }
                if (part == 9) { ++f.observation.target.activity; }
                if (part == 10) { ++f.observation.target.context; }
                if (part == 11) { ++f.observation.target.focus_revision; }
                d.pump(); check(f.accepted.empty() && d.phase() == TextPhase::Stopped, "mismatch cannot redirect edit");
            }
        });
        test("focus notifications distinguish repeats and unrelated windows from actual field changes", [] {
            using namespace gamepad::live;
            Fixture f;
            FocusTracker focus;
            focus.observe(f.fake.observation.target.windows[0], f.fake.observation.target.windows[2]);
            const FocusEvent same{f.fake.observation.target.windows[2], OBJID_CLIENT, CHILDID_SELF};
            check(focus.notify(same, true), "initial semantic focus must establish identity");
            f.fake.observation.target.focus_revision = focus.revision();
            f.open();
            const auto captured = focus.revision();
            for (unsigned i = 0; i < 8; ++i) {
                f.tap(0x1000);
                check(!focus.notify(same, true), "same focused object notification must be idempotent");
                auto other = same; ++other.window.window;
                check(!focus.notify(other, false), "unrelated/background focus must not change target");
                f.fake.observation.target.focus_revision = focus.revision();
                f.tick();
                check(f.keyboard.accepts_typing() && focus.revision() == captured,
                    "notification-only refresh dismissed keyboard");
            }
            check(f.fake.field == L"aaaaaaaa", "successive edits must all reach the original field");
            auto changed = same; ++changed.child;
            check(focus.notify(changed, true), "same-HWND semantic focus change must invalidate");
            f.fake.observation.target.focus_revision = focus.revision();
            f.tick();
            check(!f.keyboard.visible(), "actual semantic focus change must dismiss");
            check(f.keyboard.last_dismissal() == L"Focused accessibility object changed",
                "specific content-free dismissal reason must survive closing the popup");
            f.keyboard.dismiss();
            check(f.keyboard.last_dismissal() == L"Focused accessibility object changed",
                "idle cleanup must not overwrite the original dismissal reason");
            check(focus.observe({90, 9, 9}, {91, 9, 9}), "native target change must invalidate");
            check(!focus.observe({90, 9, 9}, {91, 9, 9}), "stable native target must not invalidate repeatedly");
        });
        test("caret presentation is not routing identity while menus and capture guards remain", [] {
            using namespace gamepad::live;
            for (const DWORD flags : {DWORD{0}, DWORD{GUI_INMENUMODE}, DWORD{GUI_INMOVESIZE},
                DWORD{GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE}, DWORD{0x8000}}) {
                check(routing_flags(flags) == flags && routing_flags(flags | GUI_CARETBLINKING) == flags,
                    "caret-only change must not alter safety flags");
            }
        });
        test("queued focus excursions invalidate even after the original native target returns", [] {
            using namespace gamepad::live;
            for (const bool foreground : {false, true}) {
                Fixture f;
                FocusTracker focus;
                focus.observe(f.fake.observation.target.windows[0], f.fake.observation.target.windows[2]);
                const FocusEvent original{f.fake.observation.target.windows[2], OBJID_CLIENT, CHILDID_SELF};
                focus.notify(original, true);
                f.fake.observation.target.focus_revision = focus.revision();
                f.open();
                if (foreground) { focus.invalidate(); }
                else {
                    auto sibling = original; ++sibling.window.window;
                    focus.notify(sibling, true);
                }
                focus.notify(original, true);
                check(!focus.observe(f.fake.observation.target.windows[0], f.fake.observation.target.windows[2]),
                    "native snapshot returns to the original field");
                f.fake.observation.target.focus_revision = focus.revision();
                f.button(0x1000);
                check(!f.keyboard.visible() && f.fake.accepted.empty(),
                    "an original-field snapshot must not authorize text across a focus excursion");
            }
        });
        test("raw controller navigation is not physical keyboard input", [] {
            using namespace gamepad::live;
            for (unsigned key = 1; key < 256; ++key) {
                check(raw_keyboard_activity(true, key, output_tag), "device-backed input must never be hidden by a tag");
                check(!raw_keyboard_activity(false, key, output_tag), "own virtual input must not cause takeover");
            }
            for (unsigned key = VK_NAVIGATION_VIEW; key <= VK_NAVIGATION_CANCEL; ++key) {
                check(!raw_keyboard_activity(false, key, 0), "navigation synthesis must not become physical input");
            }
            for (unsigned key = VK_GAMEPAD_A; key <= VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT; ++key) {
                check(!raw_keyboard_activity(false, key, 0), "gamepad synthesis must not become physical input");
            }
            check(raw_keyboard_activity(false, VK_LEFT, 0) && raw_keyboard_activity(false, 'A', 0),
                "ordinary virtual keyboard and arrow keys retain takeover semantics");
        });
        test("accepted text release settling defers edits without dismissing the keyboard", [] {
            Fixture f; f.open();
            f.fake.during_send = [&](TextEvent event) { if (event.up) { f.fake.observation.settling = true; } };
            f.tap(0x1000);
            f.tap(0x2000);
            check(f.fake.field == L"a" && f.keyboard.visible() && f.keyboard.delivery().pending_edits() == 1,
                "settling must preserve a bounded queued edit and keyboard session");
            f.fake.during_send = {};
            f.fake.observation.settling = false;
            f.tick();
            check(f.fake.field == L"ab" && f.keyboard.accepts_typing(), "cleared async state must resume queued edit once");
            f.tick();
            check(f.fake.field == L"ab", "settled edit must not replay");
        });
        test("text release settling never bypasses physical focus or expiry guards", [] {
            for (unsigned fault = 0; fault < 3; ++fault) {
                Fake f; TextDelivery d(f); d.start(f.observation.target); d.enqueue(letter(U'a'), 1000, 1100);
                f.observation.settling = true; d.pump();
                check(f.accepted.empty() && d.phase() == TextPhase::Ready, "settling alone must defer");
                if (fault == 0) { f.observation.physical_held = true; }
                if (fault == 1) { ++f.observation.target.focus_revision; }
                if (fault == 2) { f.now = 1101; }
                d.pump();
                check(f.accepted.empty() && d.phase() == TextPhase::Stopped, "settling masked a hard guard");
                f.observation.settling = f.observation.physical_held = false; d.pump();
                check(f.accepted.empty(), "rejected edit must not replay after settling clears");
            }
        });
        test("game protected physical and missing focus gates prevent output", [] {
            for (unsigned gate = 0; gate < 5; ++gate) {
                Fake f; TextDelivery d(f); d.start(f.observation.target); d.enqueue(enter, 1000, 2000);
                if (gate == 0) { f.observation.allowed = gamepad::live::process_integrity_allowed(0x2000, 0x3000, false); }
                if (gate == 1) {
                    gamepad::live::GameHandoff game;
                    f.observation.allowed = game.update(true, 10, gamepad::live::NotificationState::ExclusiveD3D, false).empty();
                }
                if (gate == 2) { f.observation.physical_held = true; }
                if (gate == 3) { f.observation.stable = false; }
                if (gate == 4) { f.observation.target.windows[2] = {}; }
                d.pump(); check(f.accepted.empty(), "unsafe gate cannot emit Enter");
            }
        });
        test("final target check and queued edit expiry prevent delayed output", [] {
            for (const bool expire : {false, true}) {
                Fake f; TextDelivery d(f); d.start(f.observation.target); d.enqueue(letter(U'a'), 1000, 1100);
                unsigned observations = 0;
                f.during_observe = [&] { if (++observations == 2) {
                    if (expire) { f.now = 1101; } else { ++f.observation.target.focus_revision; }
                } };
                d.pump(); check(f.accepted.empty() && d.phase() == TextPhase::Stopped, "final target/expiry check");
            }
        });
        test("reentrant cancellation records accepted key before stopping", [] {
            for (const auto edit : {letter(U'a'), backspace, enter}) {
                Fake f; TextDelivery d(f); d.start(f.observation.target); d.enqueue(edit, 1000, 2000);
                f.during_send = [&](TextEvent) { d.stop(L"Pause"); d.pump(); d.cleanup(true); check(!d.start(f.observation.target), "no in-flight reset"); };
                d.pump(); check(d.owned() && d.accepted_events() == 1 && f.attempted.size() == 1, "reentrant accepted down");
                f.during_send = {}; d.cleanup();
                check(f.accepted.size() == 2 && f.accepted.back().up && !d.owned(), "release only exact outstanding key");
            }
        });
        test("reentrant validation stop sends nothing and queue reentry is explicit", [] {
            Fake f; TextDelivery d(f); d.start(f.observation.target); d.enqueue(letter(U'a'), 1000, 2000);
            f.during_observe = [&] { check(!d.enqueue(enter, 1000, 2000), "reentrant queue write rejected"); };
            d.pump(); check(f.accepted.empty() && d.phase() == TextPhase::Stopped, "reentry must not corrupt queue");
        });
        test("bounded cleanup physical protection and impossible acceptance", [] {
            Fake f; TextDelivery d(f); d.start(f.observation.target); d.enqueue(enter, 1000, 2000); d.pump(1); d.stop(L"Stop");
            f.observation.physical_held = true;
            for (unsigned i = 0; i < 10; ++i) { f.now += 300; d.cleanup(); }
            check(d.owned() && f.accepted.size() == 1, "physical overlap blocks even cleanup");
            f.observation.physical_held = false; d.cleanup();
            check(d.owned(), "automatic cleanup budget is finite");
            f.observation.cleanup_allowed = false; d.cleanup(true);
            check(d.owned(), "explicit recovery cannot bypass protected desktop");
            f.observation.cleanup_allowed = true; d.cleanup(true);
            check(!d.owned() && f.accepted.back().key.kind == TextKeyKind::Enter, "explicit up recovery");
            check(d.start(f.observation.target), "new deliberate session can start after safe recovery");
            d.enqueue(backspace, f.now, f.now + 100); f.returns = {2}; d.pump();
            check(d.uncertain() && !d.start(f.observation.target), "impossible count latches restart-only uncertainty");
        });
        test("invalid Unicode and edit payload stop before output", [] {
            for (const auto edit : {letter(0), letter(U'\n'), letter(0x7f), letter(0xd800), letter(0x110000),
                KeyboardEdit{KeyboardEditKind::Backspace, U'x'}}) {
                Fake f; TextDelivery d(f); d.start(f.observation.target);
                check(!d.enqueue(edit, 1000, 2000) && d.phase() == TextPhase::Stopped, "invalid edit rejected explicitly");
                d.pump(); check(f.accepted.empty(), "invalid scalar or VK payload cannot emit");
            }
        });
        test("focus and physical takeover dismiss immediately without rolling back text", [] {
            for (unsigned gate = 0; gate < 4; ++gate) {
                Fixture f; f.open(); f.tap(0x1000);
                if (gate == 0) { ++f.fake.observation.target.windows[2].window; }
                if (gate == 1) { ++f.fake.observation.target.activity; }
                if (gate == 2) { f.eligibility.desktop = false; }
                if (gate == 3) { f.fake.observation.allowed = false; }
                f.button(0x2000);
                check(!f.keyboard.accepts_typing() && f.fake.field == L"a", "loss cannot redirect next edit");
                if (gate != 2) { check(!f.keyboard.visible(), "normal external loss must hide without waiting for View"); }
                f.button(0); f.button(0x0020);
                check(!f.keyboard.visible(), "View works even after target loss");
            }
        });
        test("target loss during final send validation hides and discards both queued edits", [] {
            Fixture f; f.open(); f.button(0x0010);
            unsigned observations = 0;
            f.fake.during_observe = [&] {
                if (++observations == 3) { ++f.fake.observation.target.windows[2].window; }
            };
            f.button(0x1000);
            check(!f.keyboard.visible() && f.fake.accepted.empty()
                && f.keyboard.delivery().pending_edits() == 0, "final target check hides and cancels Enter plus character");
            f.fake.during_observe = {};
            f.button(0); f.button(0x1000);
            check(f.fake.accepted.empty(), "dismissed context cannot replay pending edits");
        });
        test("automatic hide preserves accepted key-up recovery and pointer hold", [] {
            Fixture f; f.open();
            f.fake.during_send = [&](TextEvent event) {
                if (!event.up) {
                    ++f.fake.observation.target.activity;
                    f.fake.observation.physical_held = true;
                    f.keyboard.dismiss(L"Physical takeover");
                }
            };
            f.button(0x1000);
            check(!f.keyboard.visible() && f.keyboard.recovery_required() && f.keyboard.blocks_pointer()
                && f.fake.accepted.size() == 1 && f.fake.field == L"a", "hide must retain accepted down and sent character");
            f.fake.during_send = {};
            f.pad().left_y = 0; f.button(0);
            check(f.keyboard.blocks_pointer() && f.fake.accepted.size() == 1, "physical overlap still blocks pending up");
            f.fake.observation.physical_held = false;
            f.keyboard.recover();
            check(!f.keyboard.recovery_required() && f.fake.accepted.size() == 2
                && f.fake.accepted.back().up && f.fake.field == L"a", "recovery balances only the owned up, never text replay");
        });
        test("startup combined stale and reconnected Y never open unsafely", [] {
            for (unsigned kind = 0; kind < 5; ++kind) {
                Fixture f;
                if (kind) { f.tick(); f.tick(); }
                if (kind == 1) { f.pad().left_x = 28000; }
                if (kind == 2) { f.pad().right_trigger = 255; }
                if (kind == 3) { ++f.pad().connection_epoch; }
                if (kind == 4) { f.fake.now += 200; }
                f.button(0x8000);
                check(!f.keyboard.visible() && f.keyboard.blocks_pointer() && f.fake.accepted.empty(), "unsafe Y consumed");
            }
        });
        test("multiple pads and preferences cannot select or impersonate invoker", [] {
            Fixture f; f.snapshot.slots[1] = f.pad(); f.tick(); f.tick();
            f.snapshot.slots[1].buttons = 0x8000; f.button(0x8000);
            check(!f.keyboard.visible(), "multiple Y ambiguity must not choose");
            f.snapshot.slots[1].buttons = 0; f.button(0); f.tick(); f.button(0x8000); f.button(0);
            f.snapshot.slots[1].left_y = 28000; f.snapshot.slots[1].buttons = 0x1000; f.tick();
            check(f.fake.accepted.empty() && f.keyboard.model().selected_slot() == 0u, "other pad cannot type");
            Fixture g; g.eligibility.preferred = 1; g.tick(); g.tick(); g.button(0x8000);
            check(!g.keyboard.visible(), "controller preference must govern invocation");
        });
        test("disconnect does not retarget and reconnected View can dismiss", [] {
            Fixture f; f.open(); f.tap(0x1000); f.pad().connection = Connection::Disconnected; f.tick();
            check(!f.keyboard.accepts_typing(), "disconnect stops session");
            f.pad().connection = Connection::Connected; ++f.pad().connection_epoch; f.button(0x0020);
            check(f.keyboard.visible(), "reconnect-held View is not a fresh edge");
            f.button(0); f.button(0x0020);
            check(!f.keyboard.visible() && f.fake.field == L"a", "new View safely closes without rollback");
        });
        test("Menu View chord closes without Enter or character", [] {
            Fixture f; f.open(); f.button(0x0030);
            check(!f.keyboard.visible() && f.fake.accepted.empty(), "mode chord takes priority over Enter");
        });
        test("reentrant close and opening pause preserve ownership", [] {
            Fixture f; f.open();
            f.fake.during_send = [&](TextEvent) { f.keyboard.dismiss(); f.keyboard.recover(); f.keyboard.step(f.snapshot, f.fake.now, f.eligibility); };
            f.button(0x1000);
            check(!f.keyboard.visible() && f.keyboard.recovery_required() && f.fake.accepted.size() == 1, "in-flight close records down");
            f.fake.during_send = {}; f.tick();
            check(f.fake.accepted.size() == 2 && f.fake.accepted.back().up, "only cleanup follows close");
            Fixture g; g.tick(); g.tick(); g.fake.during_observe = [&] { g.keyboard.block(L"Pause"); }; g.button(0x8000);
            check(!g.keyboard.visible(), "pause during target capture must prevent invocation");
        });
        test("software dispatch timing accounts for validation without an extra UI tick", [] {
            Fake f; TextDelivery d(f); d.start(f.observation.target);
            f.during_observe = [&] { f.now += 3; };
            d.enqueue(letter(U'a'), 1000, 1100); d.pump();
            check(d.last_dispatch_ms() == 6 && d.maximum_dispatch_ms() == 6 && f.field == L"a",
                "software sample-to-dispatch measurement includes both checks");
            std::cout << "Software fake-clock dispatch: 6 ms including two 3 ms guards; zero added UI ticks\n";
        });
        test("pointer hold preserves mode chord and fresh rearming", [] {
            PointerFake f; gamepad::live::Live live(f); Snapshot s;
            s.slots[0].connection = Connection::Connected; s.slots[0].connection_epoch = 1;
            const auto tick = [&] { f.time += 16; s.sampled_at = f.time; ++s.sequence; live.automatic_step(s, f.time, true); };
            tick(); tick(); live.hold_for_keyboard(true, f.time);
            s.slots[0].buttons = 0x1000; s.slots[0].left_x = s.slots[0].right_y = 30000; tick();
            check(f.events.empty() && !live.automatically_paused(), "temporary hold never changes manual preference or emits");
            live.hold_for_keyboard(false, f.time); tick(); tick();
            check(f.events.empty(), "stale gesture cannot replay on close");
            s.slots[0].buttons = 0; s.slots[0].left_x = s.slots[0].right_y = 0; tick(); tick();
            s.slots[0].buttons = 0x1000; tick();
            check(live.owned() == gamepad::live::Primary, "fresh click after neutral is usable");
            live.hold_for_keyboard(true, f.time);
            check(live.owned() == 0 && f.events.back().kind == gamepad::live::OutputKind::PrimaryUp, "hold releases prior accepted drag");
            s.slots[0].buttons = 0; tick(); s.slots[0].buttons = 0x0030; tick();
            check(live.automatically_paused(), "mode chord works while keyboard holds pointer");
            live.hold_for_keyboard(false, f.time); tick();
            check(live.automatically_paused(), "close cannot undo explicit pause or held chord");
        });
        test("set_input_mode safely dismisses a visible keyboard without editing or discarding sent text", [] {
            Fixture f;
            f.open();
            f.button(0x1000);
            check(f.fake.field == L"a", "baseline FaceButtons typing works before any mode switch");
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            check(!f.keyboard.visible(), "mode switch must close a visible keyboard rather than corrupt its gesture");
            check(f.fake.field == L"a", "mode switch must not edit or discard already-sent text");
            check(f.keyboard.model().input_mode() == KeyboardInputMode::OneHanded, "mode takes effect immediately");
            f.tick();
            check(!f.keyboard.visible(), "switching mode alone must not itself reopen the keyboard");
            f.keyboard.set_input_mode(KeyboardInputMode::FaceButtons); // no-op path: closed already, same mode toggled back.
            check(f.keyboard.model().input_mode() == KeyboardInputMode::FaceButtons, "mode can freely switch back while closed");
        });
        test("left-stick click opens the keyboard only in OneHanded mode; Y still opens in every mode", [] {
            Fixture f;
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            f.tick(); f.tick();
            f.button(0x0040);
            check(f.keyboard.visible() && f.fake.accepted.empty(), "left-stick click opens the keyboard in OneHanded mode without output");
            Fixture g;
            g.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            g.tick(); g.tick();
            g.button(0x8000);
            g.button(0);
            check(g.keyboard.visible(), "Y still opens the keyboard in OneHanded mode");
            Fixture h; // FaceButtons default: a bare stick click must not open the keyboard.
            h.tick(); h.tick();
            h.button(0x0040);
            h.button(0);
            check(!h.keyboard.visible(), "left-stick click must not open the keyboard in FaceButtons mode");
        });
        test("mode survives an LS-open, real flicked character, View dismiss, and LS-reopen cycle", [] {
            Fixture f;
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            f.tick(); f.tick();
            f.button(0x0040);
            f.button(0);
            check(f.keyboard.visible(), "left-stick click should open the keyboard");
            f.flick(0.0, 180.0); // group 0 (Up), A (bottom) face -> a real typed character.
            check(!f.fake.field.empty(), "the two-stage flick must reach the target as a real character");
            const auto typed = f.fake.field;
            f.button(0x0020); // View dismisses.
            check(!f.keyboard.visible(), "View must dismiss the keyboard");
            check(f.keyboard.model().input_mode() == KeyboardInputMode::OneHanded,
                "input mode must survive the dismiss/reset cycle, not revert to FaceButtons");
            f.button(0);
            f.tick(); f.tick();
            f.button(0x0040);
            f.button(0);
            check(f.keyboard.visible(), "left-stick click must still reopen after a prior dismiss");
            f.flick(45.0, 90.0); // a different group/face -> flick-typing must still work post-reopen.
            check(f.fake.field.size() > typed.size(), "flick-typing must work again immediately after LS-reopen");
        });
        test("Menu Enter, a completing flick, and a fresh RB in one sample never overflow the edit buffer", [] {
            Fixture f;
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            f.tick(); f.tick();
            f.button(0x0040); f.button(0);
            check(f.keyboard.visible(), "left-stick click should open the keyboard");
            f.aim(0.0); f.tick();
            f.center_stick(); f.tick();
            check(f.keyboard.model().flick_stage() == KeyboardFlickStage::Character, "group commit should reach Character stage");
            f.aim(90.0); f.tick(); // outward toward B (right); candidate engaged but not yet committed.
            f.pad().buttons = 0x0010; f.tick(); // hold Menu while the candidate remains engaged.
            const auto before = f.keyboard.delivery().completed_edits();
            f.center_stick();                    // recenter would normally commit the flicked character now...
            f.pad().buttons = 0x0200;            // ...but Menu also releases (Enter) and RB is freshly pressed here.
            f.tick();
            const auto emitted = f.keyboard.delivery().completed_edits() - before;
            check(emitted <= 2, "a single sample must never emit more than two edits");
            check(f.keyboard.model().flick_stage() == KeyboardFlickStage::Group,
                "the completing candidate must still be consumed back to Group, not stalled");
            check(!f.keyboard.model().selected_face(), "a suppressed candidate must not remain engaged");
            check(f.fake.field == L"\r\n ", "priority order must be Enter then the physical RB space, with the flick discarded");
            f.pad().buttons = 0; f.tick();
            f.flick(45.0, 90.0);
            check(f.fake.field.size() == 4, "the discarded candidate must not silently replay on a later sample");
        });
        test("a fresh LB press suppresses a same-sample completing flick candidate without a crash", [] {
            Fixture f;
            f.fake.field = L"seed";
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            f.tick(); f.tick();
            f.button(0x0040); f.button(0);
            f.aim(0.0); f.tick();
            f.center_stick(); f.tick();
            check(f.keyboard.model().flick_stage() == KeyboardFlickStage::Character, "group commit should reach Character stage");
            f.aim(180.0); f.tick(); // outward toward A (bottom); candidate engaged, not yet committed.
            f.center_stick();
            f.pad().buttons = 0x0100; // fresh LB in the same sample the candidate would complete.
            f.tick();
            check(f.fake.field == L"see", "a fresh LB press must still erase exactly once, winning over the completing flick");
            check(f.keyboard.model().flick_stage() == KeyboardFlickStage::Group,
                "the suppressed candidate must still resolve back to Group rather than stall");
            check(!f.keyboard.model().selected_face(), "a suppressed candidate must not remain engaged");
            f.pad().buttons = 0; f.tick();
            f.flick(45.0, 90.0);
            check(f.fake.field.size() > 3, "typing must still work normally after the suppressed collision");
        });
        test("View cancellation wins over an engaged one-handed candidate without leaving stale state", [] {
            Fixture f;
            f.keyboard.set_input_mode(KeyboardInputMode::OneHanded);
            f.tick(); f.tick();
            f.button(0x0040); f.button(0);
            f.aim(0.0); f.tick();
            f.center_stick(); f.tick();
            f.aim(90.0); f.tick(); // candidate engaged, not yet committed.
            f.button(0x0020); // View cancels.
            check(!f.keyboard.visible(), "View must cancel even mid-gesture without a crash");
            check(f.keyboard.model().input_mode() == KeyboardInputMode::OneHanded, "mode must still be preserved after a View cancel");
        });
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
    std::cout << count << " live-edit cases passed\n";
}
