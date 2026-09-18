#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gamepad {

using Tick = std::uint64_t; // Monotonic milliseconds, never wall-clock time.
constexpr unsigned slot_count = 4;
constexpr std::uint16_t a_button = 0x1000;
constexpr std::uint16_t standard_buttons = 0xf3ff; // Excludes reserved/Guide bits.
constexpr Tick maximum_sample_age_ms = 250;

enum class Connection { Unknown, Connected, Disconnected, Error };

struct Controller {
    Connection connection = Connection::Unknown;
    std::uint32_t error = 0;
    std::uint64_t connection_epoch = 0;
    std::uint32_t packet = 0;
    std::uint16_t buttons = 0;
    std::uint8_t left_trigger = 0;
    std::uint8_t right_trigger = 0;
    std::int16_t left_x = 0;
    std::int16_t left_y = 0;
    std::int16_t right_x = 0;
    std::int16_t right_y = 0;
    std::array<wchar_t, 160> description{}; // Bounded controller mapping metadata, never device paths.
};

struct Snapshot {
    std::uint64_t sequence = 0;
    Tick sampled_at = 0;
    std::array<Controller, slot_count> slots{};
};

// The M0 host supplies SIMULATED gates, not production observers.
struct Gates {
    bool context_allowed = false;
    std::uint64_t context_generation = 0;
    bool session_available = false;
    bool physical_baseline_known = false;
    std::uint16_t physical_modifiers = 0;
    std::uint16_t physical_mouse_buttons = 0;
    std::uint64_t physical_activity_generation = 0;

    bool operator==(const Gates&) const = default;
};

enum class State { Disabled, Paused, Ready };
enum class Reason {
    Startup, UserPause, Exiting, NoSelection, InvalidSelection, AwaitingSample,
    StaleSample, InvalidSample, Disconnected, ReaderError, ConfirmSelection,
    ContextBlocked, SessionUnavailable, PhysicalUnknown, PhysicalHeld,
    AwaitingNeutral, Ready, SimulatedAHeld, SelectionChanged, Enabled
};

struct Status {
    State state = State::Disabled;
    Reason reason = Reason::Startup;
    bool operator==(const Status&) const = default;
};

enum class ActionKind { ADown, AUp };
struct Action {
    ActionKind kind;
    Tick at;
    Reason reason;
    bool cleanup;
};

struct Result {
    Status status;
    std::optional<Action> action;
};

// Single-owner, synchronous simulator: returning an action means the in-memory
// simulation and its ledger are already updated. There is no deferred output queue.
// A future live dispatcher must NOT treat these simulated results as OS acceptance.
class Core {
public:
    Result select_slot(std::optional<unsigned> slot, Tick now);
    Result enable(Tick now);
    Result pause(Tick now);
    Result shutdown(Tick now);
    Result step(const Snapshot& snapshot, const Gates& gates, Tick now);

    [[nodiscard]] Status status() const { return status_; }
    [[nodiscard]] std::optional<unsigned> selected_slot() const { return selected_; }
    [[nodiscard]] bool owns_a() const { return owns_a_; }
    [[nodiscard]] bool enabled() const { return enabled_; }

private:
    std::optional<Action> disarm(Tick now, Reason reason);
    Result finish(State state, Reason reason, std::optional<Action> action = {});
    State inactive_state() const;
    Reason inactive_reason() const;

    bool enabled_ = false;
    bool paused_ = false;
    bool exiting_ = false;
    bool armed_ = false;
    bool owns_a_ = false;
    bool confirmation_required_ = false;
    std::optional<unsigned> selected_;
    std::optional<std::uint64_t> connection_epoch_;
    std::optional<Gates> previous_gates_;
    std::uint64_t last_sequence_ = 0;
    Tick last_sample_time_ = 0;
    Tick arm_after_ = 0;
    Status status_{};
};

[[nodiscard]] bool neutral(const Controller& controller);
[[nodiscard]] std::wstring_view state_text(State state);
[[nodiscard]] std::wstring_view reason_text(Reason reason);

} // namespace gamepad
