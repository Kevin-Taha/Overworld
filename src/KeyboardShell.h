#pragma once

#include <Windows.h>
#include <optional>

namespace gamepad {
struct KeyboardShellObservation {
    bool available = true;
    std::optional<RECT> obstruction;
};
class KeyboardShellSource {
public:
    virtual ~KeyboardShellSource() = default;
    virtual KeyboardShellObservation observe(HWND foreground) = 0;
};
class NativeKeyboardShell final : public KeyboardShellSource {
public:
    ~NativeKeyboardShell() override;
    KeyboardShellObservation observe(HWND foreground) override;
    void reset();
private:
    HWND window_ = nullptr;
    DWORD process_id_ = 0, thread_id_ = 0;
    HANDLE process_ = nullptr;
    bool identified_ = false, shell_ = false;
};
} // namespace gamepad
