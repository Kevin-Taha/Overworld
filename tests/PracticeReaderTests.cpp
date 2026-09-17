#include "PracticeReader.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace gamepad;
using namespace gamepad::practice;
namespace {
void check(bool value, const char* reason)
{
    if (!value) { throw std::runtime_error(reason); }
}
class Fake final : public ReaderBackend {
public:
    Fake()
    {
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        rejected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!entered || !release || !returned || !rejected) { throw std::runtime_error("fake events unavailable"); }
    }
    ~Fake() override { CloseHandle(entered); CloseHandle(release); CloseHandle(returned); CloseHandle(rejected); }
    Tick now() override { return time.load(); }
    bool focused(HWND) override
    {
        const bool value = foreground.load();
        if (!value) { SetEvent(rejected); }
        return value;
    }
    Controller slot(unsigned index) override
    {
        calls[index].fetch_add(1);
        if (index == 0 && (block.exchange(false) || repeat.load())) {
            SetEvent(entered);
            if (WaitForSingleObject(release, 2000) != WAIT_OBJECT_0) { expired.store(true); }
            ResetEvent(release);
            SetEvent(returned);
        }
        if (index == 3 && revoke_last.load()) { foreground.store(false); }
        Controller result;
        result.connection = Connection::Connected;
        result.connection_epoch = index + 1;
        return result;
    }
    void wait_entered() { check(WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0, "fake slot zero started"); }
    void unblock() { SetEvent(release); }
    HANDLE entered = nullptr, release = nullptr, returned = nullptr, rejected = nullptr;
    std::atomic<Tick> time{1000};
    std::atomic<bool> foreground{true}, block{true}, expired{false}, repeat{false}, revoke_last{false};
    std::array<std::atomic<unsigned>, slot_count> calls{};
};
HWND owned_identity() { return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)); }
void settle()
{
    // Only fake work runs: bound scheduling waits without opening any window or controller.
    Sleep(40);
}
void fresh_batch()
{
    auto backend = std::make_shared<Fake>();
    backend->block.store(false);
    Reader reader(backend);
    check(reader.start(), "fresh fake reader starts");
    settle();
    for (const auto& calls : backend->calls) { check(calls.load() == 0, "no slot is read without explicit authorization"); }
    reader.authorize(owned_identity());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    Snapshot snapshot;
    while (!(snapshot = reader.read()).sequence && std::chrono::steady_clock::now() < deadline) { Sleep(1); }
    check(reader.stop(), "fresh fake reader stops");
    check(snapshot.sequence != 0 && snapshot.sampled_at == 1000, "complete fresh batch is published with original timestamp");
    for (unsigned i = 0; i < slot_count; ++i) {
        check(snapshot.slots[i].connection == Connection::Connected && snapshot.slots[i].connection_epoch == i + 1,
            "all four authorized slots retain their own identity");
    }
    check(reader.read().sequence == 0, "terminal stop cannot expose a reusable snapshot");
}
void revoked(bool focus_loss)
{
    auto backend = std::make_shared<Fake>();
    Reader reader(backend);
    check(reader.start(), "fake reader starts");
    reader.authorize(owned_identity());
    backend->wait_entered();
    if (focus_loss) { backend->foreground.store(false); }
    else { reader.authorize(nullptr); }
    backend->unblock();
    settle();
    check(reader.stop(), "revoked fake reader stops");
    for (unsigned i = 1; i < slot_count; ++i) { check(backend->calls[i].load() == 0, "revocation after delayed slot zero prevents slots 1-3"); }
    check(reader.read().sequence == 0 && reader.publications() == 0, "revoked partial batch is never published");
    check(!backend->expired.load(), "fake block was explicitly released");
}
void expired_batch()
{
    auto backend = std::make_shared<Fake>();
    backend->repeat.store(true);
    Reader reader(backend);
    check(reader.start(), "renewal fake starts");
    reader.authorize(owned_identity());
    backend->wait_entered();
    backend->time.store(1051);
    reader.authorize(owned_identity());
    backend->unblock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (backend->calls[0].load() < 2 && std::chrono::steady_clock::now() < deadline) { Sleep(1); }
    reader.authorize(nullptr);
    backend->unblock();
    check(reader.stop(), "renewed fake stops");
    check(backend->calls[0].load() == 2, "new lease can start a new batch after old batch is discarded");
    for (unsigned i = 1; i < slot_count; ++i) { check(backend->calls[i].load() == 0, "renewal cannot revive an expired in-flight batch"); }
    check(reader.read().sequence == 0 && reader.publications() == 0, "expired batch is not published");
}
void publication_gate()
{
    auto backend = std::make_shared<Fake>();
    backend->block.store(false);
    backend->revoke_last.store(true);
    Reader reader(backend);
    check(reader.start(), "publication fake starts");
    reader.authorize(owned_identity());
    check(WaitForSingleObject(backend->rejected, 1000) == WAIT_OBJECT_0, "reader observes focus loss after last slot");
    check(reader.stop(), "publication fake stops");
    check(backend->calls[3].load() == 1 && reader.publications() == 0, "focus rechecked after final slot before publication");
}
void stopped_timeout()
{
    auto backend = std::make_shared<Fake>();
    Reader reader(backend);
    check(reader.start(), "timeout fake reader starts");
    reader.authorize(owned_identity());
    backend->wait_entered();
    const auto begin = std::chrono::steady_clock::now();
    check(!reader.stop(), "blocked native-call simulation reports failed bounded stop");
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    check(elapsed < std::chrono::seconds(1) && !reader.error().empty(), "stop is bounded and failure is explicit");
    reader.authorize(owned_identity());
    backend->unblock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.stopped() && std::chrono::steady_clock::now() < deadline) { Sleep(1); }
    check(reader.stopped(), "late in-flight completion terminates retained worker");
    for (unsigned i = 1; i < slot_count; ++i) { check(backend->calls[i].load() == 0, "terminal stop forbids all later slots"); }
    check(reader.read().sequence == 0 && reader.publications() == 0 && !reader.start(), "stopped reader cannot publish partial work or restart");
}
void retained_lifetime()
{
    auto backend = std::make_shared<Fake>();
    {
        Reader reader(backend);
        check(reader.start(), "retained-state fake starts");
        reader.authorize(owned_identity());
        backend->wait_entered();
    }
    backend->unblock();
    check(WaitForSingleObject(backend->returned, 1000) == WAIT_OBJECT_0, "backend survives host's bounded destructor");
    settle();
    for (unsigned i = 1; i < slot_count; ++i) { check(backend->calls[i].load() == 0, "destruction forbids new slots after retained call returns"); }
}
}
int main()
{
    try { fresh_batch(); revoked(false); revoked(true); expired_batch(); publication_gate(); stopped_timeout(); retained_lifetime(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS fake per-slot revocation, focus loss, deadlines, bounded stop, and retained-state lifetime; no XInput backend linked.\n";
}
