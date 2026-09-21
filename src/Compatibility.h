#pragma once

namespace overworld {
// Persisted identities shared with pre-Overworld releases: changing them loses
// preferences or permits two live mappers to inject input at once.
inline constexpr wchar_t settings_directory[] = L"GamepadDesktop";
inline constexpr wchar_t mapper_mutex_name[] = L"Local\\GamepadDesktop.M1.SingleMapper";
inline constexpr wchar_t host_window_class[] = L"OverworldM0";
inline constexpr wchar_t legacy_host_window_class[] = L"GamepadDesktopM0";

template<class Lookup>
auto existing_mapper_window(Lookup lookup)
{
    if (auto window = lookup(host_window_class)) { return window; }
    return lookup(legacy_host_window_class);
}
} // namespace overworld
