#pragma once

#include <Windows.h>

namespace gamepad {
// Shared by the shipping mapper and explicitly built, controller-observing dev tools.
class MapperInstanceGuard {
public:
    MapperInstanceGuard()
    {
        handle_ = CreateMutexW(nullptr, TRUE, L"Local\\GamepadDesktop.M1.SingleMapper");
        existing_ = handle_ && GetLastError() == ERROR_ALREADY_EXISTS;
    }
    ~MapperInstanceGuard()
    {
        if (handle_) {
            if (!existing_) { ReleaseMutex(handle_); }
            CloseHandle(handle_);
        }
    }
    MapperInstanceGuard(const MapperInstanceGuard&) = delete;
    MapperInstanceGuard& operator=(const MapperInstanceGuard&) = delete;
    [[nodiscard]] bool acquired() const { return handle_ && !existing_; }
    [[nodiscard]] bool already_running() const { return existing_; }
private:
    HANDLE handle_ = nullptr;
    bool existing_ = false;
};
} // namespace gamepad
