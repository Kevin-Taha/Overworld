#pragma once

#include "Voice.h"
#include <Windows.h>
#include <filesystem>
#include <memory>

namespace gamepad {
class NativeVoice final : public VoiceBoundary {
public:
    NativeVoice() = default;
    ~NativeVoice() override;
    void configure(bool enabled, std::filesystem::path model);
    void begin(std::uint64_t generation, Tick capture_expires) override;
    void renew(std::uint64_t generation, Tick capture_expires) override;
    void finish(std::uint64_t generation) override;
    void cancel() override;
    VoiceUpdate poll() override;
    void shutdown();
private:
    struct State;
    static DWORD WINAPI run(void* value);
    bool enabled_ = false;
    std::filesystem::path model_;
    std::shared_ptr<State> state_;
    HANDLE thread_ = nullptr;
    VoiceUpdate error_;
};
} // namespace gamepad
