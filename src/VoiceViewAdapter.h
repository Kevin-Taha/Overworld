#pragma once

#include "Voice.h"
#include "VoicePresentation.h"
#include <algorithm>
#include <cmath>

namespace gamepad {
[[nodiscard]] inline VoicePresentation voice_presentation(const VoiceUpdate& update) noexcept
{
    switch (update.phase) {
    case VoiceBackendPhase::Idle: return {};
    case VoiceBackendPhase::Loading: return {VoicePhase::Loading, 0, L"Loading local voice"};
    case VoiceBackendPhase::Listening:
        return {VoicePhase::Listening, std::isfinite(update.level) ? std::clamp(update.level, 0.0f, 1.0f) : 0,
            L"Release to transcribe"};
    case VoiceBackendPhase::Transcribing: return {VoicePhase::Transcribing, 0, L"Transcribing locally"};
    case VoiceBackendPhase::Ready: return {VoicePhase::Transcribing, 0, L"Inserting dictation"};
    case VoiceBackendPhase::Error: return {VoicePhase::Error, 0, std::wstring_view{update.error}.substr(0, 120)};
    }
    return {VoicePhase::Error, 0, L"Voice state unavailable"};
}
} // namespace gamepad
