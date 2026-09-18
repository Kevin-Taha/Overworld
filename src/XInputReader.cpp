#include "XInputReader.h"
#include "NativeControllers.h"

#include <Windows.h>
#include <Xinput.h>
#include <chrono>
#include <new>

namespace gamepad {
struct ControllerReader::Hid {
    NativeControllers backend;
    ControllerDevicesReader reader{backend};
};
ControllerReader::ControllerReader() = default;
ControllerReader::~ControllerReader() = default;
void ControllerReader::service_messages() { if (hid_) { hid_->backend.service_messages(); } }

Tick monotonic_now()
{
    return static_cast<Tick>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

Snapshot ControllerReader::read()
{
    Snapshot snapshot;
    snapshot.sequence = ++sequence_;
    // Timestamp the beginning: a slow native read must not make earlier slots
    // appear newer than they are. The core rejects a batch older than 250 ms.
    snapshot.sampled_at = monotonic_now();
    for (unsigned index = 0; index < slot_count; ++index) {
        snapshot.slots[index] = read_slot(index);
    }
    return snapshot;
}

Controller ControllerReader::read_slot(unsigned index, ControllerReadScope scope)
{
    Controller slot;
    if (index >= slot_count) {
        slot.connection = Connection::Error; slot.error = ERROR_INVALID_PARAMETER; return slot;
    }
    if (!scope.allowed()) {
        slot.connection = Connection::Error; slot.error = ERROR_CANCELLED; return slot;
    }
    XINPUT_STATE state{};
    slot.error = XInputGetState(index, &state);
    slot.connection = slot.error == ERROR_SUCCESS ? Connection::Connected
        : slot.error == ERROR_DEVICE_NOT_CONNECTED ? Connection::Disconnected : Connection::Error;
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
    if (slot.connection == Connection::Disconnected) {
        try {
            if (!hid_) { hid_ = std::make_unique<Hid>(); }
            return epochs_[index].update(hid_->reader.read(index, monotonic_now(), scope), ControllerApi::DirectInput);
        } catch (const std::bad_alloc&) {
            if (hid_) { hid_->reader.suspend(index); }
            slot.connection = Connection::Error;
            slot.error = ERROR_NOT_ENOUGH_MEMORY;
        }
    } else if (hid_) {
        hid_->reader.suspend(index);
    }
    return epochs_[index].update(slot, ControllerApi::XInput);
}

void ControllerPoller::start()
{
    thread_ = std::jthread([this](std::stop_token stop) {
        ControllerReader reader;
        std::mutex wait_mutex;
        std::unique_lock wait_lock(wait_mutex);
        while (!stop.stop_requested()) {
            reader.service_messages();
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
