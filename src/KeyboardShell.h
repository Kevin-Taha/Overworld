#pragma once

#include "KeyboardPlacement.h"
#include <array>

namespace overworld {
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
    struct Root {
        Root() = default;
        ~Root();
        Root(const Root&) = delete;
        Root& operator=(const Root&) = delete;
        void reset();
        std::optional<bool> identify(HWND window);
        bool stable() const;
        HWND window = nullptr;
        DWORD process_id = 0, thread_id = 0;
        HANDLE process = nullptr;
        bool identified = false, shell = false;
    };
    KeyboardShellObservation bounds(Root& root);
    Root foreground_;
    std::array<Root, 32> peers_;
};
} // namespace overworld
