#include "LaunchMode.h"
#include "PracticeLaunch.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace gamepad;
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
            const std::array<std::wstring_view, 2> duplicate{flag, flag}, mixed{flag, L"--release-tap"};
            check(launch_mode(duplicate) == LaunchMode::Invalid && launch_mode(mixed) == LaunchMode::Invalid,
                "duplicates and mode combinations fail closed");
            check(practice::launch(argument) == practice::Launch::Invalid, "development exe rejects production flags");
        }
        for (const auto flag : {L"--practice", L"--Practice", L"--dev", L"--render-only", L"--help", L"--inspect=1", L"",
            L"--word-prediction", L"--prediction-demo", L"--prediction-preview", L"--prediction-preview-one-handed",
            L"--system-cursor", L"--system-cursor-release-tap", L"--release-tap=1"}) {
            const std::array<std::wstring_view, 1> argument{flag};
            check(launch_mode(argument) == LaunchMode::Invalid, "unknown/development flags never activate shipping mapper");
            check(practice::launch(argument) == (argument[0] == L"--help" ? practice::Launch::Help : practice::Launch::Invalid),
                "development arguments fail closed except explicit help");
            const std::array<std::wstring_view, 2> mixed{flag, L"--inspect"};
            check(launch_mode(mixed) == LaunchMode::Invalid && practice::launch(mixed) == practice::Launch::Invalid,
                "mixed arguments cannot start either host");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS separate shipping/development parsers; obsolete practice and malformed flags fail closed.\n";
}
