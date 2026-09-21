#pragma once

#include "CursorPresentation.h"

#include <Windows.h>
#include <array>

namespace overworld {
// Read-only, UI-thread sampler. Never changes foreground, input policy or cursor visibility.
class NativeCursorSurface {
public:
    NativeCursorSurface() = default;
    NativeCursorSurface(const NativeCursorSurface&) = delete;
    NativeCursorSurface& operator=(const NativeCursorSurface&) = delete;
    [[nodiscard]] CursorSurfaceObservation observe();
private:
    struct RootCache {
        ~RootCache();
        void clear();
        HWND window = nullptr;
        DWORD process_id = 0;
        DWORD thread_id = 0;
        HANDLE process = nullptr;
        CursorImageQuerySchedule image_query;
        std::wstring executable;
    };
    CursorSurfaceObservation observe_root(HWND window, RootCache& cache);
    std::array<RootCache, 2> roots_;
};
} // namespace overworld
