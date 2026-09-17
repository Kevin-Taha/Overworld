#pragma once

namespace gamepad {
// Committed close is inert even while a modal warning pumps the UI message loop.
class HostShutdown {
public:
    [[nodiscard]] bool allows_work() const noexcept { return !closing_; }
    bool begin() noexcept
    {
        if (closing_) { return false; }
        closing_ = true;
        return true;
    }
    void veto() noexcept { if (!finished_) { closing_ = false; } }
    void finish() noexcept { closing_ = finished_ = true; }
private:
    bool closing_ = false;
    bool finished_ = false;
};
} // namespace gamepad
