#pragma once

#include <span>
#include <string_view>

namespace gamepad::practice {
enum class Launch { Run, Help, Invalid };
constexpr Launch launch(std::span<const std::wstring_view> arguments)
{
    if (arguments.empty()) { return Launch::Run; }
    if (arguments.size() == 1 && arguments.front() == L"--help") { return Launch::Help; }
    return Launch::Invalid;
}
inline constexpr wchar_t window_title[] = L"Practice (development only) - no desktop input";
} // namespace gamepad::practice
