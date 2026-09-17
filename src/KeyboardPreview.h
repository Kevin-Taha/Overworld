#pragma once

#include <Windows.h>
#include "Keyboard.h"

namespace gamepad {
int run_keyboard_preview(HINSTANCE instance, int show, KeyboardInputMode mode = KeyboardInputMode::FaceButtons);
} // namespace gamepad
