#pragma once

#include <string_view>

namespace overworld {
enum class VoicePhase { Idle, Loading, Listening, Transcribing, Error };

// Presentation snapshot only. No microphone, recognizer, transcript or ownership.
struct VoicePresentation {
    VoicePhase phase = VoicePhase::Idle;
    float level = 0;
    std::wstring_view hint{};
};
} // namespace overworld
