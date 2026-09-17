#include "Audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gamepad {
bool decode_audio_frame(std::span<const std::uint8_t> frame, unsigned channels,
    unsigned bytes_per_sample, bool floating, float& mono)
{
    mono = 0;
    if (!channels || channels > 8 || frame.size() != channels * bytes_per_sample
        || (floating ? bytes_per_sample != 4 : bytes_per_sample < 2 || bytes_per_sample > 4)) { return false; }
    for (unsigned channel = 0; channel < channels; ++channel) {
        const auto* source = frame.data() + channel * bytes_per_sample;
        float sample = 0;
        if (floating) { std::memcpy(&sample, source, sizeof(sample)); }
        else if (bytes_per_sample == 2) {
            std::int16_t value; std::memcpy(&value, source, 2); sample = value / 32768.0f;
        } else if (bytes_per_sample == 4) {
            std::int32_t value; std::memcpy(&value, source, 4); sample = static_cast<float>(value / 2147483648.0);
        } else {
            const auto raw = static_cast<std::uint32_t>(source[0]) | (static_cast<std::uint32_t>(source[1]) << 8)
                | (static_cast<std::uint32_t>(source[2]) << 16);
            const auto value = static_cast<std::int32_t>((raw & 0x800000) ? raw | 0xff000000 : raw);
            sample = value / 8388608.0f;
        }
        if (!std::isfinite(sample)) { return false; }
        mono += std::clamp(sample, -1.0f, 1.0f) / channels;
    }
    return true;
}

bool AudioResampler::append(float mono, std::vector<float>& output)
{
    if (source_rate_ < 8000 || source_rate_ > 192000 || !std::isfinite(mono)) { return false; }
    mono = std::clamp(mono, -1.0f, 1.0f);
    const auto position = frame_ * voice_sample_rate;
    while (next_ <= position) {
        if (output.size() == voice_max_samples) { return false; }
        const auto prior = frame_ ? position - voice_sample_rate : 0;
        const float fraction = frame_ ? static_cast<float>(next_ - prior) / voice_sample_rate : 1.0f;
        output.push_back(previous_ + (mono - previous_) * fraction);
        next_ += source_rate_;
    }
    previous_ = mono;
    ++frame_;
    return true;
}
bool voice_has_signal(std::span<const float> samples)
{
    if (samples.size() < voice_sample_rate / 4 || samples.size() > voice_max_samples) { return false; }
    double energy = 0;
    for (const float sample : samples) {
        if (!std::isfinite(sample)) { return false; }
        energy += static_cast<double>(sample) * sample;
    }
    return std::sqrt(energy / samples.size()) >= 0.003;
}

bool decode_voice_wav(std::span<const std::uint8_t> bytes, std::vector<float>& samples, std::wstring& error)
{
    samples.clear();
    const auto fail = [&] { error = L"Fixture must be bounded mono 16-bit PCM WAV at 16 kHz"; return false; };
    const auto u16 = [&](std::size_t at) { return static_cast<unsigned>(bytes[at]) | (static_cast<unsigned>(bytes[at + 1]) << 8); };
    const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
    if (bytes.size() < 12 || bytes.size() > voice_max_samples * 2 + 65536
        || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)
        || u32(4) != bytes.size() - 8) { return fail(); }
    bool format = false, data = false;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const auto length = u32(at + 4);
        const auto start = at + 8;
        if (length > bytes.size() - start) { return fail(); }
        if (!std::memcmp(bytes.data() + at, "fmt ", 4)) {
            if (format || length < 16 || u16(start) != 1 || u16(start + 2) != 1
                || u32(start + 4) != voice_sample_rate || u16(start + 12) != 2 || u16(start + 14) != 16) { return fail(); }
            format = true;
        } else if (!std::memcmp(bytes.data() + at, "data", 4)) {
            if (!format || data || length % 2 || length / 2 > voice_max_samples) { return fail(); }
            data = true;
            samples.reserve(length / 2);
            for (std::size_t i = start; i < start + length; i += 2) {
                samples.push_back(static_cast<std::int16_t>(u16(i)) / 32768.0f);
            }
        }
        at = start + length + (length & 1u);
    }
    if (!data || samples.empty()) { return fail(); }
    error.clear();
    return true;
}

bool normalize_dictation(std::string_view utf8, std::u32string& text, std::wstring& error)
{
    text.clear();
    const auto fail = [&] { text.clear(); error = L"Recognition returned invalid or excessive text"; return false; };
    if (utf8.size() > voice_max_characters * 4) { return fail(); }
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i++]);
        char32_t scalar = lead;
        unsigned remaining = 0;
        char32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { remaining = 1; scalar &= 0x1f; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { remaining = 2; scalar &= 0xf; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { remaining = 3; scalar &= 7; minimum = 0x10000; }
        else if (lead >= 0x80) { return fail(); }
        if (remaining > utf8.size() - i) { return fail(); }
        for (unsigned n = 0; n < remaining; ++n) {
            const auto next = static_cast<unsigned char>(utf8[i++]);
            if ((next & 0xc0) != 0x80) { return fail(); }
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) { return fail(); }
        if (scalar == U'\r' || scalar == U'\n' || scalar == U'\t' || scalar == U' ') {
            if (text.empty() || text.back() == U' ') { continue; }
            scalar = U' '; // Dictation never becomes Enter or another keyboard command.
        } else if (scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f)) { return fail(); }
        if (text.size() == voice_max_characters) { return fail(); }
        text.push_back(scalar);
    }
    if (!text.empty() && text.back() == U' ') { text.pop_back(); }
    if (text.empty()) { error = L"No speech recognized"; return false; }
    error.clear();
    return true;
}
} // namespace gamepad
