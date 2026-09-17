#include "RingPixels.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gamepad {
namespace {
struct Paint { unsigned red; unsigned green; unsigned blue; };
constexpr Paint modern_body{246, 243, 236};
constexpr Paint modern_contour{28, 30, 36};
// The dot and the brackets the user already accepted keep their original neutral palette.
constexpr Paint neutral_body{255, 255, 255};
constexpr Paint neutral_contour{0, 0, 0};

[[nodiscard]] std::uint32_t resolve(unsigned covered, unsigned body, const Paint& fill, const Paint& edge,
                                  unsigned samples = 16)
{
    const unsigned outline = covered - body;
    const auto channel = [&](unsigned lit, unsigned dark) { return (body * lit + outline * dark + samples / 2) / samples; };
    return ((covered * 255 + samples / 2) / samples << 24) | (channel(fill.red, edge.red) << 16)
        | (channel(fill.green, edge.green) << 8) | channel(fill.blue, edge.blue);
}

constexpr double ring_inset = .76;
[[nodiscard]] double ring_stroke(int diameter, double scale) { return std::clamp(diameter * .155, 4.2, 14.0) * scale; }
[[nodiscard]] double ring_contour(int diameter, double scale) { return std::clamp(diameter * .035, 1.4, 2.0) * scale; }

struct Vertex { double x; double y; };
// Compact triangular head with a shallow rear notch; no stem or variable-radius bulge.
constexpr std::array pointer_outline{
    Vertex{0, 0}, Vertex{.56, .18}, Vertex{.33, .33}, Vertex{.18, .56}};
constexpr double tip_axis_x = .7071067811865475, tip_axis_y = .7071067811865475;
[[nodiscard]] double pointer_rounding(int size, double scale) { return std::clamp(size * .035, .7, 1.75) * scale; }
[[nodiscard]] double pointer_contour(int size, double scale) { return std::clamp(size * .042, 1.15, 2.5) * scale; }

[[nodiscard]] double pointer_distance(double dx, double dy, double extent)
{
    double distance = std::numeric_limits<double>::max();
    bool inside = false;
    for (std::size_t i = 0, j = pointer_outline.size() - 1; i < pointer_outline.size(); j = i++) {
        const Vertex a{pointer_outline[j].x * extent, pointer_outline[j].y * extent};
        const Vertex b{pointer_outline[i].x * extent, pointer_outline[i].y * extent};
        if ((a.y > dy) != (b.y > dy) && dx < (b.x - a.x) * (dy - a.y) / (b.y - a.y) + a.x) { inside = !inside; }
        const double vx = b.x - a.x, vy = b.y - a.y;
        const double t = std::clamp(((dx - a.x) * vx + (dy - a.y) * vy) / (vx * vx + vy * vy), 0.0, 1.0);
        distance = (std::min)(distance, std::hypot(dx - a.x - t * vx, dy - a.y - t * vy));
    }
    return inside ? -distance : distance;
}

template<typename Distance>
[[nodiscard]] std::uint32_t modern_pixel(double x, double y, double contour, Distance distance)
{
    const double center = distance(x, y);
    // Signed distances are 1-Lipschitz. Only pixels touching an ink/alpha edge need 8x8 coverage.
    constexpr double half_diagonal = .707107;
    if (center > half_diagonal) { return 0; }
    if (center < -contour - half_diagonal) { return resolve(1, 1, modern_body, modern_contour, 1); }
    if (center < -half_diagonal && center > -contour + half_diagonal) {
        return resolve(1, 0, modern_body, modern_contour, 1);
    }
    unsigned covered = 0, lit = 0;
    for (int sy = 0; sy < 8; ++sy) {
        for (int sx = 0; sx < 8; ++sx) {
            const double d = distance(x + (sx + .5) / 8 - .5, y + (sy + .5) / 8 - .5);
            if (d <= 0) { ++covered; if (d <= -contour) { ++lit; } }
        }
    }
    return resolve(covered, lit, modern_body, modern_contour, 64);
}
} // namespace

bool valid_cursor_style(CursorStyle style)
{
    return std::any_of(cursor_styles.begin(), cursor_styles.end(), [style](const auto& choice) { return choice.style == style; });
}

std::optional<RingGeometry> ring_geometry(live::Point hotspot, unsigned dpi, int diameter)
{
    if (dpi < 96 || dpi > 768 || diameter < 20 || diameter > 96) { return {}; }
    const int half = static_cast<int>(std::ceil(diameter * (dpi / 96.0) / 2)) + 1;
    const auto left = std::int64_t{hotspot.x} - half;
    const auto top = std::int64_t{hotspot.y} - half;
    if (left < INT_MIN || top < INT_MIN || std::int64_t{hotspot.x} + half > INT_MAX
        || std::int64_t{hotspot.y} + half > INT_MAX) { return {}; }
    return RingGeometry{static_cast<int>(left), static_cast<int>(top), half * 2 + 1, half, half};
}

