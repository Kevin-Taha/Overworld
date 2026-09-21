#include "NativeNavigationFilter.h"
#include "XInputReader.h"

#include <atomic>
#include <new>

namespace overworld {
namespace {
constexpr std::uintptr_t own_output_tag = 0x47445031;
enum class Phase : unsigned { Starting, Observing, Draining, Off, Failed };
NavigationForeground foreground() noexcept
{
    const HWND window = GetForegroundWindow();
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    return {reinterpret_cast<std::uintptr_t>(window), process, thread};
}
}

struct NativeNavigationFilter::State {
    // All cross-thread fields are lock-free. A callback attempts one consistent read,
    // never spins waiting for the GUI publisher.
    NavigationPolicyMailbox policy;
    NavigationStopRequest stop;
    std::atomic<bool> emergency_pass{false};
    std::atomic<HHOOK> hook{nullptr};
    std::atomic<Phase> phase{Phase::Starting};
    std::atomic<DWORD> error{0};
    std::atomic<unsigned> pending{0};
    std::atomic<std::uint64_t> down{0}, up{0}, first{0}, quarantined{0}, abandoned{0};
    std::array<std::atomic<std::uint64_t>, 10> passed{};
    std::atomic<std::uint64_t> source_conflicts{0}, foreground_abandons{0}, drain_abandons{0};
    std::atomic<NavigationPass> last_pass{NavigationPass::None};
    std::atomic<NavigationGate> last_gate{NavigationGate::Unpublished};
    std::atomic<live::BlockReason> last_block{live::BlockReason::None};
    std::atomic<Tick> last_age{0}, last_at{0};
    std::atomic<std::uint64_t> detail_revision{0};
    std::atomic<bool> hook_removed{false};
    HANDLE wake = nullptr;
    NavigationFilter filter;
    ~State() { if (wake) { CloseHandle(wake); } }

