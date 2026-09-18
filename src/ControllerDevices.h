#pragma once

#include "Core.h"
#include "ControllerReadScope.h"

namespace gamepad {
using ControllerDeviceId = std::array<std::uint8_t, 16>;
struct ControllerDevices {
    std::array<ControllerDeviceId, 32> ids{};
    unsigned count = 0;
    std::uint32_t error = 0;
};
class ControllerBackend {
public:
    virtual ~ControllerBackend() = default;
    virtual ControllerDevices discover() = 0;
    virtual std::uint32_t open(unsigned slot, const ControllerDeviceId& id) = 0;
    virtual Controller sample(unsigned slot) = 0;
    virtual Controller sample(unsigned slot, ControllerReadScope scope);
    virtual void close(unsigned slot) = 0;
    virtual void describe(unsigned slot, Controller& value) const = 0;
};
class ControllerDevicesReader {
public:
    explicit ControllerDevicesReader(ControllerBackend& backend) : backend_(backend) {}
    Controller read(unsigned slot, Tick now, ControllerReadScope scope = {});
    void suspend(unsigned slot);
private:
    struct Slot {
        ControllerDeviceId id{};
        bool assigned = false, opened = false;
        Tick retry_at = 0;
        std::uint64_t epoch = 0;
        Controller last{};
    };
    Controller finish(unsigned slot, Controller value);
    ControllerBackend& backend_;
    ControllerDevices devices_{};
    std::array<Slot, slot_count> slots_{};
    Tick discovery_at_ = 0, last_read_ = 0;
    bool started_ = false;
};
enum class ControllerApi { XInput, DirectInput };
class ControllerEpoch {
public:
    Controller update(Controller value, ControllerApi source)
    {
        if (source_ != source || connection_ != value.connection || native_epoch_ != value.connection_epoch) { ++epoch_; }
        source_ = source;
        connection_ = value.connection;
        native_epoch_ = value.connection_epoch;
        value.connection_epoch = epoch_;
        return value;
    }
private:
    ControllerApi source_ = ControllerApi::XInput;
    Connection connection_ = Connection::Unknown;
    std::uint64_t epoch_ = 0, native_epoch_ = 0;
};
} // namespace gamepad
