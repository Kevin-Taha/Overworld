#pragma once

#include "PracticeVisual.h"

namespace gamepad::practice {
class ControllerSource {
public:
    virtual ~ControllerSource() = default;
    virtual void authorize(HWND window) = 0;
    virtual Snapshot read() = 0;
    virtual Tick now() = 0;
};

// With no source, creates an offscreen, hidden render-only window. No controller worker.
class Window {
public:
    Window();
    ~Window();
    bool create(HINSTANCE instance, ControllerSource* source = nullptr);
    int run(int show);
    bool update(std::optional<AppPalette> palette = {});
    [[nodiscard]] HWND handle() const;
    [[nodiscard]] Model& model();
    [[nodiscard]] Visual& visual();
    [[nodiscard]] const std::wstring& error() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
int run(HINSTANCE instance, int show, ControllerSource& source);
} // namespace gamepad::practice
