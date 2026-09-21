#include "VoiceRecognizer.h"
#include "VoiceModel.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <chrono>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) { std::cerr << "Usage: overworld_voice_fixture model.bin owned-public-fixture.wav (no microphone)\n"; return 2; }
    std::ifstream file(std::filesystem::path{argv[2]}, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > static_cast<std::streamoff>(overworld::voice_max_samples * 2 + 65536)) {
        std::cerr << "Fixture file missing or exceeds bound\n"; return 1;
    }
    file.seekg(0);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, {}};
    std::vector<float> audio;
    std::wstring error;
    if (!overworld::decode_voice_wav(bytes, audio, error)) { std::wcerr << error << L'\n'; return 1; }
    overworld::VoiceRecognizer recognizer;
    std::vector<std::uint8_t> verified_model;
    if (!overworld::load_voice_model(argv[1], verified_model, error)) { std::wcerr << error << L'\n'; return 1; }
    verified_model[0] ^= 1;
    if (overworld::verified_voice_model(verified_model, error)) {
        std::cerr << "FAIL mutated model accepted\n"; return 1;
    }
    verified_model.clear();
    verified_model.shrink_to_fit();
    const auto started = std::chrono::steady_clock::now();
    if (!recognizer.load(argv[1], error)) { std::wcerr << error << L'\n'; return 1; }
    const auto loaded = std::chrono::steady_clock::now();
    std::atomic<bool> cancelled{false};
    std::u32string text;
    if (!recognizer.transcribe(audio, cancelled, text, error)) { std::wcerr << error << L'\n'; return 1; }
    const bool expected = text.find(U"ask not what your country can do for you") != text.npos;
    const auto recognized_scalars = text.size();
    const auto finished = std::chrono::steady_clock::now();
    cancelled.store(true);
    const bool cancellation_rejected = !recognizer.transcribe(audio, cancelled, text, error) && text.empty();
    cancelled.store(false);
    std::vector<float> silence(overworld::voice_sample_rate, 0.0f);
    const bool silence_rejected = !recognizer.transcribe(silence, cancelled, text, error) && text.empty();
    std::cout << "Public JFK fixture: expected phrase=" << expected << " scalars=" << recognized_scalars
        << " load_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(loaded - started).count()
        << " inference_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished - loaded).count()
        << " cancelled_rejected=" << cancellation_rejected << " silence_rejected=" << silence_rejected
        << " microphone_opened=0\n";
    return expected && cancellation_rejected && silence_rejected ? 0 : 1;
}
