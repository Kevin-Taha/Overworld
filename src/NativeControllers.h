#pragma once

#include "ControllerDevices.h"
#include "ControllerMapping.h"
#include "DirectInputLayout.h"

#include <Windows.h>
#include <dinput.h>
#include <string>

namespace gamepad {
class NativeControllers final : public ControllerBackend {
public:
    ~NativeControllers() override;
    ControllerDevices discover() override;
    std::uint32_t open(unsigned slot, const ControllerDeviceId& id) override;
    Controller sample(unsigned slot) override;
    Controller sample(unsigned slot, ControllerReadScope scope) override;
    void close(unsigned slot) override;
    void describe(unsigned slot, Controller& value) const override;
    void service_messages();
private:
    struct Device {
        IDirectInputDevice8W* handle = nullptr;
        ControllerMapping mapping;
        DirectInputLayout layout;
        std::array<wchar_t, 160> description{};
        std::uint32_t packet = 0;
    };
    DWORD load_mappings();
    IDirectInput8W* input_ = nullptr;
    HWND window_ = nullptr;
    std::array<Device, slot_count> devices_;
    std::string database_, overrides_;
    DWORD mapping_error_ = 0;
    bool mappings_loaded_ = false;
};
} // namespace gamepad
