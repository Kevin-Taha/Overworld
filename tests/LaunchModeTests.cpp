#include "LaunchMode.h"
#include "PracticeLaunch.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace overworld;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
}
int main()
{
    try {
        check(launch_mode({}) == LaunchMode::Desktop, "shipping default remains desktop");
        check(!launch_uses_release_tap(launch_mode({})), "default click timing unchanged");
        check(!launch_uses_controller_view(launch_mode({})), "controller view is off for ordinary launches");
        check(practice::launch({}) == practice::Launch::Run, "separate development exe opens only its harness");
        for (const auto [flag, expected] : std::array{
            std::pair{L"--inspect", LaunchMode::Inspect},
            std::pair{L"--keyboard-preview", LaunchMode::Keyboard},
            std::pair{L"--keyboard-preview-one-handed", LaunchMode::OneHandedKeyboard},
            std::pair{L"--restore-cursor", LaunchMode::RestoreCursor},
            std::pair{L"--release-tap", LaunchMode::ReleaseTap}}) {
            const std::array<std::wstring_view, 1> argument{flag};
            check(launch_mode(argument) == expected, "supported production mode unchanged");
            check(launch_uses_release_tap(expected) == (expected == LaunchMode::ReleaseTap),
                "only explicit release-tap changes click timing");
            check(!launch_uses_controller_view(expected), "existing modes never opt in to a controller view");
            const std::array<std::wstring_view, 2> duplicate{flag, flag}, mixed{flag, L"--release-tap"};
            check(launch_mode(duplicate) == LaunchMode::Invalid && launch_mode(mixed) == LaunchMode::Invalid,
                "duplicates and mode combinations fail closed");
            check(practice::launch(argument) == practice::Launch::Invalid, "development exe rejects production flags");
        }
        for (const auto flag : {L"--practice", L"--Practice", L"--dev", L"--render-only", L"--help", L"--inspect=1", L"",
            L"--word-prediction", L"--prediction-demo", L"--prediction-preview", L"--prediction-preview-one-handed",
            L"--system-cursor", L"--system-cursor-release-tap", L"--release-tap=1", L"--controller-view=1"}) {
            const std::array<std::wstring_view, 1> argument{flag};
            check(launch_mode(argument) == LaunchMode::Invalid, "unknown/development flags never activate shipping mapper");
            check(practice::launch(argument) == (argument[0] == L"--help" ? practice::Launch::Help : practice::Launch::Invalid),
                "development arguments fail closed except explicit help");
            const std::array<std::wstring_view, 2> mixed{flag, L"--inspect"};
            check(launch_mode(mixed) == LaunchMode::Invalid && practice::launch(mixed) == practice::Launch::Invalid,
                "mixed arguments cannot start either host");
        }
        const std::array<std::wstring_view, 1> view{L"--controller-view"};
        check(launch_mode(view) == LaunchMode::ControllerView
            && launch_uses_controller_view(launch_mode(view)) && !launch_uses_release_tap(launch_mode(view)),
            "explicit view opts in without changing ordinary click timing");
        check(practice::launch(view) == practice::Launch::Invalid, "development harness never starts the product view");
        for (const auto arguments : {std::array<std::wstring_view, 2>{L"--controller-view", L"--release-tap"},
            std::array<std::wstring_view, 2>{L"--release-tap", L"--controller-view"}}) {
            const auto mode = launch_mode(arguments);
            check(mode == LaunchMode::ControllerViewReleaseTap && launch_uses_controller_view(mode)
                && launch_uses_release_tap(mode), "view and release-tap combine explicitly in either order");
            check(practice::launch(arguments) == practice::Launch::Invalid, "combined product options remain invalid in harness");
        }
        for (const auto flag : {L"--controller-view", L"--inspect", L"--keyboard-preview",
            L"--keyboard-preview-one-handed", L"--restore-cursor", L"--practice", L"", L"--controller-view=1"}) {
            for (const auto arguments : {std::array<std::wstring_view, 2>{L"--controller-view", flag},
                std::array<std::wstring_view, 2>{flag, L"--controller-view"}}) {
                const auto mode = launch_mode(arguments);
                check(mode == LaunchMode::Invalid && !launch_uses_controller_view(mode) && !launch_uses_release_tap(mode),
                    "unsupported or duplicate flags cannot start mapper or view");
            }
        }
        const std::array<std::wstring_view, 3> extra{L"--controller-view", L"--release-tap", L"--inspect"};
        check(launch_mode(extra) == LaunchMode::Invalid, "extra flags fail closed before any native construction");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS separate shipping/development parsers; obsolete practice and malformed flags fail closed.\n";
}
