#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gamepad {
constexpr unsigned voice_sample_rate = 16000;
constexpr unsigned voice_max_seconds = 25;
constexpr std::size_t voice_max_samples = voice_sample_rate * voice_max_seconds;
constexpr std::size_t voice_max_characters = 1024;
[[nodiscard]] bool decode_audio_frame(std::span<const std::uint8_t> frame, unsigned channels,
    unsigned bytes_per_sample, bool floating, float& mono);

// Incremental mono resampling; state crosses packets, never duplicates packet boundaries.
class AudioResampler {
public:
    explicit AudioResampler(unsigned source_rate) : source_rate_(source_rate) {}
    bool append(float mono, std::vector<float>& output);
private:
    unsigned source_rate_;
    std::uint64_t frame_ = 0, next_ = 0;
    float previous_ = 0;
};
[[nodiscard]] bool voice_has_signal(std::span<const float> samples);
[[nodiscard]] bool decode_voice_wav(std::span<const std::uint8_t> bytes, std::vector<float>& samples, std::wstring& error);
[[nodiscard]] bool normalize_dictation(std::string_view utf8, std::u32string& text, std::wstring& error);
} // namespace gamepad
