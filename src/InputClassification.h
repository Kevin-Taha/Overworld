#pragma once

#include <string>

namespace overworld::live {
enum class InputCategory { None, Keyboard, Modifier, Mouse, ReservedGamepad, ReservedNavigation, Owned };
[[nodiscard]] InputCategory input_category(unsigned key, bool owned = false);
[[nodiscard]] std::wstring input_diagnostic_text(InputCategory category, bool raw, bool asynchronous, bool settling);
[[nodiscard]] bool physical_async_key(unsigned key);
} // namespace overworld::live
