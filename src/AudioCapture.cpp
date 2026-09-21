#include "AudioCapture.h"
#include "XInputReader.h"

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace overworld {
bool capture_voice(const std::atomic<bool>& cancelled, const std::atomic<bool>& released,
    const std::atomic<Tick>& capture_expires, HANDLE changed, const std::function<void(float)>& listening,
    std::vector<float>& audio, std::wstring& error)
{
    using Microsoft::WRL::ComPtr;
    const auto fail = [&](std::wstring_view operation, HRESULT result) {
        error = std::wstring{operation} + L" (HRESULT " + std::to_wstring(result) + L")"; return false;
    };
    const auto authorized = [&] {
        const auto expires = capture_expires.load();
        const auto now = monotonic_now();
        return voice_capture_authorized(expires, now);
    };
    audio.clear();
    if (!authorized() || cancelled.load() || released.load()) { error = L"Microphone authorization expired or released"; return false; }
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) { return fail(L"Microphone enumerator unavailable", result); }
    ComPtr<IMMDevice> device;
    result = enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
    if (FAILED(result)) { return fail(L"No default microphone available", result); }
    ComPtr<IAudioClient> client;
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &client);
    if (FAILED(result)) { return fail(L"Microphone unavailable or access denied", result); }
    WAVEFORMATEX* format = nullptr;
    result = client->GetMixFormat(&format);
    if (FAILED(result)) { return fail(L"Microphone format unavailable", result); }
    struct FormatGuard { WAVEFORMATEX* value; ~FormatGuard() { CoTaskMemFree(value); } } format_guard{format};
    bool floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcm = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
        floating = extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        pcm = extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    }
    const unsigned bytes = format->wBitsPerSample / 8;
    if ((!floating && !pcm) || (floating && bytes != 4) || (pcm && bytes != 2 && bytes != 3 && bytes != 4)
        || format->nChannels < 1 || format->nChannels > 8 || format->nSamplesPerSec < 8000
        || format->nSamplesPerSec > 192000 || format->nBlockAlign != bytes * format->nChannels) {
        error = L"Microphone sample format unsupported"; return false;
    }
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, format, nullptr);
    if (FAILED(result)) { return fail(L"Microphone initialization failed; check app microphone permission", result); }
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) { return fail(L"Microphone event unavailable", HRESULT_FROM_WIN32(GetLastError())); }
    struct EventGuard { HANDLE value; ~EventGuard() { CloseHandle(value); } } event_guard{ready};
    result = client->SetEventHandle(ready);
    if (FAILED(result)) { return fail(L"Microphone event setup failed", result); }
    ComPtr<IAudioCaptureClient> capture;
    result = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(result)) { return fail(L"Microphone capture unavailable", result); }
    if (cancelled.load() || released.load()) { error = L"Hold right-stick click until Listening before speaking"; return false; }
    audio.reserve(voice_max_samples);
    AudioResampler resampler{format->nSamplesPerSec};
    if (!authorized() || cancelled.load() || released.load()) { error = L"Microphone authorization expired before start"; return false; }
    result = client->Start();
    if (FAILED(result)) { return fail(L"Microphone start failed", result); }
    struct StopGuard { IAudioClient* client; ~StopGuard() { if (client) { client->Stop(); } } } stop_guard{client.Get()};
    const auto deadline = monotonic_now() + voice_max_seconds * 1000;
    listening(0);
    const HANDLE events[]{changed, ready};
    while (!cancelled.load() && !released.load()) {
        if (!authorized()) { error = L"Microphone authorization expired; recording cancelled"; return false; }
        if (monotonic_now() >= deadline) { error = L"Recording limit reached (25 seconds); release and try again"; return false; }
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 50);
        if (wait == WAIT_FAILED) { return fail(L"Microphone wait failed", HRESULT_FROM_WIN32(GetLastError())); }
        if (cancelled.load() || released.load()) { break; }
        if (!authorized()) { error = L"Microphone authorization expired; recording cancelled"; return false; }
        UINT32 packet = 0;
        result = capture->GetNextPacketSize(&packet);
        if (FAILED(result)) { return fail(L"Microphone disconnected", result); }
        while (packet && !cancelled.load() && !released.load()) {
            if (!authorized()) { error = L"Microphone authorization expired; recording cancelled"; return false; }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) { return fail(L"Microphone read failed", result); }
            bool valid = frames <= format->nSamplesPerSec && (data || (flags & AUDCLNT_BUFFERFLAGS_SILENT));
            bool authorization_lost = false;
            double energy = 0;
            for (UINT32 frame = 0; valid && frame < frames; ++frame) {
                if ((frame & 255u) == 0 && (!authorized() || cancelled.load())) {
                    authorization_lost = true;
                    valid = false;
                    break;
                }
                float mono = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    valid = decode_audio_frame({data + frame * format->nBlockAlign, format->nBlockAlign},
                        format->nChannels, bytes, floating, mono);
                }
                energy += static_cast<double>(mono) * mono;
                valid = valid && resampler.append(mono, audio);
            }
            result = capture->ReleaseBuffer(frames);
            if (FAILED(result)) { return fail(L"Microphone buffer release failed", result); }
            if (authorization_lost) { error = L"Microphone authorization expired or recording cancelled"; return false; }
            if (!valid) { error = L"Microphone data invalid or recording limit reached"; return false; }
            listening(frames ? std::clamp(static_cast<float>(std::sqrt(energy / frames) * 5), 0.0f, 1.0f) : 0);
            result = capture->GetNextPacketSize(&packet);
            if (FAILED(result)) { return fail(L"Microphone disconnected", result); }
        }
    }
    result = client->Stop();
    stop_guard.client = nullptr;
    if (FAILED(result)) { return fail(L"Microphone stop failed", result); }
    if (cancelled.load()) { error = L"Recording cancelled"; return false; }
    if (!voice_has_signal(audio)) { error = L"No audible speech captured"; return false; }
    return true;
}
} // namespace overworld
