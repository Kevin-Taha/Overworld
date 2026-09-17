#include "NativeCursorVisibility.h"

#include <Windows.h>
#include <Magnification.h>

namespace gamepad {
NativeCursorVisibility::~NativeCursorVisibility()
{
    // No implicit TRUE here: only the coordinator knows whether hiding belongs to us.
    if (initialized_ && !shutdown()) { OutputDebugStringW(error_.c_str()); }
}

bool NativeCursorVisibility::fail(std::wstring_view operation)
{
    error_ = std::wstring{operation} + L" failed (Windows error " + std::to_wstring(GetLastError())
        + L"; 0 means no extended error supplied). No elevation/UIAccess workaround is used.";
    return false;
}

bool NativeCursorVisibility::initialize()
{
    if (initialized_) { return true; }
    SetLastError(ERROR_SUCCESS);
    if (!MagInitialize()) { return fail(L"MagInitialize"); }
    initialized_ = true;
    error_.clear();
    return true;
}

NativeCursorState NativeCursorVisibility::observe()
{
    CURSORINFO info{sizeof(info)};
    SetLastError(ERROR_SUCCESS);
    if (!GetCursorInfo(&info)) { fail(L"GetCursorInfo"); return NativeCursorState::Unknown; }
    error_.clear();
    // This is an observation, not a query of another magnifier's ownership (no such lease exists).
    return (info.flags & CURSOR_SHOWING) && !(info.flags & CURSOR_SUPPRESSED)
        ? NativeCursorState::Visible : NativeCursorState::Hidden;
}

bool NativeCursorVisibility::show(bool visible)
{
    if (!initialized_) { SetLastError(ERROR_INVALID_STATE); return fail(L"Uninitialized cursor runtime"); }
    SetLastError(ERROR_SUCCESS);
    if (!MagShowSystemCursor(visible ? TRUE : FALSE)) { return fail(L"MagShowSystemCursor"); }
    error_.clear();
    return true;
}

bool NativeCursorVisibility::shutdown()
{
    if (!initialized_) { return true; }
    SetLastError(ERROR_SUCCESS);
    if (!MagUninitialize()) { return fail(L"MagUninitialize"); }
    initialized_ = false;
    error_.clear();
    return true;
}
} // namespace gamepad
