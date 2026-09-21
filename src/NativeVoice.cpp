#include "NativeVoice.h"
#include "AudioCapture.h"
#include "VoiceRecognizer.h"
#include "XInputReader.h"
#include <combaseapi.h>

#include <mutex>
#include <new>
#include <system_error>

namespace overworld {
struct NativeVoice::State {
    struct Job {
        std::uint64_t generation = 0;
        std::filesystem::path model;
        std::atomic<bool> cancelled{false}, released{false};
        std::atomic<Tick> capture_expires{0};
        HANDLE changed = nullptr;
        ~Job() { if (changed) { CloseHandle(changed); } }
        void cancel() { cancelled.store(true); if (changed) { SetEvent(changed); } }
    };
    std::mutex mutex;
    std::shared_ptr<Job> active, pending;
    VoiceUpdate update;
    std::uint64_t desired = 0;
    std::atomic<bool> stopping{false};
    HANDLE command = nullptr;
    ~State() { if (command) { CloseHandle(command); } }
    void publish(const Job& job, VoiceBackendPhase phase, float level = 0, std::wstring error = {}, std::u32string text = {})
    {
        std::lock_guard lock(mutex);
        if (desired != job.generation || job.cancelled.load() || stopping.load()) { return; }
        update = {job.generation, phase, level, std::move(text), std::move(error)};
    }
};
NativeVoice::~NativeVoice() { shutdown(); }
void NativeVoice::configure(bool enabled, std::filesystem::path model)
{
    if (enabled_ == enabled && model_ == model) { return; }
    cancel();
    enabled_ = enabled;
    model_ = std::move(model);
}
void NativeVoice::begin(std::uint64_t generation, Tick capture_expires)
{
    cancel();
    error_ = {};
    if (!enabled_ || !VoiceRecognizer::supported()) {
        error_ = {generation, VoiceBackendPhase::Error, 0, {}, L"Local voice is disabled or unavailable on this build/CPU"};
        return;
    }
    if (!voice_capture_authorized(capture_expires, monotonic_now())) {
        error_ = {generation, VoiceBackendPhase::Error, 0, {}, L"Microphone authorization expired"};
        return;
    }
    try {
        if (thread_ && WaitForSingleObject(thread_, 0) == WAIT_OBJECT_0) {
            CloseHandle(thread_); thread_ = nullptr; state_.reset();
        }
        if (!state_) {
            state_ = std::make_shared<State>();
            state_->command = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!state_->command) { throw std::system_error(static_cast<int>(GetLastError()), std::system_category()); }
            auto transfer = std::make_unique<std::shared_ptr<State>>(state_);
            thread_ = CreateThread(nullptr, 0, run, transfer.get(), 0, nullptr);
            if (!thread_) { throw std::system_error(static_cast<int>(GetLastError()), std::system_category()); }
            transfer.release();
        }
        auto job = std::make_shared<State::Job>();
        job->generation = generation;
        job->capture_expires.store(capture_expires);
        job->model = model_;
        job->changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!job->changed) { throw std::system_error(static_cast<int>(GetLastError()), std::system_category()); }
        {
            std::lock_guard lock(state_->mutex);
            state_->desired = generation;
            state_->pending = job;
            state_->update = {generation, VoiceBackendPhase::Loading, 0, {}, {}};
        }
        if (!SetEvent(state_->command)) { throw std::system_error(static_cast<int>(GetLastError()), std::system_category()); }
    } catch (const std::bad_alloc&) {
        cancel();
        if (!thread_) { state_.reset(); }
        error_ = {generation, VoiceBackendPhase::Error, 0, {}, L"Local voice allocation failed"};
    } catch (const std::system_error&) {
        cancel();
        if (!thread_) { state_.reset(); }
        error_ = {generation, VoiceBackendPhase::Error, 0, {}, L"Local voice worker/event initialization failed"};
    }
}
void NativeVoice::renew(std::uint64_t generation, Tick capture_expires)
{
    if (!state_) { return; }
    std::lock_guard lock(state_->mutex);
    if (state_->desired != generation) { return; }
    for (const auto& job : {state_->pending, state_->active}) {
        if (job && job->generation == generation && !job->cancelled.load() && !job->released.load()) {
            // Never revive a capture generation across an expired lease, even if
            // the latest controller sample is itself still fresh after validation.
            const auto previous = job->capture_expires.load();
            const auto now = monotonic_now();
            if (!voice_capture_renewable(previous, capture_expires, now)) {
                job->cancel();
                state_->update = {generation, VoiceBackendPhase::Error, 0, {},
                    L"Microphone authorization expired or invalid; release and press again"};
                continue;
            }
            job->capture_expires.store(capture_expires);
        }
    }
}
void NativeVoice::finish(std::uint64_t generation)
{
    if (!state_) { return; }
    std::lock_guard lock(state_->mutex);
    for (const auto& job : {state_->pending, state_->active}) {
        if (job && job->generation == generation) { job->released.store(true); SetEvent(job->changed); }
    }
}
void NativeVoice::cancel()
{
    error_ = {};
    if (!state_) { return; }
    std::lock_guard lock(state_->mutex);
    state_->desired = 0;
    if (state_->pending) { state_->pending->cancel(); state_->pending.reset(); }
    if (state_->active) { state_->active->cancel(); }
    state_->update = {};
}
VoiceUpdate NativeVoice::poll()
{
    if (error_.phase == VoiceBackendPhase::Error) { return error_; }
    if (!state_) { return {}; }
    std::lock_guard lock(state_->mutex);
    auto update = state_->update;
    if (update.phase == VoiceBackendPhase::Ready) { state_->update.text.clear(); }
    return update;
}
void NativeVoice::shutdown()
{
    cancel();
    if (!state_) { return; }
    state_->stopping.store(true);
    if (state_->command) { SetEvent(state_->command); }
    if (thread_) {
        // Slow model loading/inference may finish later; the worker retains shared
        // state and has no Host/HWND callbacks, and cancellation disallows publication.
        WaitForSingleObject(thread_, 250);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    state_.reset();
}
DWORD WINAPI NativeVoice::run(void* value)
{
    std::unique_ptr<std::shared_ptr<State>> transfer{static_cast<std::shared_ptr<State>*>(value)};
    auto state = *transfer; transfer.reset();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_ptr<VoiceRecognizer> recognizer;
    std::filesystem::path loaded;
    while (!state->stopping.load()) {
        if (WaitForSingleObject(state->command, INFINITE) != WAIT_OBJECT_0) {
            std::lock_guard lock(state->mutex);
            state->update = {state->desired, VoiceBackendPhase::Error, 0, {}, L"Local voice worker wait failed"};
            break;
        }
        if (state->stopping.load()) { break; }
        std::shared_ptr<State::Job> job;
        {
            std::lock_guard lock(state->mutex);
            job = std::move(state->pending);
            state->active = job;
        }
        if (!job) { continue; }
        try {
            std::wstring error;
            if (FAILED(com)) { state->publish(*job, VoiceBackendPhase::Error, 0, L"Microphone COM initialization failed"); continue; }
            if (!recognizer || loaded != job->model) {
                recognizer = std::make_unique<VoiceRecognizer>();
                loaded.clear();
                if (!recognizer->load(job->model, error)) {
                    recognizer.reset();
                    state->publish(*job, VoiceBackendPhase::Error, 0, std::move(error)); continue;
                }
                loaded = job->model;
            }
            if (job->cancelled.load() || state->stopping.load()) { continue; }
            const auto capture_expires = job->capture_expires.load();
            const auto capture_now = monotonic_now();
            if (!voice_capture_authorized(capture_expires, capture_now)) {
                state->publish(*job, VoiceBackendPhase::Error, 0, L"Microphone authorization expired during model loading");
                continue;
            }
            std::vector<float> audio;
            const auto listening = [&](float level) { state->publish(*job, VoiceBackendPhase::Listening, level); };
            if (!capture_voice(job->cancelled, job->released, job->capture_expires, job->changed, listening, audio, error)) {
                state->publish(*job, VoiceBackendPhase::Error, 0, std::move(error)); continue;
            }
            state->publish(*job, VoiceBackendPhase::Transcribing);
            std::u32string text;
            if (!recognizer->transcribe(audio, job->cancelled, text, error)) {
                state->publish(*job, VoiceBackendPhase::Error, 0, std::move(error)); continue;
            }
            state->publish(*job, VoiceBackendPhase::Ready, 0, {}, std::move(text));
        } catch (const std::bad_alloc&) {
            state->publish(*job, VoiceBackendPhase::Error, 0, L"Local voice memory exhausted");
        } catch (const std::filesystem::filesystem_error&) {
            state->publish(*job, VoiceBackendPhase::Error, 0, L"Local model path could not be accessed");
        } catch (const std::exception&) {
            state->publish(*job, VoiceBackendPhase::Error, 0, L"Local voice provider failed; recording and recognition stopped");
        }
    }
    if (SUCCEEDED(com)) { CoUninitialize(); }
    return 0;
}
} // namespace overworld
