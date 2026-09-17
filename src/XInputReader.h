#pragma once

#include "Core.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace gamepad {

[[nodiscard]] Tick monotonic_now();

// Reader-only adapter. Call from one thread; epochs preserve observed connection
// changes even when the UI skips intermediate snapshots in its latest-value mailbox.
class XInputReader {
public:
    Snapshot read();

private:
    std::uint64_t sequence_ = 0;
    std::array<Connection, slot_count> connections_{};
    std::array<std::uint64_t, slot_count> epochs_{};
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
