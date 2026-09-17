#pragma once

#include <Windows.h>

namespace gamepad {
// Shared by the shipping mapper and explicitly built, controller-observing dev tools.
class MapperInstanceGuard {
public:
    explicit MapperInstanceGuard(const wchar_t* name = L"Local\\GamepadDesktop.M1.SingleMapper")
    {
        handle_ = CreateMutexW(nullptr, TRUE, name);
        existing_ = handle_ && GetLastError() == ERROR_ALREADY_EXISTS;
        if (existing_) {
            // A rejected launch must not retain the name while displaying its warning.
            CloseHandle(handle_);
            handle_ = nullptr;
        }
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
