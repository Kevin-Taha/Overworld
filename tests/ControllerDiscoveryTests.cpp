#include "ControllerDiscovery.h"
#include "KeyboardDelivery.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace gamepad;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
struct QueryState {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE destroyed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<unsigned> calls{0};
    std::atomic<bool> expired{false};
    std::atomic<DWORD> error{0}, owner{0};
    unsigned block_at = 2;
    QueryState() { check(entered && release && destroyed, "fake query events available"); }
    ~QueryState() { CloseHandle(entered); CloseHandle(release); CloseHandle(destroyed); }
};
class Query final : public ControllerDiscoverySource {
public:
    explicit Query(std::shared_ptr<QueryState> state) : state_(std::move(state)) {}
    ~Query() override { state_->owner.store(GetCurrentThreadId()); SetEvent(state_->destroyed); }
    ControllerDevices query(const std::atomic<bool>&) override
    {
        if (++state_->calls >= state_->block_at) {
            SetEvent(state_->entered);
            if (WaitForSingleObject(state_->release, 3000) != WAIT_OBJECT_0) { state_->expired.store(true); }
        }
        ControllerDevices devices;
        devices.ids[0] = {1};
        devices.count = 1;
        devices.error = state_->error.load();
        return devices;
    }
private:
    std::shared_ptr<QueryState> state_;
};
void wait_ready(ControllerDiscovery& discovery)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (discovery.latest().error == ERROR_IO_PENDING && std::chrono::steady_clock::now() < until) { Sleep(1); }
    check(discovery.latest().error == 0, "initial inventory published");
}
class SampleBackend final : public ControllerBackend {
public:
    explicit SampleBackend(ControllerDiscovery& discovery) : discovery_(discovery) { pad.connection = Connection::Connected; }
    ControllerDevices discover() override { return discovery_.latest(); }
    std::uint32_t open(unsigned, const ControllerDeviceId&) override { return 0; }
    Controller sample(unsigned) override { ++reads; return pad; }
    void close(unsigned) override {}
    void describe(unsigned, Controller&) const override {}
    Controller pad;
    unsigned reads = 0;
private:
    ControllerDiscovery& discovery_;
};
class Text final : public TextBoundary {
public:
    Tick now = 1000;
    unsigned sent = 0;
    TextObservation observe_text(std::optional<TextKey>) override
    {
        TextObservation result;
        result.allowed = result.cleanup_allowed = result.stable = true;
        result.physical_held = false;
        result.target.windows = {{{10, 1, 2}, {10, 1, 2}, {11, 1, 2}}};
        result.target.context = 1;
        return result;
    }
    Tick text_now() const override { return now; }
    unsigned send_text(TextEvent) override { ++sent; return 1; }
};
void keyboard_continuity()
{
    auto state = std::make_shared<QueryState>();
    ControllerDiscovery discovery(std::make_unique<Query>(state));
    wait_ready(discovery);
    check(WaitForSingleObject(state->entered, 2000) == WAIT_OBJECT_0, "periodic discovery is deliberately blocked");
    SampleBackend source(discovery);
    ControllerDevicesReader reader(source);
    Text text;
    KeyboardDelivery keyboard(text);
    Snapshot snapshot;
    const auto tick = [&](Tick elapsed = 16) {
        text.now += elapsed;
        snapshot.sampled_at = text.now;
        ++snapshot.sequence;
        snapshot.slots[0] = reader.read(0, text.now);
        keyboard.step(snapshot, text.now, {true, {}});
    };
    tick(); tick();
    source.pad.buttons = 0x8000; tick();
    check(keyboard.visible(), "fresh mapped controller opens real keyboard model during blocked discovery");
    source.pad.buttons = 0; source.pad.left_y = 28000; tick();
    for (unsigned i = 0; i < 70; ++i) {
        source.pad.buttons = i % 2 ? 0 : 0x1000;
        tick();
        check(keyboard.accepts_typing(), "metadata scan cannot interrupt fresh controller typing");
    }
    check(text.sent > 0 && source.reads == 74 && !state->expired.load(), "fresh samples and fake delivery continue without waiting for metadata");
    const auto accepted = text.sent;
    tick(101);
    check(!keyboard.accepts_typing() && text.sent == accepted, "real stale controller gap still cancels without replay");
    SetEvent(state->release);
    check(discovery.stop(), "released metadata worker stops cleanly");
    check(state->owner.load() != GetCurrentThreadId(), "discovery resources destroyed on their owning worker");
}
void pending_and_failure()
{
    auto state = std::make_shared<QueryState>();
    state->block_at = 1;
    ControllerDiscovery discovery(std::make_unique<Query>(state));
    check(discovery.latest().error == ERROR_IO_PENDING, "unavailable first inventory is explicit, not an invented empty success");
    check(WaitForSingleObject(state->entered, 1000) == WAIT_OBJECT_0, "initial metadata query blocked");
    for (unsigned i = 0; i < 20; ++i) { check(discovery.latest().error == ERROR_IO_PENDING, "pending query remains nonblocking"); }
    state->error.store(ERROR_ACCESS_DENIED);
    SetEvent(state->release);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (discovery.latest().error == ERROR_IO_PENDING && std::chrono::steady_clock::now() < until) { Sleep(1); }
    check(discovery.latest().error == ERROR_ACCESS_DENIED, "metadata errors propagate unchanged");
    check(discovery.stop() && discovery.latest().error == ERROR_CANCELLED, "stop is terminal");
}
void retained_shutdown()
{
    auto state = std::make_shared<QueryState>();
    state->block_at = 1;
    {
        ControllerDiscovery discovery(std::make_unique<Query>(state));
        discovery.latest();
        check(WaitForSingleObject(state->entered, 1000) == WAIT_OBJECT_0, "shutdown fake query in flight");
        const auto before = std::chrono::steady_clock::now();
        check(!discovery.stop(), "uncancellable metadata query reports failed bounded stop");
        check(std::chrono::steady_clock::now() - before < std::chrono::seconds(1), "shutdown is bounded");
        check(discovery.latest().error == ERROR_CANCELLED && !discovery.stop(), "failed stop cannot resume or hide its error");
    }
    SetEvent(state->release);
    check(WaitForSingleObject(state->destroyed, 1000) == WAIT_OBJECT_0, "retained worker safely destroys query state after owner exits");
    check(state->calls == 1 && !state->expired.load(), "no later enumeration begins after terminal stop");
}
}
int main()
{
    try { keyboard_continuity(); pending_and_failure(); retained_shutdown(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS metadata isolation, fresh keyboard delivery, stale rejection, bounded terminal teardown; fake devices only.\n";
}
