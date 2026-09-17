#pragma once

#include "Pointer.h"

#include <string>
#include <vector>

namespace gamepad::live {

enum class Mode { Pointer, Menu, Blocked };
enum class BlockReason {
    None, Infrastructure, DesktopSession, Routing, Recipient, Exclusion, Surface,
    Geometry, SwappedButtons, Physical, Settling, Auto, AutoUnavailable, AutoNotPresent, TextOutput
};
enum class InputCategory { None, Keyboard, Modifier, Mouse, ReservedGamepad, ReservedNavigation, Owned };
[[nodiscard]] InputCategory input_category(unsigned key, bool owned = false);
[[nodiscard]] std::wstring input_diagnostic_text(InputCategory category, bool raw, bool asynchronous, bool settling);
enum class AutoReason { None, ExclusiveD3D, Presentation, Confinement, NotPresent, Unavailable };
enum class EffectiveMode { Desktop, Physical, ManualNative, AutoNative, Blocked };
struct AppIdentity {
    std::uintptr_t root_owner = 0;
    std::uint32_t process = 0;
    bool operator==(const AppIdentity&) const = default;
};
struct AppActivation {
    AppIdentity identity;
    std::uint64_t generation = 0;
    bool operator==(const AppActivation&) const = default;
};
[[nodiscard]] bool known_app(AppIdentity app);
[[nodiscard]] bool heuristic_auto(AutoReason reason);
[[nodiscard]] std::wstring_view block_reason_text(BlockReason reason);
[[nodiscard]] bool physical_async_key(unsigned key);
[[nodiscard]] bool confinement_candidate(std::uint32_t foreground, std::uint32_t shell, std::uint32_t self);
enum class KeyState { Clear, Physical, Settling };
class ReleasedKeyState {
public:
    void accepted_up(unsigned key, Tick now);
    KeyState observe(unsigned key, bool asynchronous, bool physical, Tick now);
private:
    std::array<std::optional<Tick>, 256> released_{};
};
enum Held : unsigned { Primary = 1, Secondary = 2, Escape = 4, Other = 8 };
enum class OutputKind { Move, Wheel, PrimaryDown, PrimaryUp, SecondaryDown, SecondaryUp, EscapeDown, EscapeUp };
struct Output {
    OutputKind kind;
    Point point{};
    int wheel = 0;
};

using ExecutableExclusions = std::vector<std::wstring>;
[[nodiscard]] std::wstring_view validate_exclusions(const ExecutableExclusions& names);
[[nodiscard]] bool executable_excluded(std::wstring_view basename, const ExecutableExclusions& names);
// Values match the documented QUERY_USER_NOTIFICATION_STATE, not a game identity API.
enum class NotificationState { NotPresent = 1, Busy, ExclusiveD3D, Presentation, Available, QuietTime, StoreApp };
[[nodiscard]] std::wstring_view notification_pause_reason(NotificationState state);
[[nodiscard]] bool fullscreen_pointer_confined(Rect window, Rect monitor, Rect clip, Rect desktop);

// A confined fullscreen pointer is evidence for this foreground activation, not a permanent app identity.
class GameHandoff {
public:
    void foreground_changed(std::uintptr_t foreground);
    void reset();
    std::wstring_view update(bool automatic, std::uintptr_t foreground, NotificationState state, bool confined_fullscreen);
private:
    std::uintptr_t foreground_ = 0;
    bool pointer_game_ = false;
};

// These observations never come from the M0 simulated Gates.
struct Observation {
    bool available = false;
    bool allowed = false;
    bool baseline_known = false;
    bool cleanup_allowed = false;
    unsigned physical_held = 0;
    std::uint64_t context = 0; // Unsafe boundaries, NOT ordinary focus/menu changes.
    std::uint64_t activity = 0;
    std::uint64_t routing = 0;
    bool routing_stable = true;
    bool routing_pending = false; // Route query unavailable; never authorizes output.
    Mode mode = Mode::Blocked;
    Point cursor{};
    Rect bounds{};
    Rect desktop{};
    Tick expires = 0;
    std::wstring_view reason = L"Live observers unavailable";
    BlockReason block = BlockReason::None;
    AutoReason automatic = AutoReason::None;
    AppActivation app;
    bool auto_overridden = false;
};

[[nodiscard]] bool same_context(const Observation& a, const Observation& b);
[[nodiscard]] unsigned physical_holds(unsigned asynchronous, unsigned observed_physical, unsigned owned);
[[nodiscard]] bool process_integrity_allowed(unsigned host, unsigned target, bool ui_access);
[[nodiscard]] bool mouse_report_activity(bool absolute, long x, long y, unsigned buttons);
[[nodiscard]] bool routing_query_disappeared(unsigned error, bool missing_window);

class OutputBoundary {
public:
    virtual ~OutputBoundary() = default;
    // The intent selects the actual destination for motion, or the key/mouse
    // recipient for other events. Native observations recheck after token/session calls.
    virtual Observation observe(unsigned owned, const Output* intent = nullptr) = 0;
    virtual Tick now() const = 0;
    // Exactly ONE event per call: acceptance is 0 or 1, never an assumed batch prefix.
    virtual unsigned send(const Output& output, Rect desktop) = 0;
    [[nodiscard]] const ExecutableExclusions& exclusions() const { return exclusions_; }
    bool request_desktop(const Observation& expected, Tick expires);
    void clear_auto_override();
    [[nodiscard]] bool auto_override_active() const;

protected:
    AppActivation observe_activation(AppIdentity identity);
    void reset_activation();
    void apply_auto_policy(Observation& observation);

private:
    friend class Live;
    ExecutableExclusions exclusions_;
    AppActivation activation_;
    std::optional<AppActivation> override_;
};

enum class Emission { Accepted, Deferred, Blocked };

class Dispatcher {
public:
    explicit Dispatcher(OutputBoundary& boundary) : boundary_(boundary) {}
    Emission emit(const Output& output, const Observation& expected,
                  std::optional<Tick> routing_wait_since = {});
    void cancel(Tick now);
    void retry(Tick now);
    void recover(Tick now);
    [[nodiscard]] unsigned owned() const { return owned_; }
    [[nodiscard]] bool faulted() const { return faulted_; }
    [[nodiscard]] bool uncertain() const { return uncertain_; }

private:
    Emission release(OutputKind kind, unsigned bit);
    void cleanup(Tick now);
    OutputBoundary& boundary_;
    unsigned owned_ = 0;
    bool faulted_ = false;
    bool uncertain_ = false;
    unsigned attempts_ = 0;
    Tick retry_at_ = 0;
};

class Live {
public:
    explicit Live(OutputBoundary& boundary) : boundary_(boundary), output_(boundary) {}
    void select(std::optional<unsigned> slot, const Snapshot& snapshot, Tick now);
    void enable(Tick now);
    void pause(Tick now, std::wstring_view reason = L"Input paused; fresh neutral required");
    void invalidate(Tick now, std::wstring_view reason);
    void physical_takeover(Tick now);
    void recover(Tick now);
    void step(const Snapshot& snapshot, Tick now);
    void automatic_step(const Snapshot& snapshot, Tick now, bool prerequisites);
    void suspend_automatic(Tick now, std::wstring_view reason = L"Native gamepad input; Menu+View switches back to desktop control");
    void resume_automatic(Tick now);
    bool request_desktop(Tick now, bool prerequisites = true);
    void observe_text_fault(std::uint64_t generation, Tick now);
    [[nodiscard]] std::uint64_t desktop_requests() const { return desktop_requests_; }
    [[nodiscard]] EffectiveMode effective_mode() const { return effective_; }
    [[nodiscard]] BlockReason blocked_reason() const { return blocked_; }
    [[nodiscard]] bool auto_override_active() const { return boundary_.auto_override_active(); }
    void hold_for_keyboard(bool held, Tick now);
    [[nodiscard]] bool keyboard_held() const { return keyboard_held_; }
    [[nodiscard]] std::wstring_view prefer_slot(std::optional<unsigned> slot, Tick now);
    [[nodiscard]] bool automatically_paused() const { return automatic_paused_; }
    [[nodiscard]] std::optional<unsigned> preferred_slot() const { return preferred_; }
    [[nodiscard]] std::wstring_view set_pointer_settings(const PointerSettings& settings, Tick now);
    [[nodiscard]] std::wstring_view set_exclusions(const ExecutableExclusions& names, Tick now);
    [[nodiscard]] const PointerSettings& pointer_settings() const { return settings_; }
    [[nodiscard]] const ExecutableExclusions& exclusions() const { return boundary_.exclusions(); }
    [[nodiscard]] std::optional<unsigned> selected_slot() const { return slot_; }
    // Neutral/eligibility alone never claims controller use; cleanup ups do not count.
    [[nodiscard]] bool controller_active() const { return controller_active_; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] bool armed() const { return armed_; }
    [[nodiscard]] unsigned owned() const { return output_.owned(); }
    [[nodiscard]] bool faulted() const { return output_.faulted(); }
    [[nodiscard]] std::wstring_view status() const { return reason_; }

private:
    bool handle_mode_chord(const Snapshot& snapshot, Tick now, bool prerequisites);
    void record_effective(const Observation& observation, bool prerequisites = true);
    bool activate_from_neutral(unsigned slot, const Snapshot& neutral, const Observation& observation, Tick now);
    void disarm(Tick now, std::wstring_view reason);
    void defer(Tick now);
    bool emit(Output output, const Observation& observation, Tick now);
    OutputBoundary& boundary_;
    Dispatcher output_;
    Pointer pointer_;
    PointerSettings settings_;
    std::optional<unsigned> slot_;
    std::optional<std::uint64_t> epoch_;
    std::optional<Observation> previous_;
    std::optional<Tick> routing_wait_since_;
    std::uint64_t sequence_ = 0;
    Tick sampled_at_ = 0;
    Tick arm_after_ = 0;
    bool enabled_ = false;
    bool armed_ = false;
    bool controller_active_ = false;
    bool physical_owner_ = false;
    std::uint16_t buttons_ = 0;
    std::array<std::optional<Snapshot>, slot_count> automatic_neutral_;
    std::optional<Observation> automatic_observation_;
    std::optional<unsigned> preferred_;
    std::uint64_t automatic_sequence_ = 0;
    Tick automatic_sampled_at_ = 0;
    bool automatic_paused_ = false;
    bool keyboard_held_ = false;
    std::uint64_t desktop_requests_ = 0;
    std::uint64_t text_fault_generation_ = 0;
    EffectiveMode effective_ = EffectiveMode::Desktop;
    BlockReason blocked_ = BlockReason::None;
    std::optional<Snapshot> chord_snapshot_;
    bool chord_released_ = false;
    std::wstring_view reason_ = L"Automatic - center a controller, then move a stick";
};

} // namespace gamepad::live
