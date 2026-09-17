#pragma once

#include "Core.h"

namespace gamepad::live {

struct Point {
    int x = 0;
    int y = 0;
    bool operator==(const Point&) const = default;
};

// Inclusive physical-pixel endpoints, unlike a Win32 RECT.
struct Rect {
    int left = 0;
    int top = 0;
    int right = -1;
    int bottom = -1;
    bool operator==(const Rect&) const = default;
};

struct Vector { double x; double y; };
struct Motion { Point point; int wheel = 0; };

constexpr double pointer_speed = 1100.0; // Physical pixels per second.
constexpr double precision_scale = 0.25;
constexpr double scroll_speed = 960.0; // Wheel units per second; 120 = one detent.
constexpr Tick live_sample_age_ms = 100;
constexpr Tick integration_cap_ms = 32;

enum class MotionProfile { Smooth, SpeedBands };

struct PointerSettings {
    double speed = pointer_speed;
    double precision = precision_scale;
    double scroll = scroll_speed;
    double left_exit = 0.20;
    double left_enter = 0.24;
    double right_exit = 0.23;
    double right_enter = 0.27;
    MotionProfile profile = MotionProfile::Smooth;
    std::array<double, 3> band_rates{0.10, 0.35, 1.0}; // Fractions of maximum speed; equal neighbors merge.
    bool operator==(const PointerSettings&) const = default;
};

// Empty means valid. A nonempty result is a displayable validation error.
[[nodiscard]] std::wstring_view validate_pointer_settings(const PointerSettings& settings);
[[nodiscard]] bool valid(Rect rect);
[[nodiscard]] bool contains(Rect rect, Point point);
[[nodiscard]] Point clamp(Point point, Rect rect);
[[nodiscard]] Point absolute(Point point, Rect desktop);
[[nodiscard]] Vector response(std::int16_t x, std::int16_t y, double deadzone);
[[nodiscard]] bool pointer_neutral(const Controller& controller, const PointerSettings& settings = {});

class Pointer {
public:
    void reset(Tick now);
    void skip_interval(const Controller& controller, Tick now, const PointerSettings& settings);
    // Nondefault settings must pass validate_pointer_settings; Live enforces this.
    Motion step(const Controller& controller, Point actual, Rect bounds, Tick now, bool scroll,
                const PointerSettings& settings = {});

private:
    Vector left_response(const Controller& controller, const PointerSettings& settings);
    Tick last_ = 0;
    double x_fraction_ = 0;
    double y_fraction_ = 0;
    double scroll_fraction_ = 0;
    bool left_active_ = false;
    bool right_active_ = false;
    unsigned band_ = 0;
};

} // namespace gamepad::live
