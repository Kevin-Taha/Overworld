#include "XInputReader.h"

#include <Windows.h>
#include <Xinput.h>
#include <chrono>

namespace gamepad {

Tick monotonic_now()
{
    return static_cast<Tick>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

Snapshot XInputReader::read()
{
    Snapshot snapshot;
    snapshot.sequence = ++sequence_;
    // Timestamp the beginning: a slow native read must not make earlier slots
    // appear newer than they are. The core rejects a batch older than 250 ms.
    snapshot.sampled_at = monotonic_now();
    for (unsigned index = 0; index < slot_count; ++index) {
        XINPUT_STATE state{};
        auto& slot = snapshot.slots[index];
        slot.error = XInputGetState(index, &state);
        slot.connection = slot.error == ERROR_SUCCESS ? Connection::Connected
            : slot.error == ERROR_DEVICE_NOT_CONNECTED ? Connection::Disconnected : Connection::Error;
        if (connections_[index] != slot.connection) {
            connections_[index] = slot.connection;
            ++epochs_[index];
        }
        slot.connection_epoch = epochs_[index];
        if (slot.connection == Connection::Connected) {
            slot.packet = state.dwPacketNumber;
            slot.buttons = state.Gamepad.wButtons & standard_buttons;
            slot.left_trigger = state.Gamepad.bLeftTrigger;
            slot.right_trigger = state.Gamepad.bRightTrigger;
            slot.left_x = state.Gamepad.sThumbLX;
            slot.left_y = state.Gamepad.sThumbLY;
            slot.right_x = state.Gamepad.sThumbRX;
            slot.right_y = state.Gamepad.sThumbRY;
        }
    }
    return snapshot;
}

void ControllerPoller::start()
{
    thread_ = std::jthread([this](std::stop_token stop) {
        XInputReader reader;
        std::mutex wait_mutex;
        std::unique_lock wait_lock(wait_mutex);
        while (!stop.stop_requested()) {
            const auto next = reader.read();
            {
                std::lock_guard lock(mutex_);
                latest_ = next;
            }
            wake_.wait_for(wait_lock, stop, std::chrono::milliseconds(16), [] { return false; });
        }
    });
}

Snapshot ControllerPoller::latest()
{
    std::lock_guard lock(mutex_);
    return latest_;
}

void ControllerPoller::stop() { thread_.request_stop(); }
} // namespace gamepad
