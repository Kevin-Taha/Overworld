#pragma once

#include "Audio.h"
#include "Voice.h"
#include <Windows.h>
#include <atomic>
#include <functional>

namespace overworld {
// Called only on a COM-initialized worker after an explicit live push-to-talk gesture.
bool capture_voice(const std::atomic<bool>& cancelled, const std::atomic<bool>& released,
    const std::atomic<Tick>& capture_expires, HANDLE changed, const std::function<void(float)>& listening,
    std::vector<float>& audio, std::wstring& error);
}
