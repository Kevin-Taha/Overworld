#pragma once

namespace gamepad {
struct ControllerReadScope {
    void* context = nullptr;
    bool (*check)(void*) = nullptr;
    [[nodiscard]] bool allowed() const { return !check || check(context); }
};
} // namespace gamepad
