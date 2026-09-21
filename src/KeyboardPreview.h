#pragma once

#include <Windows.h>
#include "Keyboard.h"

namespace overworld {
int run_keyboard_preview(HINSTANCE instance, int show, KeyboardInputMode mode = KeyboardInputMode::FaceButtons);
} // namespace overworld
