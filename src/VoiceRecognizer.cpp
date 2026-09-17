#include "VoiceRecognizer.h"
#include "VoiceModel.h"

#include <algorithm>
#include <chrono>
#include <thread>
#if defined(GAMEPAD_VOICE_AVX2) && defined(_M_X64)
#include <Windows.h>
#include <intrin.h>
#endif
#ifdef GAMEPAD_VOICE_WHISPER
#include <whisper.h>
#endif

namespace gamepad {
struct VoiceRecognizer::State {
#ifdef GAMEPAD_VOICE_WHISPER
    whisper_context* context = nullptr;
    ~State() { if (context) { whisper_free(context); } }
#endif
};
VoiceRecognizer::VoiceRecognizer() : state_(std::make_unique<State>()) {}
VoiceRecognizer::~VoiceRecognizer() = default;
bool VoiceRecognizer::supported()
{
#ifdef GAMEPAD_VOICE_WHISPER
#if defined(GAMEPAD_VOICE_AVX2) && defined(_M_X64)
    if (!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) { return false; }
    int basic[4]{}, extended[4]{};
    __cpuidex(basic, 1, 0);
    __cpuidex(extended, 7, 0);
    return (basic[2] & ((1 << 12) | (1 << 29))) == ((1 << 12) | (1 << 29))
        && (extended[1] & ((1 << 5) | (1 << 8))) == ((1 << 5) | (1 << 8));
#else
    return true;
#endif
#else
    return false;
#endif
}
bool VoiceRecognizer::load(const std::filesystem::path& model, std::wstring& error)
{
#ifdef GAMEPAD_VOICE_WHISPER
    if (!supported()) { error = L"This voice build requires AVX2/FMA/F16C/BMI2; use the portable build"; return false; }
    // Suppress provider text/progress logging entirely; transcripts never enter diagnostics.
    whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
    if (state_->context) { return true; }
    std::vector<std::uint8_t> verified_bytes;
    if (!load_voice_model(model, verified_bytes, error)) { return false; }
    auto parameters = whisper_context_default_params();
    parameters.use_gpu = false;
    state_->context = whisper_init_from_buffer_with_params(verified_bytes.data(), verified_bytes.size(), parameters);
    if (!state_->context) { error = L"Local speech model could not load"; return false; }
    return true;
#else
    (void)model;
    error = L"Local voice backend is not included in this build";
    return false;
#endif
}
bool VoiceRecognizer::transcribe(std::span<const float> audio, const std::atomic<bool>& cancelled,
    std::u32string& text, std::wstring& error)
{
    text.clear();
    if (!voice_has_signal(audio)) { error = L"No audible speech captured"; return false; }
#ifdef GAMEPAD_VOICE_WHISPER
    if (!state_->context) { error = L"Local voice model is not loaded"; return false; }
    struct Abort {
        const std::atomic<bool>& cancelled;
        std::chrono::steady_clock::time_point deadline;
    } abort{cancelled, std::chrono::steady_clock::now() + std::chrono::seconds(45)};
    auto parameters = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    parameters.n_threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
    parameters.language = "en";
    parameters.translate = false;
    parameters.no_context = true;
    parameters.no_timestamps = true;
    parameters.print_progress = parameters.print_realtime = parameters.print_timestamps = parameters.print_special = false;
    parameters.abort_callback = [](void* value) {
        const auto& state = *static_cast<Abort*>(value);
        return state.cancelled.load() || std::chrono::steady_clock::now() >= state.deadline;
    };
    parameters.abort_callback_user_data = &abort;
    if (cancelled.load() || whisper_full(state_->context, parameters, audio.data(), static_cast<int>(audio.size())) != 0
        || cancelled.load() || std::chrono::steady_clock::now() >= abort.deadline) {
        error = L"Local recognition cancelled or timed out"; return false;
    }
    std::string utf8;
    const int count = whisper_full_n_segments(state_->context);
    for (int i = 0; i < count; ++i) {
        const char* segment = whisper_full_get_segment_text(state_->context, i);
        if (!segment) { error = L"Local recognition returned no text"; return false; }
        const auto length = std::char_traits<char>::length(segment);
        if (length > voice_max_characters * 4 - utf8.size()) { error = L"Dictation exceeds 1024 characters"; return false; }
        utf8.append(segment, length);
    }
    return normalize_dictation(utf8, text, error);
#else
    (void)cancelled;
    error = L"Local voice backend is not included in this build";
    return false;
#endif
}
} // namespace gamepad