std::vector<std::uint32_t> ring_pixels(unsigned dpi, int diameter)
{
    const auto geometry = ring_geometry({}, dpi, diameter);
    if (!geometry) { return {}; }
    const int side = geometry->side;
    const double scale = dpi / 96.0;
    const double outer = diameter * ring_inset * scale / 2;
    const double contour = ring_contour(diameter, scale);
    const double inner = outer - ring_stroke(diameter, scale);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(side) * side);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            pixels[static_cast<std::size_t>(y) * side + x] = modern_pixel(x - side / 2, y - side / 2,
                contour, [=](double dx, double dy) {
                    const double radius = std::hypot(dx, dy);
                    return (std::max)(inner - radius, radius - outer);
                });
        }
    }
    return pixels;
}

std::optional<RingGeometry> cursor_geometry(live::Point hotspot, unsigned dpi, int size, CursorStyle style)
{
    if (!valid_cursor_style(style) || dpi < 96 || dpi > 768 || size < 20 || size > 96) { return {}; }
    if (style == CursorStyle::Ring) { return ring_geometry(hotspot, dpi, size); }
    const double scale = dpi / 96.0;
    int offset = 0, side = 0;
    if (style == CursorStyle::Arrow) {
        const double rounding = pointer_rounding(size, scale);
        offset = static_cast<int>(std::ceil(rounding)) + 1;
        side = offset + static_cast<int>(std::ceil(.56 * size * scale + rounding * (1 + tip_axis_y))) + 2;
        if (side % 2 == 0) { ++side; }
    } else {
        // Dot diameter is one quarter of the nominal size; do not turn 40 DIPs into an opaque disk.
        const double extent = (style == CursorStyle::Dot ? size / 4.0 : static_cast<double>(size)) * scale;
        offset = static_cast<int>(std::ceil(extent / 2)) + 1;
        side = offset * 2 + 1;
    }
    const auto left = std::int64_t{hotspot.x} - offset;
    const auto top = std::int64_t{hotspot.y} - offset;
    if (left < INT_MIN || top < INT_MIN || left + side - 1 > INT_MAX || top + side - 1 > INT_MAX) { return {}; }
    return RingGeometry{static_cast<int>(left), static_cast<int>(top), side, offset, offset};
}

std::vector<std::uint32_t> cursor_pixels(unsigned dpi, int size, CursorStyle style)
{
    if (style == CursorStyle::Ring) { return ring_pixels(dpi, size); }
    const auto geometry = cursor_geometry({}, dpi, size, style);
    if (!geometry) { return {}; }
    const auto& g = *geometry;
    const double scale = dpi / 96.0;
    const double extent = size * scale;
    const double tip_contour = pointer_contour(size, scale);
    const double rounding = pointer_rounding(size, scale);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(g.side) * g.side);
    for (int y = 0; y < g.side; ++y) {
        for (int x = 0; x < g.side; ++x) {
            if (style == CursorStyle::Arrow) {
                pixels[static_cast<std::size_t>(y) * g.side + x] = modern_pixel(x - g.hotspot_x, y - g.hotspot_y,
                    tip_contour, [=](double dx, double dy) {
                        // Translate the rounded core so its forward apex, not the circle's centre,
                        // lands on the input hotspot at every DPI.
                        return pointer_distance(dx - tip_axis_x * rounding, dy - tip_axis_y * rounding, extent) - rounding;
                    });
                continue;
            }
            unsigned covered = 0, body = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const double dx = x - g.hotspot_x + (sx + .5) / 4 - .5;
                    const double dy = y - g.hotspot_y + (sy + .5) / 4 - .5;
                    bool inside = false, light = false;
                    if (style == CursorStyle::Dot) {
                        const double radius = std::hypot(dx, dy);
                        inside = radius <= extent / 8;
                        light = radius <= extent / 8 - 1.25 * scale;
                    } else if (style == CursorStyle::Brackets) {
                        const double ax = std::abs(dx), ay = std::abs(dy);
                        const double edge = extent / 2 - 1.75 * scale;
                        const double end = extent * .22;
                        // Distance to two perpendicular corner segments, reflected into four quadrants.
                        const double distance = (std::min)(
                            std::hypot(ax - std::clamp(ax, end, edge), ay - edge),
                            std::hypot(ax - edge, ay - std::clamp(ay, end, edge)));
                        inside = distance <= 1.75 * scale;
                        light = distance <= .75 * scale;
                    }
                    if (inside) { ++covered; if (light) { ++body; } }
                }
            }
            pixels[static_cast<std::size_t>(y) * g.side + x] = resolve(covered, body, neutral_body, neutral_contour);
        }
    }
    return pixels;
}
} // namespace gamepad
