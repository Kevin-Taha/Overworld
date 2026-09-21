#include "PredictionTarget.h"

#include <Windows.h>
#include <cwchar>

namespace overworld {
namespace {
bool same_window(TextWindow target)
{
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(reinterpret_cast<HWND>(target.window), &process);
    return target.window && process == target.process && thread && thread == target.thread;
}
bool native_edit(const wchar_t* name)
{
    return _wcsicmp(name, L"Edit") == 0 || _wcsnicmp(name, L"RichEdit", 8) == 0;
}
}

PredictionPrivacy prediction_target_privacy(TextWindow target)
{
    if (!same_window(target)) { return PredictionPrivacy::Unavailable; }
    const auto window = reinterpret_cast<HWND>(target.window);
    wchar_t name[64]{}, real_name[64]{};
    if (!GetClassNameW(window, name, 64) || !RealGetWindowClassW(window, real_name, 64)) {
        return PredictionPrivacy::Unavailable;
    }
    if (native_edit(name) || native_edit(real_name)) {
        SetLastError(ERROR_SUCCESS);
        const auto style = GetWindowLongPtrW(window, GWL_STYLE);
        if (!style && GetLastError() != ERROR_SUCCESS) { return PredictionPrivacy::Unavailable; }
        if (style & ES_PASSWORD) { return PredictionPrivacy::Protected; }
    }
    // Browsers/custom controls can hide password state or change semantic fields
    // inside one HWND. No class is a positive privacy or field-eligibility proof.
    return same_window(target) ? PredictionPrivacy::Unknown : PredictionPrivacy::Unavailable;
}
} // namespace overworld
