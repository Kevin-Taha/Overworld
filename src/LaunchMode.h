#pragma once

#include <span>
#include <string_view>

namespace gamepad {
enum class LaunchMode {
    Desktop, Inspect, Keyboard, OneHandedKeyboard, RestoreCursor,
    WordPrediction, Prediction, OneHandedPrediction, PredictionDemo, Invalid
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
    if (value == L"--word-prediction") { return LaunchMode::WordPrediction; }
    if (value == L"--prediction-preview") { return LaunchMode::Prediction; }
    if (value == L"--prediction-preview-one-handed") { return LaunchMode::OneHandedPrediction; }
    if (value == L"--prediction-demo") { return LaunchMode::PredictionDemo; }
    return LaunchMode::Invalid;
}
} // namespace gamepad
