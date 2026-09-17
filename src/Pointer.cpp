#include "Pointer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gamepad::live {
namespace {
double magnitude(std::int16_t x, std::int16_t y)
{
    return std::hypot(static_cast<double>(x), static_cast<double>(y)) / 32767.0;
}

Vector stick(std::int16_t x, std::int16_t y, double enter, double leave, bool& active)
{
    const double radius = magnitude(x, y);
    active = active ? radius > leave : radius > enter;
    return active ? response(x, y, leave) : Vector{};
}

int normalize(int value, int minimum, int maximum)
{
    const auto count = std::int64_t{maximum} - minimum + 1;
    const auto offset = std::int64_t{value} - minimum;
    // Windows maps normalized values to pixel buckets, not inclusive point endpoints.
    // Use the integer bucket midpoint so rounding cannot select an adjacent pixel.
    const auto first = (offset * 65536 + count - 1) / count;
    const auto last = ((offset + 1) * 65536 + count - 1) / count - 1;
    return static_cast<int>(first + (last - first) / 2);
}
} // namespace

std::wstring_view validate_pointer_settings(const PointerSettings& settings)
{
    if (settings.profile != MotionProfile::Smooth && settings.profile != MotionProfile::SpeedBands) {
        return L"Choose Smooth or Speed bands for the motion profile";
    }
    const auto between = [](double value, double low, double high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    if (!between(settings.speed, 100, 5000)) { return L"Pointer speed must be finite and between 100 and 5000 pixels/s"; }
    if (!std::all_of(settings.band_rates.begin(), settings.band_rates.end(),
                    [&](double rate) { return between(rate, .01, 1); })
        || !std::is_sorted(settings.band_rates.begin(), settings.band_rates.end())) {
        return L"Band speeds must be 1-100% in low-to-high order; equal speeds are allowed";
    }
    if (!between(settings.precision, 0.05, 1)) { return L"Precision must be finite and between 0.05 and 1"; }
    if (!between(settings.scroll, 120, 4800)) { return L"Scroll speed must be finite and between 120 and 4800 units/s"; }
    if (!between(settings.left_exit, 0, 0.90) || !between(settings.left_enter, 0, 0.95)
        || settings.left_exit >= settings.left_enter) {
        return L"Left deadzones require finite 0 <= exit <= 0.90 and exit < enter <= 0.95";
    }
    if (!between(settings.right_exit, 0, 0.90) || !between(settings.right_enter, 0, 0.95)
        || settings.right_exit >= settings.right_enter) {
        return L"Right deadzones require finite 0 <= exit <= 0.90 and exit < enter <= 0.95";
    }
    return {};
}

bool valid(Rect rect)
{
    // Also bounds arithmetic and prevents pathological desktop spans.
    return rect.right >= rect.left && rect.bottom >= rect.top
        && std::int64_t{rect.right} - rect.left < 65536
        && std::int64_t{rect.bottom} - rect.top < 65536;
}

bool contains(Rect rect, Point point)
{
    return valid(rect) && point.x >= rect.left && point.x <= rect.right
        && point.y >= rect.top && point.y <= rect.bottom;
}

Point clamp(Point point, Rect rect)
{
    if (!valid(rect)) { return {}; }
    return {std::clamp(point.x, rect.left, rect.right), std::clamp(point.y, rect.top, rect.bottom)};
}

Point absolute(Point point, Rect desktop)
{
    if (!valid(desktop)) { return {}; }
    point = clamp(point, desktop);
    return {normalize(point.x, desktop.left, desktop.right),
            normalize(point.y, desktop.top, desktop.bottom)};
}

Vector response(std::int16_t x, std::int16_t y, double deadzone)
{
    if (!std::isfinite(deadzone) || deadzone < 0 || deadzone >= 1) { return {}; }
    const double radius = magnitude(x, y);
    if (radius <= deadzone) { return {}; }
    const double strength = (std::min(radius, 1.0) - deadzone) / (1.0 - deadzone);
    const double scale = strength * strength / (radius * 32767.0);
    return {x * scale, y * scale};
}

bool pointer_neutral(const Controller& controller, const PointerSettings& settings)
{
    return (controller.buttons & standard_buttons) == 0
        && controller.left_trigger <= 30 && controller.right_trigger <= 30
        && magnitude(controller.left_x, controller.left_y) <= settings.left_exit
        && magnitude(controller.right_x, controller.right_y) <= settings.right_exit;
}

void Pointer::reset(Tick now)
{
    last_ = now;
    x_fraction_ = y_fraction_ = scroll_fraction_ = 0;
    left_active_ = right_active_ = false;
    band_ = 0;
}

Vector Pointer::left_response(const Controller& controller, const PointerSettings& settings)
{
    const auto smooth = stick(controller.left_x, controller.left_y,
        settings.left_enter, settings.left_exit, left_active_);
    if (!left_active_ || settings.profile == MotionProfile::Smooth) {
        band_ = 0;
        return smooth;
    }
    const double radius = magnitude(controller.left_x, controller.left_y);
    // Bands use the range ABOVE entry, not raw magnitude or the Smooth exit-rescaled curve.
    // Even a legal 95% entry deadzone leaves all three bands reachable.
    const double active = std::clamp((std::min(radius, 1.0) - settings.left_enter)
        / (1.0 - settings.left_enter), 0.0, 1.0);
    if (active >= .90) { band_ = 2; }
    else if (band_ == 2 && active < .85) { band_ = active < .30 ? 0 : 1; }
    else if (band_ == 0 && active >= .35) { band_ = 1; }
    else if (band_ == 1 && active < .30) { band_ = 0; }
    const double rate = settings.band_rates[band_];
    const double scale = rate / (radius * 32767.0);
    return {controller.left_x * scale, controller.left_y * scale};
}

void Pointer::skip_interval(const Controller& controller, Tick now, const PointerSettings& settings)
{
    last_ = now;
    x_fraction_ = y_fraction_ = scroll_fraction_ = 0;
    // Discard deferred motion, but preserve hysteresis and observe centering.
    left_response(controller, settings);
    stick(controller.right_x, controller.right_y, settings.right_enter, settings.right_exit, right_active_);
}

Motion Pointer::step(const Controller& controller, Point actual, Rect bounds, Tick now, bool scroll,
                     const PointerSettings& settings)
{
    if (!valid(bounds) || now < last_ || now - last_ > live_sample_age_ms) {
        reset(now);
        return {actual, 0};
    }
    const double seconds = static_cast<double>(std::min(now - last_, integration_cap_ms)) / 1000.0;
    last_ = now;
    const auto left = left_response(controller, settings);
    const auto right = stick(controller.right_x, controller.right_y, settings.right_enter, settings.right_exit, right_active_);
    const double speed = settings.speed * ((controller.buttons & 0x0100) ? settings.precision : 1.0);
    if (!left_active_) { x_fraction_ = y_fraction_ = 0; }
    x_fraction_ += left.x * speed * seconds;
    y_fraction_ -= left.y * speed * seconds;
    const int dx = static_cast<int>(x_fraction_);
    const int dy = static_cast<int>(y_fraction_);
    x_fraction_ -= dx;
    y_fraction_ -= dy;
    actual = clamp(actual, bounds);
    // Compute in double before clamping, even when bounds border INT_MIN/MAX.
    Point next{
        static_cast<int>(std::clamp(static_cast<double>(actual.x) + dx,
                                   static_cast<double>(bounds.left), static_cast<double>(bounds.right))),
        static_cast<int>(std::clamp(static_cast<double>(actual.y) + dy,
                                   static_cast<double>(bounds.top), static_cast<double>(bounds.bottom)))
    };
    if (next.x == bounds.left || next.x == bounds.right) { x_fraction_ = 0; }
    if (next.y == bounds.top || next.y == bounds.bottom) { y_fraction_ = 0; }
    if (!scroll || !right_active_) { scroll_fraction_ = 0; }
    if (scroll) { scroll_fraction_ += right.y * settings.scroll * seconds; }
    const int wheel = static_cast<int>(scroll_fraction_);
    scroll_fraction_ -= wheel;
    return {next, wheel};
}

} // namespace gamepad::live
