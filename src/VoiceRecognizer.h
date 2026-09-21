#pragma once

#include "Audio.h"
#include <atomic>
#include <filesystem>
#include <memory>

namespace overworld {
class VoiceRecognizer {
public:
    VoiceRecognizer();
    ~VoiceRecognizer();
    VoiceRecognizer(const VoiceRecognizer&) = delete;
    VoiceRecognizer& operator=(const VoiceRecognizer&) = delete;
    bool load(const std::filesystem::path& model, std::wstring& error);
    bool transcribe(std::span<const float> audio, const std::atomic<bool>& cancelled,
        std::u32string& text, std::wstring& error);
    static bool supported();
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace overworld
