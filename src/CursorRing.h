#pragma once

#include "CursorPresentation.h"

#include <Windows.h>
#include <string>

namespace overworld {
// UI-thread-only visual. Never owns input, capture, or a system cursor.
class CursorRing final : public CursorOverlay {
public:
    ~CursorRing();
    CursorRing() = default;
    CursorRing(const CursorRing&) = delete;
    CursorRing& operator=(const CursorRing&) = delete;
    bool present(int diameter, CursorStyle style) override;
    void hide() override;
    void reset() override; // Caller must first restore any owned native hiding.
    [[nodiscard]] const std::wstring& error() const override { return error_; }
    [[nodiscard]] HWND window() const { return window_; }
    // Also used by the separately invoked native smoke harness with its own HWNDs.
    bool show_at(live::Point hotspot, unsigned dpi, int diameter, CursorStyle style = CursorStyle::Ring);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    bool fail(std::wstring_view operation);
    void release_bitmap();
    HWND window_ = nullptr;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    unsigned dpi_ = 0;
    int diameter_ = 0;
    CursorStyle style_ = CursorStyle::Ring;
    live::Point position_{};
    bool visible_ = false;
    std::wstring error_;
};
} // namespace overworld
