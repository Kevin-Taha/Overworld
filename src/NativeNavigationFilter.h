#pragma once

#include "NavigationFilter.h"
#include <Windows.h>
#include <memory>
#include <string>

namespace gamepad {
// UI-owned lifecycle; a dedicated hook thread owns all callback state.
class NativeNavigationFilter {
public:
    NativeNavigationFilter() = default;
    ~NativeNavigationFilter();
    NativeNavigationFilter(const NativeNavigationFilter&) = delete;
    NativeNavigationFilter& operator=(const NativeNavigationFilter&) = delete;
    void set_enabled(bool enabled);
    void publish(NavigationPolicy policy);
    void suspend();
    bool shutdown();
    [[nodiscard]] std::wstring status() const;
    [[nodiscard]] bool faulted() const;
private:
    struct State;
    static DWORD WINAPI run(void* context);
    static LRESULT CALLBACK hook_proc(int code, WPARAM message, LPARAM data);
    static thread_local State* callback_;
    bool reap();
    std::shared_ptr<State> state_;
    HANDLE thread_ = nullptr;
    bool requested_ = false;
    NavigationShutdown shutdown_;
    DWORD start_error_ = 0;
};
} // namespace gamepad
