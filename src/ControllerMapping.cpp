#include "ControllerMapping.h"

#include <algorithm>
#include <charconv>
#include <Windows.h>

namespace gamepad {
namespace {
constexpr std::array<std::string_view, 14> button_names{
    "dpup", "dpdown", "dpleft", "dpright", "start", "back", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "a", "b", "x", "y"
};
constexpr std::array<std::string_view, 6> axis_names{
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"
};
bool number(std::string_view text, unsigned& value)
{
    if (text.empty()) { return false; }
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), value);
    return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
}
bool binding(std::string_view text, ControllerBinding& out)
{
    if (text.empty()) { return true; }
    if (text.front() == '+' || text.front() == '-') {
        out.half = text.front() == '+' ? 1 : -1;
        text.remove_prefix(1);
    }
    if (text.empty()) { return false; }
    if (text.back() == '~') { out.inverted = true; text.remove_suffix(1); }
    if (text.size() < 2) { return false; }
    const char kind = text.front();
    text.remove_prefix(1);
    if (kind == 'h') {
        const auto dot = text.find('.');
        out.kind = BindingKind::Hat;
        return !out.half && !out.inverted && dot != text.npos
            && number(text.substr(0, dot), out.index) && out.index < controller_hats
            && number(text.substr(dot + 1), out.mask) && out.mask > 0 && out.mask <= 15;
    }
    if (!number(text, out.index)) { return false; }
    if (kind == 'b') {
        out.kind = BindingKind::Button;
        return !out.half && !out.inverted && out.index < controller_buttons;
    }
    out.kind = BindingKind::Axis;
    return kind == 'a' && out.index < controller_axes;
}
int axis_value(const ControllerBinding& bind, const RawController& raw)
{
    const int value = raw.axes[bind.index];
    return bind.inverted ? -value - 1 : value;
}
int strength(const ControllerBinding& bind, const RawController& raw)
{
    switch (bind.kind) {
    case BindingKind::None: return 0;
    case BindingKind::Button: return raw.buttons[bind.index] ? 65535 : 0;
    case BindingKind::Hat: return (raw.hats[bind.index] & bind.mask) != 0 ? 65535 : 0;
    case BindingKind::Axis: {
        const int value = raw.axes[bind.index];
        if ((bind.half > 0 && value < 0) || (bind.half < 0 && value > 0)) { return 0; }
        int amount = bind.half > 0 ? value * 65535 / 32767
            : bind.half < 0 ? -value * 65535 / 32768 : value + 32768;
        return bind.inverted ? 65535 - amount : amount;
    }
    }
    return 0;
}
}

