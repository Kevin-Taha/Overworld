#pragma once

#include "Audio.h"
#include "Core.h"
#include <string>

namespace gamepad {
constexpr Tick voice_capture_lease_ms = 100;
[[nodiscard]] constexpr bool voice_capture_authorized(Tick expires, Tick now) noexcept
{
    return expires != 0 && expires >= now && expires - now <= voice_capture_lease_ms;
}
[[nodiscard]] constexpr bool voice_capture_renewable(Tick previous, Tick replacement, Tick now) noexcept
{
    return voice_capture_authorized(previous, now) && voice_capture_authorized(replacement, now)
        && replacement >= previous;
}
enum class VoiceBackendPhase { Idle, Loading, Listening, Transcribing, Ready, Error };
struct VoiceUpdate {
    std::uint64_t generation = 0;
    VoiceBackendPhase phase = VoiceBackendPhase::Idle;
    float level = 0;
    std::u32string text;
    std::wstring error;
};
class VoiceBoundary {
public:
    virtual ~VoiceBoundary() = default;
    virtual void begin(std::uint64_t generation, Tick capture_expires) = 0;
    virtual void renew(std::uint64_t generation, Tick capture_expires) = 0;
    virtual void finish(std::uint64_t generation) = 0;
    virtual void cancel() = 0;
    virtual VoiceUpdate poll() = 0;
};
} // namespace gamepad
