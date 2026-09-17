#include "PracticeReader.h"

#include <atomic>
#include <mutex>
#include <new>

namespace gamepad::practice {
struct Reader::State {
    explicit State(std::shared_ptr<ReaderBackend> value) : backend(std::move(value)) {}
    ~State() { if (wake) { CloseHandle(wake); } }
    bool allowed(HWND target, std::uint64_t epoch, Tick expires)
    {
        return !stop.load() && generation.load() == epoch && window.load() == target
            && target && reader_lease_current(expires, backend->now()) && backend->focused(target)
            && !stop.load() && generation.load() == epoch
            && reader_lease_current(expires, backend->now());
    }
    std::shared_ptr<ReaderBackend> backend;
    std::atomic<HWND> window{nullptr};
    std::atomic<Tick> deadline{0};
    std::atomic<std::uint64_t> generation{0}, sequence{0}, publications{0};
    std::atomic<bool> stop{false}, failed{false};
    HANDLE wake = nullptr;
    std::mutex mutex;
    Snapshot latest;
};
Reader::Reader(std::shared_ptr<ReaderBackend> backend) : state_(std::make_shared<State>(std::move(backend))) {}
Reader::~Reader()
{
    stop();
    if (thread_) { CloseHandle(thread_); }
}
bool Reader::start()
{
    if (start_attempted_ || stop_attempted_ || !state_->backend) {
        error_ = L"The practice controller reader cannot be started in this state."; return false;
    }
    start_attempted_ = true;
    state_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state_->wake) { error_ = L"Could not create the practice reader wake event."; return false; }
    auto transfer = std::make_unique<std::shared_ptr<State>>(state_);
    thread_ = CreateThread(nullptr, 0, worker, transfer.get(), 0, nullptr);
    if (!thread_) { error_ = L"Could not start the practice controller reader."; return false; }
    transfer.release();
    return true;
}
void Reader::authorize(HWND window)
{
    if (state_->stop.load()) { return; }
    if (window) { state_->deadline.store(state_->backend->now() + reader_lease_ms); }
    if (state_->window.exchange(window) != window) {
        state_->generation.fetch_add(1);
        std::lock_guard lock(state_->mutex);
        state_->latest = {};
    }
}
Tick Reader::now() { return state_->backend->now(); }
Snapshot Reader::read()
{
    if (state_->stop.load()) { return {}; }
    if (state_->failed.load()) {
        Snapshot failed;
        failed.sequence = state_->sequence.fetch_add(1) + 1;
        failed.sampled_at = now();
        failed.slots[0].connection = Connection::Error;
        failed.slots[0].error = ERROR_GEN_FAILURE;
        return failed;
    }
    std::lock_guard lock(state_->mutex);
    return state_->latest;
}
bool Reader::stopped() const
{
    return !thread_ || WaitForSingleObject(thread_, 0) == WAIT_OBJECT_0;
}
std::uint64_t Reader::publications() const { return state_->publications.load(); }
bool Reader::stop()
{
    if (stop_attempted_) { return stop_result_; }
    stop_attempted_ = true;
    state_->stop.store(true);
    state_->window.store(nullptr);
    state_->generation.fetch_add(1);
    const bool signaled = !state_->wake || SetEvent(state_->wake) != FALSE;
    const auto wait = thread_ ? WaitForSingleObject(thread_, 250) : WAIT_OBJECT_0;
    stop_result_ = signaled && wait == WAIT_OBJECT_0 && !state_->failed.load();
    if (!stop_result_) {
        error_ = wait == WAIT_TIMEOUT
            ? L"The practice controller read did not stop within 250 ms. No further reads are authorized; this process will exit."
            : L"The practice controller reader failed to stop cleanly. No further reads are authorized; this process will exit.";
    }
    return stop_result_;
}
DWORD WINAPI Reader::worker(void* context)
{
    std::unique_ptr<std::shared_ptr<State>> transfer(static_cast<std::shared_ptr<State>*>(context));
    const auto state = std::move(*transfer);
    transfer.reset();
    while (!state->stop.load()) {
        const auto target = state->window.load();
        const auto epoch = state->generation.load();
        const auto expires = state->deadline.load();
        if (state->allowed(target, epoch, expires)) {
            Snapshot next;
            next.sequence = state->sequence.fetch_add(1) + 1;
            next.sampled_at = state->backend->now();
            bool complete = true;
            for (unsigned index = 0; index < slot_count; ++index) {
                if (!state->allowed(target, epoch, expires)) { complete = false; break; }
                next.slots[index] = state->backend->slot(index);
            }
            if (complete && state->allowed(target, epoch, expires)) {
                std::lock_guard lock(state->mutex);
                if (!state->stop.load() && state->generation.load() == epoch
                    && reader_lease_current(expires, state->backend->now())) {
                    state->latest = next;
                    state->publications.fetch_add(1);
                }
            }
        }
        if (state->stop.load()) { break; }
        const auto wait = WaitForSingleObject(state->wake, 16);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) { state->failed.store(true); break; }
    }
    return state->failed.load() ? 1 : 0;
}
} // namespace gamepad::practice
