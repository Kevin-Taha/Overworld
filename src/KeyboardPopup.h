#pragma once

#include "KeyboardDelivery.h"
#include "VoicePresentation.h"

#include <Windows.h>
#include <memory>
#include <string>

namespace gamepad {
class KeyboardVisual;
class KeyboardThemeSource;
constexpr UINT keyboard_popup_message = WM_APP + 4;

[[nodiscard]] std::optional<RECT> keyboard_popup_bounds(RECT work, UINT dpi, int logical_height,
    std::optional<RECT> anchor = {});

class KeyboardPopup {
public:
    explicit KeyboardPopup(KeyboardThemeSource* theme_source = nullptr);
    ~KeyboardPopup();
    KeyboardPopup(const KeyboardPopup&) = delete;
    KeyboardPopup& operator=(const KeyboardPopup&) = delete;
    bool update(HWND host, KeyboardDelivery& keyboard, const VoicePresentation& voice = {});
    void close();
    [[nodiscard]] const std::wstring& error() const { return error_; }
private:
    friend struct KeyboardPopupTestAccess;
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    bool update_window(HWND host, KeyboardDelivery& keyboard, const VoicePresentation& voice,
        HWND placement_target, bool reveal);
    bool create(HWND host);
    bool place(RECT work, UINT dpi, int logical_height, std::optional<RECT> anchor = {});
    void fail(std::wstring reason);
    void paint();
    HWND window_ = nullptr;
    HWND host_ = nullptr;
    KeyboardDelivery* keyboard_ = nullptr;
    std::unique_ptr<KeyboardVisual> visual_;
    KeyboardThemeSource* theme_source_ = nullptr;
    std::wstring error_;
    VoicePhase voice_phase_ = VoicePhase::Idle;
    float voice_level_ = 0;
    std::wstring voice_hint_;
    bool shown_ = false;
    bool failed_ = false;
    bool positioning_ = false;
    int logical_height_ = 0;
    UINT dpi_ = 96;
};
} // namespace gamepad
