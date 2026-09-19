#include "ControllerDiscovery.h"

#include <mutex>

namespace gamepad {
namespace {
ControllerDevices failure(DWORD error)
{
    ControllerDevices value;
    value.error = error;
    return value;
}
}
struct ControllerDiscovery::State {
    explicit State(std::unique_ptr<ControllerDiscoverySource> input) : source(std::move(input)) {}
    ~State() { if (wake) { CloseHandle(wake); } }
    std::unique_ptr<ControllerDiscoverySource> source;
    std::atomic<bool> stop{false};
    HANDLE wake = nullptr;
    std::mutex mutex;
    ControllerDevices latest = failure(ERROR_IO_PENDING);
};
ControllerDiscovery::ControllerDiscovery(std::unique_ptr<ControllerDiscoverySource> source)
    : state_(std::make_shared<State>(std::move(source))) {}
ControllerDiscovery::~ControllerDiscovery()
{
    stop();
    if (thread_) { CloseHandle(thread_); }
}
ControllerDevices ControllerDiscovery::latest()
{
    if (stopped_) { return failure(ERROR_CANCELLED); }
    if (!started_) {
        if (!state_->source) { started_ = true; return state_->latest = failure(ERROR_INVALID_PARAMETER); }
        auto transfer = std::make_unique<std::shared_ptr<State>>(state_);
        state_->wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        started_ = true;
        if (!state_->wake) { return state_->latest = failure(GetLastError()); }
        thread_ = CreateThread(nullptr, 0, worker, transfer.get(), 0, nullptr);
        if (!thread_) { return state_->latest = failure(GetLastError()); }
        transfer.release();
    }
    std::lock_guard lock(state_->mutex);
    return state_->latest;
}
bool ControllerDiscovery::stop()
{
    if (stopped_) { return stop_result_; }
    stopped_ = true;
    state_->stop.store(true);
    const bool signaled = !state_->wake || SetEvent(state_->wake) != FALSE;
    const auto result = thread_ ? WaitForSingleObject(thread_, 200) : WAIT_OBJECT_0;
    stop_result_ = signaled && result == WAIT_OBJECT_0;
    return stop_result_;
}
DWORD WINAPI ControllerDiscovery::worker(void* context)
{
    std::unique_ptr<std::shared_ptr<State>> transfer(static_cast<std::shared_ptr<State>*>(context));
    const auto state = std::move(*transfer);
    transfer.reset();
    while (!state->stop.load()) {
        const auto devices = state->source->query(state->stop);
        {
            std::lock_guard lock(state->mutex);
            if (!state->stop.load()) { state->latest = devices; }
        }
        if (state->stop.load()) { break; }
        const auto wait = WaitForSingleObject(state->wake, 1000);
        if (wait == WAIT_OBJECT_0) { break; }
        if (wait != WAIT_TIMEOUT) {
            std::lock_guard lock(state->mutex);
            state->latest = failure(GetLastError());
            break;
        }
    }
    state->source.reset();
    return 0;
}
} // namespace gamepad
