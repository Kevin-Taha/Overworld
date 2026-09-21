#pragma once

#include "ControllerDevices.h"

#include <Windows.h>
#include <atomic>
#include <memory>

namespace overworld {
class ControllerDiscoverySource {
public:
    virtual ~ControllerDiscoverySource() = default;
    virtual ControllerDevices query(const std::atomic<bool>& stop) = 0;
};

// Only metadata crosses this mailbox; it can never supply controller samples.
class ControllerDiscovery {
public:
    explicit ControllerDiscovery(std::unique_ptr<ControllerDiscoverySource> source);
    ~ControllerDiscovery();
    ControllerDiscovery(const ControllerDiscovery&) = delete;
    ControllerDiscovery& operator=(const ControllerDiscovery&) = delete;
    ControllerDevices latest();
    bool stop();
private:
    struct State;
    static DWORD WINAPI worker(void* context);
    std::shared_ptr<State> state_;
    HANDLE thread_ = nullptr;
    bool started_ = false, stopped_ = false, stop_result_ = true;
};
} // namespace overworld
