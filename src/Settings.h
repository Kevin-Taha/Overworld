#pragma once

#include "Keyboard.h"
#include "Live.h"
#include "RingPixels.h"

#include <array>
#include <filesystem>
#include <string>

namespace gamepad {

struct Settings {
    live::PointerSettings pointer;
    int ring_diameter = 28; // DIPs, nominal cursor size; existing saved sizes are preserved.
    CursorStyle cursor_style = CursorStyle::Arrow;
    live::ExecutableExclusions exclusions;
    bool automatic_game_switching = true;
    // Opt-in stick-only two-flick keyboard selection; existing files default to FaceButtons.
    KeyboardInputMode keyboard_input_mode = KeyboardInputMode::FaceButtons;
    bool keyboard_in_native_mode = false;
    bool experimental_navigation_filter = false;
    bool voice_enabled = false;
    std::wstring voice_model_path; // Empty uses models\ggml-tiny.en.bin beside the executable.
    bool operator==(const Settings&) const = default;
};

constexpr std::size_t settings_byte_limit = 128 * 1024;
constexpr std::size_t exclusion_text_limit = 128 * 261;
constexpr std::size_t voice_model_path_limit = 1024;
constexpr int settings_field_count = 11;
using SettingsFields = std::array<std::wstring, settings_field_count>;

[[nodiscard]] bool settings_number(std::wstring_view text, double& result);
[[nodiscard]] std::wstring validate_settings(const Settings& settings);
[[nodiscard]] std::wstring parse_settings(std::wstring_view text, Settings& result);
[[nodiscard]] std::wstring serialize_settings(const Settings& settings);
// UI fields: speed, precision %, scroll, left/right stop/start %, size, low/middle/high band %.
[[nodiscard]] SettingsFields settings_fields(const Settings& settings);
[[nodiscard]] std::wstring exclusion_text(const Settings& settings);
[[nodiscard]] std::wstring parse_settings_fields(const SettingsFields& fields,
    std::wstring_view exclusions, const Settings& original, Settings& result);

enum class LoadState { Missing, Loaded, Invalid, Unreadable };
struct SettingsFile {
    LoadState state = LoadState::Unreadable;
    Settings settings;
    std::string bytes; // Last read version; detects external edits before saving.
    std::wstring error;
};

[[nodiscard]] std::wstring settings_path(std::filesystem::path& result);
[[nodiscard]] SettingsFile load_settings(const std::filesystem::path& path);
// Refuses invalid/unreadable baselines and externally changed files. Updates baseline only on success.
[[nodiscard]] std::wstring save_settings(const std::filesystem::path& path,
    const Settings& settings, SettingsFile& baseline);

} // namespace gamepad
