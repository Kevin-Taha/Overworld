#pragma once

#include "Core.h"
#include "ControllerDevices.h"

#include <condition_variable>
#include <mutex>
#include <memory>
#include <thread>

namespace gamepad {

[[nodiscard]] Tick monotonic_now();

// Reader-only adapter. Call from one thread; epochs preserve observed connection
// changes even when the UI skips intermediate snapshots in its latest-value mailbox.
class ControllerReader {
public:
    ControllerReader();
    ~ControllerReader();
    Snapshot read();
    Controller read_slot(unsigned index, ControllerReadScope scope = {});
    void service_messages();
    bool shutdown();

private:
    struct Hid;
    std::unique_ptr<Hid> hid_;
    std::uint64_t sequence_ = 0;
    std::array<ControllerEpoch, slot_count> epochs_{};
};

// Only snapshots cross threads; native UI and input decisions remain on their owning thread.
class ControllerPoller {
public:
    void start();
    Snapshot latest();
    void stop();
private:
    std::mutex mutex_;
    std::condition_variable_any wake_;
    Snapshot latest_;
    std::jthread thread_; // Joins before the mailbox and wait primitives are destroyed.
};

} // namespace gamepad
