#include "KeyboardShell.h"
#include "KeyboardPlacement.h"

#include <array>
#include <dwmapi.h>

namespace gamepad {
NativeKeyboardShell::~NativeKeyboardShell() { reset(); }
void NativeKeyboardShell::reset()
{
    if (process_) { CloseHandle(process_); }
    process_ = nullptr;
    window_ = nullptr;
    process_id_ = thread_id_ = 0;
    identified_ = shell_ = false;
}
KeyboardShellObservation NativeKeyboardShell::observe(HWND foreground)
{
    const auto root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    DWORD pid = 0;
    const auto tid = root ? GetWindowThreadProcessId(root, &pid) : 0;
    if (!root || !tid || !pid) { reset(); return {false, {}}; }
    if (root != window_ || pid != process_id_ || tid != thread_id_
        || (process_ && WaitForSingleObject(process_, 0) != WAIT_TIMEOUT)) {
        reset();
        window_ = root; process_id_ = pid; thread_id_ = tid;
    }
    if (!identified_) {
        process_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!process_) { return {false, {}}; }
        std::array<wchar_t, 32768> image{};
        DWORD length = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process_, 0, image.data(), &length)) { reset(); return {false, {}}; }
        const std::wstring_view path(image.data(), length);
        const auto separator = path.find_last_of(L"\\/");
        shell_ = keyboard_shell_image(path.substr(separator == path.npos ? 0 : separator + 1));
        identified_ = true;
    }
    KeyboardShellObservation result;
    if (shell_) {
        RECT bounds{};
        DWORD cloaked = 0;
        if (!IsWindowVisible(root) || IsIconic(root)
            || FAILED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked
            || FAILED(DwmGetWindowAttribute(root, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))
            || bounds.right <= bounds.left || bounds.bottom <= bounds.top) { return {false, {}}; }
        result.obstruction = bounds;
    }
    DWORD final_pid = 0;
    if (GetWindowThreadProcessId(root, &final_pid) != tid || final_pid != pid
        || GetAncestor(root, GA_ROOT) != root || GetForegroundWindow() != foreground
        || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) { return {false, {}}; }
    return result;
}
} // namespace gamepad
