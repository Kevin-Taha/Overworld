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
    Tick now() override
    {
        if (expire_publication.load() && last_slot.load() && post_last_clocks.fetch_add(1) == 2) { time.store(1051); }
        return time.load();
    }
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
        if (index == 3) { last_slot.store(true); }
        Controller result;
        result.connection = Connection::Connected;
        result.connection_epoch = index + 1;
        return result;
    }
    void wait_entered() { check(WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0, "fake slot zero started"); }
    bool shutdown() override
    {
        shutdown_thread.store(GetCurrentThreadId());
        if (block_shutdown.load()) {
            SetEvent(entered);
            if (WaitForSingleObject(release, 2000) != WAIT_OBJECT_0) { expired.store(true); }
        }
        shutdown_finished.store(true);
        return !shutdown_failure.load();
    }
    void service_messages() override { services.fetch_add(1); }
    void unblock() { SetEvent(release); }
    HANDLE entered = nullptr, release = nullptr, returned = nullptr, rejected = nullptr;
    std::atomic<Tick> time{1000};
    std::atomic<bool> foreground{true}, block{true}, expired{false}, repeat{false}, revoke_last{false};
    std::atomic<bool> expire_publication{false}, last_slot{false};
    std::atomic<bool> block_shutdown{false}, shutdown_finished{false};
    std::atomic<bool> shutdown_failure{false};
    std::atomic<DWORD> shutdown_thread{0};
    std::atomic<unsigned> services{0};
    std::atomic<unsigned> post_last_clocks{0};
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
    check(backend->services.load() > 0, "worker services native messages even without sampling authorization");
    for (const auto& calls : backend->calls) { check(calls.load() == 0, "no slot is read without explicit authorization"); }
    reader.authorize(owned_identity());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    Snapshot snapshot;
    while (!(snapshot = reader.read()).sequence && std::chrono::steady_clock::now() < deadline) { Sleep(1); }
    check(reader.stop(), "fresh fake reader stops");
    check(backend->shutdown_finished.load() && backend->shutdown_thread.load() != GetCurrentThreadId(),
        "native teardown completes on the worker before successful stop");
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
void publication_expiry()
{
    auto backend = std::make_shared<Fake>();
    backend->block.store(false);
    backend->expire_publication.store(true);
    Reader reader(backend);
    check(reader.start(), "publication expiry fake starts");
    reader.authorize(owned_identity());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (backend->post_last_clocks.load() < 3 && std::chrono::steady_clock::now() < deadline) { Sleep(1); }
    check(reader.stop(), "publication expiry fake stops");
    check(backend->post_last_clocks.load() >= 3 && reader.publications() == 0,
        "lease expiring between final focus check and publication lock prevents publication");
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
void teardown_timeout()
{
    auto backend = std::make_shared<Fake>();
    backend->block_shutdown.store(true);
    {
        Reader reader(backend);
        check(reader.start(), "teardown fake starts");
        const auto begin = std::chrono::steady_clock::now();
        check(!reader.stop(), "blocked teardown is covered by bounded stop");
        check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1)
            && !reader.error().empty(), "teardown timeout is bounded and explicit");
        check(WaitForSingleObject(backend->entered, 1000) == WAIT_OBJECT_0, "worker owns native cleanup");
        check(!reader.start() && !reader.read().sequence, "timeout remains terminal");
    }
    check(!backend->shutdown_finished.load(), "UI destruction does not repeat native cleanup");
    backend->unblock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!backend->shutdown_finished.load() && std::chrono::steady_clock::now() < deadline) { Sleep(1); }
    check(backend->shutdown_finished.load() && !backend->expired.load(), "retained worker finishes late teardown");
    for (const auto& calls : backend->calls) { check(calls.load() == 0, "teardown does not sample controllers"); }
}
void teardown_failure()
{
    auto backend = std::make_shared<Fake>();
    backend->shutdown_failure.store(true);
    Reader reader(backend);
    check(reader.start() && !reader.stop() && !reader.error().empty(),
        "background metadata shutdown failure reaches explicit terminal harness error");
}
int main()
{
    try { fresh_batch(); revoked(false); revoked(true); expired_batch(); publication_gate(); publication_expiry(); stopped_timeout(); retained_lifetime(); teardown_timeout(); teardown_failure(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS fake per-slot revocation, focus loss, deadlines, bounded stop, and retained-state lifetime; no XInput backend linked.\n";
}