    NavigationPolicy read() const noexcept
    {
        auto result = policy.read();
        if (stop.requested() || emergency_pass.load()) { result.enabled = false; }
        return result;
    }
    void counts() noexcept
    {
        const auto& counts = filter.counts();
        pending.store(filter.pending());
        down.store(counts.suppressed_down); up.store(counts.suppressed_up);
        first.store(counts.first_holds); quarantined.store(counts.quarantined); abandoned.store(counts.abandoned);
        for (unsigned i = 0; i < passed.size(); ++i) { passed[i].store(counts.passed[i]); }
        source_conflicts.store(counts.source_conflicts);
        foreground_abandons.store(counts.foreground_abandons); drain_abandons.store(counts.drain_abandons);
        detail_revision.fetch_add(1);
        last_pass.store(counts.last_pass); last_gate.store(counts.last_gate); last_block.store(counts.last_block);
        last_age.store(counts.last_age); last_at.store(counts.last_at);
        detail_revision.fetch_add(1);
    }
};
thread_local NativeNavigationFilter::State* NativeNavigationFilter::callback_ = nullptr;

LRESULT CALLBACK NativeNavigationFilter::hook_proc(int code, WPARAM message, LPARAM data)
{
    auto* state = callback_;
    if (code == HC_ACTION && data && state && !state->emergency_pass.load()
        && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP)) {
        const auto& input = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if (input.vkCode >= 0xc3 && input.vkCode <= 0xda) {
            const NavigationEvent event{input.vkCode, message == WM_KEYDOWN || message == WM_SYSKEYDOWN,
                (input.flags & LLKHF_INJECTED) != 0, input.dwExtraInfo == own_output_tag,
                input.scanCode, input.dwExtraInfo, input.flags & (LLKHF_EXTENDED | LLKHF_LOWER_IL_INJECTED)};
            const auto policy = state->read();
            const auto current_foreground = foreground();
            // Same clock/epoch as Host policy issuance, sampled after the snapshot.
            const auto now = monotonic_now();
            if (state->filter.event(event, policy, current_foreground, now)) { return 1; }
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

DWORD WINAPI NativeNavigationFilter::run(void* context)
{
    std::unique_ptr<std::shared_ptr<State>> transfer{static_cast<std::shared_ptr<State>*>(context)};
    auto state = *transfer;
    transfer.reset();
    callback_ = state.get();
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    if (state->stop.requested() || state->emergency_pass.load()) {
        state->hook_removed.store(true);
        state->phase.store(Phase::Off); callback_ = nullptr; return 0;
    }
    const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, hook_proc, GetModuleHandleW(nullptr), 0);
    state->hook.store(hook);
    if (!hook) {
        state->error.store(GetLastError());
        state->hook_removed.store(true);
        state->phase.store(Phase::Failed);
        callback_ = nullptr;
        return 1;
    }
    state->phase.store(Phase::Observing);
    std::optional<Tick> stopping;
    bool done = false;
    while (!done) {
        const auto policy = state->read();
        const auto current_foreground = foreground();
        const auto now = monotonic_now();
        if (state->stop.requested()) {
            if (!stopping) {
                stopping = now;
                state->phase.store(Phase::Draining);
            }
            if (state->filter.pending() == 0 || now - *stopping >= navigation_drain_ms) { break; }
        }
        state->filter.update(policy, current_foreground, now);
        state->counts();
        const DWORD wait = MsgWaitForMultipleObjectsEx(1, &state->wake, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) {
            state->error.store(GetLastError());
            state->emergency_pass.store(true);
            break;
        }
        if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) {
            state->error.store(ERROR_INVALID_FUNCTION);
            state->emergency_pass.store(true);
            break;
        }
        // Bound each pump pass so posted messages cannot starve stop/deadline checks.
        for (unsigned i = 0; i < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
            if (message.message == WM_QUIT) { done = true; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    state->emergency_pass.store(true);
    state->filter.abandon();
    state->counts();
    if (const auto installed = state->hook.exchange(nullptr)) {
        if (!UnhookWindowsHookEx(installed)) { state->error.store(GetLastError()); }
        else { state->hook_removed.store(true); }
    }
    state->phase.store(state->error.load() ? Phase::Failed : Phase::Off);
    callback_ = nullptr;
    return state->error.load() ? 1 : 0;
}

NativeNavigationFilter::~NativeNavigationFilter()
{
    shutdown();
    if (thread_) { CloseHandle(thread_); } // Worker retains its shared callback state on an OS shutdown timeout.
}
bool NativeNavigationFilter::reap()
{
    if (!thread_) { return true; }
    if (WaitForSingleObject(thread_, 0) != WAIT_OBJECT_0) { return false; }
    CloseHandle(thread_);
    thread_ = nullptr;
    return true;
}
void NativeNavigationFilter::set_enabled(bool enabled)
{
    enabled = shutdown_.can_enable(enabled);
    if (requested_ == enabled) { reap(); return; }
    requested_ = enabled;
    if (!enabled) {
        if (state_) { state_->stop.request(state_->abandoned.load()); }
        suspend();
        if (state_) {
            if (state_->wake && !SetEvent(state_->wake)) { start_error_ = GetLastError(); }
        }
        return;
    }
    if (!reap()) {
        start_error_ = ERROR_BUSY; // Explicit off/on retry after the short drain, never auto-restart in a loop.
        return;
    }
    start_error_ = 0;
    try { state_ = std::make_shared<State>(); }
    catch (const std::bad_alloc&) { start_error_ = ERROR_NOT_ENOUGH_MEMORY; return; }
    state_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state_->wake) { start_error_ = GetLastError(); return; }
    std::unique_ptr<std::shared_ptr<State>> transfer{new (std::nothrow) std::shared_ptr<State>(state_)};
    if (!transfer) { start_error_ = ERROR_NOT_ENOUGH_MEMORY; return; }
    thread_ = CreateThread(nullptr, 0, run, transfer.get(), 0, nullptr);
    if (!thread_) { start_error_ = GetLastError(); return; }
    transfer.release();
}
void NativeNavigationFilter::publish(NavigationPolicy policy)
{
    if (!state_) { return; }
    policy.enabled = shutdown_.can_enable(requested_ && policy.enabled);
    state_->policy.publish(policy);
}
void NativeNavigationFilter::suspend()
{
    NavigationPolicy policy;
    policy.issued = monotonic_now();
    policy.gate = NavigationGate::Suspended;
    publish(policy);
}
bool NativeNavigationFilter::shutdown()
{
    shutdown_.begin();
    set_enabled(false);
    const DWORD wait = thread_ ? WaitForSingleObject(thread_, 1000) : WAIT_OBJECT_0;
    if (thread_ && wait != WAIT_OBJECT_0) {
        state_->emergency_pass.store(true);
        start_error_ = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        if (const auto hook = state_->hook.exchange(nullptr)) {
            if (!UnhookWindowsHookEx(hook)) { start_error_ = GetLastError(); }
            else { state_->hook_removed.store(true); }
        }
        // The worker's shared state outlives this host even on exceptional OS shutdown.
        return false;
    }
    reap();
    // Retain cumulative warning counters in status(), but old abandonment alone is
    // not a failure of this stop. Newly abandoned holds and actual errors still are.
    return NavigationStopResult{thread_ == nullptr, !state_ || state_->hook_removed.load(),
        start_error_ ? start_error_ : state_ ? state_->error.load() : 0,
        state_ ? state_->pending.load() : 0,
        state_ ? state_->stop.abandoned_before() : 0,
        state_ ? state_->abandoned.load() : 0}.clean();
}
bool NativeNavigationFilter::faulted() const
{
    return start_error_ || (state_ && (state_->error.load() || state_->abandoned.load() || state_->quarantined.load()));
}
std::wstring NativeNavigationFilter::status() const
{
    if (!state_) { return start_error_ ? L"Navigation filter failed to start" : L"Navigation filter off"; }
    constexpr std::wstring_view phases[]{L"starting", L"observing (health not guaranteed)", L"draining", L"off", L"failed"};
    const auto policy = state_->read();
    const auto tick = monotonic_now();
    std::wstring result = L"Experimental navigation filter: " + std::wstring{phases[static_cast<unsigned>(state_->phase.load())]}
        + L"; policy=" + std::wstring{navigation_gate_text(policy.gate)}
        + L"; age=" + std::to_wstring(tick >= policy.issued ? tick - policy.issued : 0) + L"ms"
        + L"; suppressed " + std::to_wstring(state_->down.load()) + L"/" + std::to_wstring(state_->up.load())
        + L"; first holds passed " + std::to_wstring(state_->first.load())
        + L"; pending " + std::to_wstring(state_->pending.load())
        + L"; quarantined " + std::to_wstring(state_->quarantined.load())
        + L"; abandoned " + std::to_wstring(state_->abandoned.load())
        + L" (foreground=" + std::to_wstring(state_->foreground_abandons.load())
        + L", drain=" + std::to_wstring(state_->drain_abandons.load())
        + L", source=" + std::to_wstring(state_->source_conflicts.load()) + L")"
        + L"; error " + std::to_wstring(start_error_ ? start_error_ : state_->error.load());
    result += L"\r\nGAMEPAD passed-down totals:";
    for (unsigned i = 1; i < state_->passed.size(); ++i) {
        result += L" " + std::wstring{navigation_pass_text(static_cast<NavigationPass>(i))}
            + L"=" + std::to_wstring(state_->passed[i].load()) + L";";
    }
    const auto before = state_->detail_revision.load();
    const auto last_at = state_->last_at.load();
    const auto last_pass = state_->last_pass.load();
    const auto last_gate = state_->last_gate.load();
    const auto last_block = state_->last_block.load();
    const auto last_age = state_->last_age.load();
    if (!(before & 1) && before == state_->detail_revision.load() && last_at) {
        result += L"\r\nLast missed GAMEPAD down (retained): " + std::wstring{navigation_pass_text(last_pass)}
            + L"; gate=" + std::wstring{navigation_gate_text(last_gate)}
            + L"; block=" + std::wstring{live::block_reason_text(last_block)}
            + L"; policy age=" + std::to_wstring(last_age) + L"ms";
    }
    return result;
}
} // namespace overworld
