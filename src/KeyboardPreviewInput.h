#pragma once

#include "Live.h"

#include <Windows.h>

namespace overworld {
// Shared by the window procedure and pure message-dispatch tests. Reserved
// controller/navigation messages are not ordinary keyboard takeover.
template<class Focused, class Close, class Suspend>
bool dispatch_keyboard_preview_input(UINT message, WPARAM key, bool practice,
    Focused focused, Close close, Suspend suspend)
{
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (message == WM_KEYDOWN && key == VK_ESCAPE && focused()) {
            close();
            return true;
        }
        if (practice && key < 256 && live::physical_async_key(static_cast<unsigned>(key))) { suspend(); }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_MOUSEWHEEL:
        if (practice) { suspend(); }
        break;
    default:
        break;
    }
    return false;
}
} // namespace overworld
