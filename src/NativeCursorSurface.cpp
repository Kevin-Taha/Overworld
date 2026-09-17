#include "NativeCursorSurface.h"

#include <dwmapi.h>

namespace gamepad {
NativeCursorSurface::RootCache::~RootCache() { clear(); }

void NativeCursorSurface::RootCache::clear()
{
    if (process) { CloseHandle(process); }
    process = nullptr;
    window = nullptr;
    process_id = thread_id = 0;
    attempted = false;
    executable.clear();
}

CursorSurfaceObservation NativeCursorSurface::observe_root(HWND window, RootCache& cache)
{
    const auto root = window ? GetAncestor(window, GA_ROOT) : nullptr;
    DWORD process_id = 0;
    const auto thread_id = root ? GetWindowThreadProcessId(root, &process_id) : 0;
    if (!root || !thread_id || !process_id) {
        cache.clear();
        return CursorSurfaceObservation::Unknown;
    }
    if (root != cache.window || process_id != cache.process_id || thread_id != cache.thread_id
        || (cache.process && WaitForSingleObject(cache.process, 0) != WAIT_TIMEOUT)) {
        cache.clear();
        cache.window = root;
        cache.process_id = process_id;
        cache.thread_id = thread_id;
    }
    wchar_t window_class[256]{};
    if (!GetClassNameW(root, window_class, static_cast<int>(std::size(window_class)))) {
        return CursorSurfaceObservation::Unknown;
    }
    CursorSurfaceRoot observation{{}, window_class, true, IsWindowVisible(root) != FALSE, IsIconic(root) != FALSE, {}};
    // Only the observed class needs process-image and cloak checks. Other visible
    // shell/XAML/Explorer classes never become presentation exceptions.
    if (observation.visible && !observation.iconic && observation.window_class == L"Windows.UI.Core.CoreWindow") {
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
            observation.cloaked = cloaked != 0;
        }
        if (observation.cloaked && !*observation.cloaked && !cache.attempted) {
            cache.attempted = true;
            cache.process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process_id);
            if (cache.process) {
                std::array<wchar_t, 32768> path{};
                DWORD length = static_cast<DWORD>(path.size());
                if (QueryFullProcessImageNameW(cache.process, 0, path.data(), &length)) {
                    const std::wstring_view full(path.data(), length);
                    const auto separator = full.find_last_of(L"\\/");
                    cache.executable.assign(full.substr(separator == std::wstring_view::npos ? 0 : separator + 1));
                }
            }
        }
        observation.executable = cache.executable;
    }
    DWORD current_process = 0;
    const auto current_thread = GetWindowThreadProcessId(root, &current_process);
    if (current_process != process_id || current_thread != thread_id
        || GetAncestor(root, GA_ROOT) != root
        || (cache.process && WaitForSingleObject(cache.process, 0) != WAIT_TIMEOUT)) {
        cache.clear();
        return CursorSurfaceObservation::Unknown;
    }
    return classify_cursor_surface(observation);
}

CursorSurfaceObservation NativeCursorSurface::observe()
{
    const auto foreground = GetForegroundWindow();
    const auto foreground_state = observe_root(foreground, roots_[0]);
    POINT point{};
    const auto hit = GetCursorPos(&point) ? WindowFromPoint(point) : nullptr;
    const auto hit_state = observe_root(hit, roots_[1]);
    if (GetForegroundWindow() != foreground) { return CursorSurfaceObservation::Unknown; }
    return combine_cursor_surfaces(foreground_state, hit_state);
}
} // namespace gamepad
