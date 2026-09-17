#pragma once

#include "CursorPresentation.h"

namespace gamepad {
// Lazy and windowless. Does not set magnification/input transforms, schemes, or process privileges.
class NativeCursorVisibility final : public CursorVisibility {
public:
    ~NativeCursorVisibility() override;
    bool initialize() override;
    NativeCursorState observe() override;
    bool show(bool visible) override;
    bool shutdown() override;
    const std::wstring& error() const override { return error_; }

private:
    bool fail(std::wstring_view operation);
    bool initialized_ = false;
    std::wstring error_;
};
} // namespace gamepad
