#include "ControllerDevices.h"

#include <Windows.h>
#include <algorithm>

namespace gamepad {
namespace {
Controller state(Connection connection, std::uint32_t error)
{
    Controller value;
    value.connection = connection;
    value.error = error;
    return value;
}
}
Controller ControllerBackend::sample(unsigned slot, ControllerReadScope scope)
{
    return scope.allowed() ? sample(slot) : state(Connection::Error, ERROR_CANCELLED);
}
void ControllerDevicesReader::suspend(unsigned slot)
{
    auto& current = slots_[slot];
    if (current.assigned) { backend_.close(slot); ++current.epoch; }
    current.assigned = current.opened = false;
    current.retry_at = 0;
}
Controller ControllerDevicesReader::finish(unsigned slot, Controller value)
{
    auto& current = slots_[slot];
    if (value.connection != current.last.connection) { ++current.epoch; }
    value.connection_epoch = current.epoch;
    if (current.assigned) { backend_.describe(slot, value); }
    current.last = value;
    return value;
}
Controller ControllerDevicesReader::read(unsigned slot, Tick now, ControllerReadScope scope)
{
    if (slot >= slot_count) { return state(Connection::Error, ERROR_INVALID_PARAMETER); }
    if (!scope.allowed()) { return state(Connection::Error, ERROR_CANCELLED); }
    if (started_ && now < last_read_) {
        for (unsigned i = 0; i < slot_count; ++i) { suspend(i); }
        started_ = false;
        return finish(slot, state(Connection::Error, ERROR_INVALID_TIME));
    }
    last_read_ = now;
    if (!started_ || now >= discovery_at_) {
        started_ = true;
        discovery_at_ = now + 1000;
        devices_ = backend_.discover();
        if (devices_.count > devices_.ids.size()) { devices_.error = ERROR_MORE_DATA; }
        if (!devices_.error) {
            for (unsigned i = 0; i < devices_.count; ++i) {
                for (unsigned j = 0; j < i; ++j) {
                    if (devices_.ids[i] == devices_.ids[j]) { devices_.error = ERROR_DUP_NAME; }
                }
            }
        }
        for (unsigned i = 0; i < slot_count; ++i) {
            if (devices_.error || (slots_[i].assigned && std::find(devices_.ids.begin(),
                devices_.ids.begin() + devices_.count, slots_[i].id) == devices_.ids.begin() + devices_.count)) {
                suspend(i);
            }
        }
    }
    if (devices_.error) { return finish(slot, state(Connection::Error, devices_.error)); }
    if (!scope.allowed()) { return finish(slot, state(Connection::Error, ERROR_CANCELLED)); }
    auto& current = slots_[slot];
    if (!current.assigned) {
        for (unsigned i = 0; i < devices_.count; ++i) {
            const auto& id = devices_.ids[i];
            if (std::none_of(slots_.begin(), slots_.end(), [&](const Slot& other) { return other.assigned && other.id == id; })) {
                current.id = id;
                current.assigned = true;
                ++current.epoch;
                break;
            }
        }
    }
    if (!current.assigned) { return finish(slot, state(Connection::Disconnected, ERROR_DEVICE_NOT_CONNECTED)); }
    if (!current.opened) {
        if (now < current.retry_at) { return current.last; }
        current.retry_at = now + 1000;
        if (const auto error = backend_.open(slot, current.id)) {
            backend_.close(slot);
            return finish(slot, state(Connection::Error, error));
        }
        current.opened = true;
        ++current.epoch;
    }
    auto value = backend_.sample(slot, scope);
    if (value.connection != Connection::Connected) {
        backend_.close(slot);
        current.opened = false;
        current.retry_at = now + 1000;
        ++current.epoch;
        // Never retain partially decoded axes/buttons on a failed read.
        value = state(value.connection, value.error);
    }
    return finish(slot, value);
}
} // namespace gamepad
