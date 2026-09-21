#pragma once

#include "ControllerDevices.h"
#include "ControllerMapping.h"
#include "DirectInputLayout.h"
#include "ControllerDiscovery.h"

#include <Windows.h>
#include <dinput.h>
#include <string>

namespace overworld {
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
    bool shutdown();
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
    std::unique_ptr<ControllerDiscovery> discovery_;
};
} // namespace overworld
