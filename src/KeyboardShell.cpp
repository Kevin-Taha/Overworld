#include "KeyboardShell.h"

#include <dwmapi.h>

namespace gamepad {
NativeKeyboardShell::Root::~Root() { reset(); }
void NativeKeyboardShell::Root::reset()
{
    if (process) { CloseHandle(process); }
    process = nullptr;
    window = nullptr;
    process_id = thread_id = 0;
    identified = shell = false;
}
bool NativeKeyboardShell::Root::stable() const
{
    DWORD current_process = 0;
    return window && process && GetWindowThreadProcessId(window, &current_process) == thread_id
        && current_process == process_id && GetAncestor(window, GA_ROOT) == window
        && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}
std::optional<bool> NativeKeyboardShell::Root::identify(HWND candidate)
{
    DWORD pid = 0;
    const auto tid = candidate ? GetWindowThreadProcessId(candidate, &pid) : 0;
    if (!candidate || !tid || !pid) { reset(); return {}; }
    if (candidate != window || pid != process_id || tid != thread_id || (process && !stable())) {
        reset();
        window = candidate; process_id = pid; thread_id = tid;
    }
    if (!identified) {
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!process) { return {}; }
        std::array<wchar_t, 32768> image{};
        DWORD length = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process, 0, image.data(), &length)) { reset(); return {}; }
        const std::wstring_view path(image.data(), length);
        const auto separator = path.find_last_of(L"\\/");
        shell = keyboard_shell_image(path.substr(separator == path.npos ? 0 : separator + 1));
        identified = true;
    }
    return stable() ? std::optional<bool>{shell} : std::nullopt;
}
NativeKeyboardShell::~NativeKeyboardShell() { reset(); }
void NativeKeyboardShell::reset()
{
    foreground_.reset();
    for (auto& peer : peers_) { peer.reset(); }
}
KeyboardShellObservation NativeKeyboardShell::bounds(Root& root)
{
    if (!root.stable()) { return {false, {}}; }
    if (!IsWindowVisible(root.window) || IsIconic(root.window)) {
        return root.stable() ? KeyboardShellObservation{} : KeyboardShellObservation{false, {}};
    }
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(root.window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) { return {false, {}}; }
    if (cloaked) { return root.stable() ? KeyboardShellObservation{} : KeyboardShellObservation{false, {}}; }
    RECT frame{}, window{}, region{};
    if (FAILED(DwmGetWindowAttribute(root.window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))
        || !GetWindowRect(root.window, &window)) { return {false, {}}; }
    const auto kind = GetWindowRgnBox(root.window, &region);
    auto observation = keyboard_shell_region(frame, window, kind, region);
    RECT final_window{};
    if (!root.stable() || !GetWindowRect(root.window, &final_window) || !EqualRect(&window, &final_window)) { return {false, {}}; }
    return observation;
}
KeyboardShellObservation NativeKeyboardShell::observe(HWND foreground)
{
    const auto root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    const auto identified = foreground_.identify(root);
    if (!identified) { return {false, {}}; }
    if (!*identified) {
        for (auto& peer : peers_) { peer.reset(); }
        return GetForegroundWindow() == foreground ? KeyboardShellObservation{} : KeyboardShellObservation{false, {}};
    }
    MONITORINFO monitor{sizeof(monitor)};
    const auto target_monitor = MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(target_monitor, &monitor)) { return {false, {}}; }
    auto result = keyboard_shell_union({}, bounds(foreground_), monitor.rcWork);
    if (!result.available) { return result; }

    // The focused Search window can have no pixels while a separate Start root is visible.
    // FindWindowEx includes these immersive roots; EnumWindows need not enumerate them.
    HWND previous = nullptr;
    unsigned visited = 0, used = 0;
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const auto candidate = FindWindowExW(nullptr, previous, L"Windows.UI.Core.CoreWindow", nullptr);
        if (!candidate) {
            if (GetLastError() != ERROR_SUCCESS) { return {false, {}}; }
            break;
        }
        if (candidate == previous || ++visited > 64) { return {false, {}}; }
        previous = candidate;
        if (candidate == root || !IsWindowVisible(candidate) || IsIconic(candidate)
            || MonitorFromWindow(candidate, MONITOR_DEFAULTTONEAREST) != target_monitor) { continue; }
        if (used == peers_.size()) { return {false, {}}; }
        auto& peer = peers_[used++];
        const auto shell = peer.identify(candidate);
        if (!shell) { return {false, {}}; }
        if (*shell) {
            result = keyboard_shell_union(result, bounds(peer), monitor.rcWork);
            if (!result.available) { return result; }
        }
    }
    for (unsigned i = used; i < peers_.size(); ++i) { peers_[i].reset(); }
    if (!foreground_.stable() || GetForegroundWindow() != foreground || !result.obstruction) { return {false, {}}; }
    return result;
}
} // namespace gamepad