bool controller_guid(std::string_view value)
{
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}
std::string directinput_mapping_guid(std::uint16_t vendor, std::uint16_t product, std::string_view name)
{
    std::array<unsigned char, 16> bytes{};
    bytes[0] = vendor && product ? 3 : 5;
    if (vendor) {
        bytes[4] = static_cast<unsigned char>(vendor);
        bytes[5] = static_cast<unsigned char>(vendor >> 8);
        bytes[8] = static_cast<unsigned char>(product);
        bytes[9] = static_cast<unsigned char>(product >> 8);
    } else {
        for (std::size_t i = 0; i < std::min<std::size_t>(name.size(), 11); ++i) { bytes[i + 4] = static_cast<unsigned char>(name[i]); }
    }
    std::string result;
    result.reserve(32);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
std::uint16_t controller_name_crc(std::string_view name)
{
    std::uint16_t crc = 0;
    for (const unsigned char byte : name) {
        crc ^= byte;
        for (unsigned i = 0; i < 8; ++i) {
            crc = static_cast<std::uint16_t>((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
        }
    }
    return crc;
}
std::wstring parse_controller_mapping(std::string_view line, ControllerMapping& result)
{
    if (line.size() > 4096 || line.find('\0') != line.npos) { return L"Controller mapping exceeds its bound."; }
    if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
    const auto first = line.find(',');
    const auto second = first == line.npos ? line.npos : line.find(',', first + 1);
    if (first == line.npos || second == line.npos || !controller_guid(line.substr(0, first)) || second == first + 1) {
        return L"Controller mapping needs a 32-digit GUID and a name.";
    }
    ControllerMapping parsed;
    unsigned embedded_crc = 0;
    const std::array<char, 4> crc_digits{line[6], line[7], line[4], line[5]};
    std::from_chars(crc_digits.data(), crc_digits.data() + crc_digits.size(), embedded_crc, 16);
    std::array<bool, 28> used{};
    bool platform_seen = false, crc_seen = false;
    line.remove_prefix(second + 1);
    while (!line.empty()) {
        const auto comma = line.find(',');
        const auto field = line.substr(0, comma);
        line = comma == line.npos ? std::string_view{} : line.substr(comma + 1);
        if (field.empty()) { return L"Controller mapping contains an empty field."; }
        const auto colon = field.find(':');
        if (colon == field.npos) { return L"Controller mapping field needs a colon."; }
        auto name = field.substr(0, colon);
        const auto value = field.substr(colon + 1);
        if (name == "platform") {
            if (platform_seen || value != "Windows") { return L"Controller mapping needs one Windows platform."; }
            platform_seen = true;
            continue;
        }
        // Extra standardized controls are outside the desktop model; never become output.
        if (name == "guide" || name == "misc1" || name == "misc2" || name == "misc3"
            || name == "misc4" || name == "misc5" || name == "touchpad" || name == "paddle1"
            || name == "paddle2" || name == "paddle3" || name == "paddle4") {
            ControllerBinding ignored;
            if (!binding(value, ignored)) { return L"Invalid extra controller binding."; }
            continue;
        }
        if (name == "type") { continue; }
        if (name == "crc") {
            if (crc_seen) { return L"Duplicate mapping name CRC."; }
            crc_seen = true;
            unsigned crc = 0;
            const auto converted = std::from_chars(value.data(), value.data() + value.size(), crc, 16);
            if (value.empty() || converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || crc > 0xffff) {
                return L"Invalid mapping name CRC.";
            }
            if (embedded_crc && embedded_crc != crc) { return L"Conflicting embedded and explicit mapping CRC."; }
            continue;
        }
        int output_half = -1;
        if (!name.empty() && (name.front() == '+' || name.front() == '-')) {
            output_half = name.front() == '+' ? 0 : 1;
            name.remove_prefix(1);
        }
        const auto button = std::find(button_names.begin(), button_names.end(), name);
        const auto axis = std::find(axis_names.begin(), axis_names.end(), name);
        if (button == button_names.end() && axis == axis_names.end()) {
            return L"Unsupported controller mapping field or conditional mapping.";
        }
        if (output_half >= 0) {
            if (axis == axis_names.end() || axis - axis_names.begin() >= 4) { return L"Only sticks accept output half bindings."; }
            const auto index = static_cast<std::size_t>(axis - axis_names.begin());
            auto& seen = used[20 + index * 2 + output_half];
            if (seen || used[14 + index]) { return L"Duplicate or conflicting stick mapping."; }
            seen = true;
            if (!binding(value, parsed.split_axes[index][output_half])) { return L"Invalid split-axis binding."; }
            continue;
        }
        const auto index = button != button_names.end()
            ? static_cast<std::size_t>(button - button_names.begin())
            : button_names.size() + static_cast<std::size_t>(axis - axis_names.begin());
        if (used[index] || (index >= 14 && index < 18 && (used[20 + (index - 14) * 2] || used[21 + (index - 14) * 2]))) {
            return L"Duplicate controller mapping action.";
        }
        used[index] = true;
        auto& target = index < button_names.size() ? parsed.buttons[index] : parsed.axes[index - button_names.size()];
        if (!binding(value, target)) { return L"Invalid controller binding."; }
    }
    const auto has_axis = [&](unsigned index) {
        return parsed.axes[index].kind != BindingKind::None || parsed.split_axes[index][0].kind != BindingKind::None
            || parsed.split_axes[index][1].kind != BindingKind::None;
    };
    if (parsed.buttons[10].kind == BindingKind::None || !has_axis(0) || !has_axis(1)) {
        return L"Controller mapping requires A and left X/Y controls.";
    }
    result = parsed;
    return {};
}
bool mapping_fits(const ControllerMapping& mapping, ControllerLayout layout)
{
    if (layout.axes > controller_axes || layout.buttons > controller_buttons || layout.hats > controller_hats) { return false; }
    const auto fits = [&](const ControllerBinding& bind) {
        switch (bind.kind) {
        case BindingKind::None: return true;
        case BindingKind::Axis: return bind.index < layout.axes;
        case BindingKind::Button: return bind.index < layout.buttons;
        case BindingKind::Hat: return bind.index < layout.hats;
        }
        return false;
    };
    return std::all_of(mapping.buttons.begin(), mapping.buttons.end(), fits)
        && std::all_of(mapping.axes.begin(), mapping.axes.end(), fits)
        && std::all_of(mapping.split_axes.begin(), mapping.split_axes.end(), [&](const auto& halves) { return fits(halves[0]) && fits(halves[1]); });
}
Controller mapped_controller(const ControllerMapping& mapping, const RawController& raw)
{
    Controller value;
    for (const auto& halves : mapping.split_axes) {
        if (strength(halves[0], raw) && strength(halves[1], raw)) {
            value.connection = Connection::Error;
            value.error = ERROR_INVALID_DATA;
            return value;
        }
    }
    value.connection = Connection::Connected;
    constexpr std::array<std::uint16_t, 14> bits{1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000};
    for (unsigned i = 0; i < bits.size(); ++i) {
        if (strength(mapping.buttons[i], raw) > 32767) { value.buttons |= bits[i]; }
    }
    const auto stick = [&](unsigned index, bool invert) -> std::int16_t {
        const auto& bind = mapping.axes[index];
        int axis = 0;
        if (bind.kind == BindingKind::Axis) {
            if (bind.half) {
                const int raw_axis = raw.axes[bind.index];
                if ((bind.half > 0 && raw_axis >= 0) || (bind.half < 0 && raw_axis <= 0)) {
                    axis = strength(bind, raw) - 32768;
                }
            } else { axis = axis_value(bind, raw); }
        } else if (bind.kind != BindingKind::None) {
            axis = strength(bind, raw) ? 32767 : 0;
        } else {
            axis = strength(mapping.split_axes[index][0], raw) * 32767 / 65535
                - strength(mapping.split_axes[index][1], raw) * 32768 / 65535;
        }
        return static_cast<std::int16_t>(invert ? std::min(-axis, 32767) : axis);
    };
    value.left_x = stick(0, false); value.left_y = stick(1, true);
    value.right_x = stick(2, false); value.right_y = stick(3, true);
    value.left_trigger = static_cast<std::uint8_t>(strength(mapping.axes[4], raw) * 255 / 65535);
    value.right_trigger = static_cast<std::uint8_t>(strength(mapping.axes[5], raw) * 255 / 65535);
    return value;
}
std::string_view find_controller_mapping(std::string_view database, std::string_view guid, std::uint16_t crc)
{
    std::string_view result;
    bool specific = false;
    while (!database.empty()) {
        const auto newline = database.find('\n');
        auto line = database.substr(0, newline);
        database = newline == database.npos ? std::string_view{} : database.substr(newline + 1);
        if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
        if (line.size() > 32 && line[32] == ',' && guid.size() == 32
            && line.substr(0, 4) == guid.substr(0, 4) && line.substr(8, 24) == guid.substr(8)
            && (line.find("platform:") == line.npos || line.find("platform:Windows") != line.npos)) {
            unsigned embedded = 0;
            const std::array<char, 4> digits{line[6], line[7], line[4], line[5]};
            const auto converted_guid = std::from_chars(digits.data(), digits.data() + digits.size(), embedded, 16);
            if (converted_guid.ec != std::errc{} || converted_guid.ptr != digits.data() + digits.size()
                || (embedded && embedded != crc)) { continue; }
            const auto position = line.find(",crc:");
            if (position != line.npos) {
                const auto text = line.substr(position + 5, line.find(',', position + 5) - position - 5);
                unsigned number_value = 0;
                const auto converted = std::from_chars(text.data(), text.data() + text.size(), number_value, 16);
                if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() || number_value != crc) { continue; }
                specific = true;
                result = line;
            } else if (embedded) { specific = true; result = line; }
            else if (!specific) { result = line; }
        }
    }
    return result;
}
} // namespace gamepad
