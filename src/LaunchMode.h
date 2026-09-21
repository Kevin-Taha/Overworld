#pragma once

#include <span>
#include <string_view>

namespace overworld {
enum class LaunchMode {
    Desktop, Inspect, Keyboard, OneHandedKeyboard, RestoreCursor, ReleaseTap, Invalid
};

// Arguments exclude the executable name. Never fall back to live mode on an unrecognized flag.
constexpr LaunchMode launch_mode(std::span<const std::wstring_view> arguments)
{
    if (arguments.empty()) { return LaunchMode::Desktop; }
    if (arguments.size() != 1) { return LaunchMode::Invalid; }
    const auto value = arguments.front();
    if (value == L"--inspect") { return LaunchMode::Inspect; }
    if (value == L"--keyboard-preview") { return LaunchMode::Keyboard; }
    if (value == L"--keyboard-preview-one-handed") { return LaunchMode::OneHandedKeyboard; }
    if (value == L"--restore-cursor") { return LaunchMode::RestoreCursor; }
    if (value == L"--release-tap") { return LaunchMode::ReleaseTap; }
    return LaunchMode::Invalid;
}
constexpr bool launch_uses_release_tap(LaunchMode mode) { return mode == LaunchMode::ReleaseTap; }
} // namespace overworld
