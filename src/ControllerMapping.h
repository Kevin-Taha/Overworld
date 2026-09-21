#pragma once

#include "Core.h"

#include <string>
#include <string_view>

namespace overworld {
constexpr unsigned controller_axes = 8, controller_buttons = 128, controller_hats = 4;
struct ControllerLayout {
    unsigned axes = 0, buttons = 0, hats = 0;
};
struct RawController {
    std::array<std::int16_t, controller_axes> axes{};
    std::array<bool, controller_buttons> buttons{};
    std::array<std::uint8_t, controller_hats> hats{};
};
enum class BindingKind { None, Button, Axis, Hat };
struct ControllerBinding {
    BindingKind kind = BindingKind::None;
    unsigned index = 0, mask = 0;
    int half = 0;
    bool inverted = false;
};
struct ControllerMapping {
    // D-pad U/D/L/R, Start/Back, L3/R3, LB/RB, A/B/X/Y; Guide is never mapped.
    std::array<ControllerBinding, 14> buttons{};
    // Left X/Y, right X/Y, LT/RT.
    std::array<ControllerBinding, 6> axes{};
    std::array<std::array<ControllerBinding, 2>, 4> split_axes{}; // Positive/negative stick output halves.
};
[[nodiscard]] bool controller_guid(std::string_view value);
[[nodiscard]] std::string directinput_mapping_guid(std::uint16_t vendor, std::uint16_t product, std::string_view name = {});
[[nodiscard]] std::uint16_t controller_name_crc(std::string_view name);
[[nodiscard]] std::wstring parse_controller_mapping(std::string_view line, ControllerMapping& result);
[[nodiscard]] bool mapping_fits(const ControllerMapping& mapping, ControllerLayout layout);
[[nodiscard]] Controller mapped_controller(const ControllerMapping& mapping, const RawController& raw);
// Empty means no mapping. A malformed selected line is returned for explicit rejection.
[[nodiscard]] std::string_view find_controller_mapping(std::string_view database, std::string_view guid, std::uint16_t crc = 0);
} // namespace overworld
