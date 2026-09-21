#pragma once

#include "ControllerView.h"
#include <Windows.h>

#include <memory>
#include <string>

namespace overworld {
// The worker owns every HWND/renderer resource; only value snapshots cross the
// mailbox. Neither publication nor a timed-out shutdown leaves a borrowed Host.
// The borderless body moves/resizes through native hit tests without activation;
// right-clicking the visible artwork closes only this companion.
class ControllerViewWindow {
public:
    ControllerViewWindow();
    ~ControllerViewWindow();
    ControllerViewWindow(const ControllerViewWindow&) = delete;
    ControllerViewWindow& operator=(const ControllerViewWindow&) = delete;

    bool start(HINSTANCE instance, bool show = true);
    bool publish(const ControllerViewFrame& frame);
    bool stop();
    // False only after worker-owned HWND, renderer and COM teardown is complete.
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::wstring error() const;
    // Read-only identity of this owned window, never another application's HWND.
    [[nodiscard]] HWND handle() const;

private:
    class State;
    std::shared_ptr<State> state_;
    HANDLE worker_ = nullptr;
};
} // namespace overworld
