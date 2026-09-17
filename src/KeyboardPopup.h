#pragma once

#include "KeyboardDelivery.h"
#include "VoicePresentation.h"

#include <Windows.h>
#include <memory>
#include <string>

namespace gamepad {
class KeyboardVisual;
constexpr UINT keyboard_popup_message = WM_APP + 4;

class KeyboardPopup {
public:
    KeyboardPopup();
    ~KeyboardPopup();
    KeyboardPopup(const KeyboardPopup&) = delete;
    KeyboardPopup& operator=(const KeyboardPopup&) = delete;
    bool update(HWND host, KeyboardDelivery& keyboard, const VoicePresentation& voice = {});
    void close();
    [[nodiscard]] const std::wstring& error() const { return error_; }
private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    bool create(HWND host);
    void paint();
    HWND window_ = nullptr;
    HWND host_ = nullptr;
    KeyboardDelivery* keyboard_ = nullptr;
    std::unique_ptr<KeyboardVisual> visual_;
    std::wstring error_;
    VoicePhase voice_phase_ = VoicePhase::Idle;
    float voice_level_ = 0;
    std::wstring voice_hint_;
    bool shown_ = false;
};
} // namespace gamepad
