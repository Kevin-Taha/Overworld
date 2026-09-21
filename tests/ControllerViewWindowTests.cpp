#include "ControllerViewWindow.h"
#include "ControllerViewVisual.h"
#include "MonotonicClock.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace overworld;
namespace {
using Clock = std::chrono::steady_clock;
static_assert(noexcept(std::declval<const ControllerViewWindow&>().running()));
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
template<class Predicate>
void eventually(Predicate predicate, Tick duration, const char* message)
{
    const auto start = monotonic_now();
    do {
        if (predicate()) { return; }
        Sleep(5);
    } while (monotonic_now() - start < duration);
    throw std::runtime_error(message);
}
void dormant_lifecycle()
{
    DWORD before = 0, after = 0;
    const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    check(GetProcessHandleCount(GetCurrentProcess(), &before), "fixture process handles are measurable");
    for (unsigned repeat = 0; repeat < 16; ++repeat) {
        ControllerViewWindow window;
        check(!window.running() && !window.handle(), "default construction creates neither a native window nor a running worker");
        check(window.stop() && window.stop() && !window.publish({}), "non-started stop is inert and publication cannot start the view");
        for (unsigned poll = 0; poll < 1000; ++poll) {
            check(!window.running(), "nonblocking lifecycle polling does not start a dormant worker");
        }
    }
    check(GetProcessHandleCount(GetCurrentProcess(), &after) && before == after
        && GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == gdi
        && GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) == user,
        "construction, dormant polling and non-started destruction create no native handles or graphics resources");
}
HWND owned_window(const ControllerViewWindow& window)
{
    const HWND handle = window.handle();
    check(handle && IsWindow(handle), "hidden companion must own an actual native window");
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(handle, &process);
    check(process == GetCurrentProcessId() && thread != 0 && thread != GetCurrentThreadId(),
        "only this fixture's worker-thread HWND may be targeted");
    return handle;
}
void nonactivating(HWND handle, HWND foreground)
{
    check(!IsWindowVisible(handle), "synthetic companion must remain hidden throughout the fixture");
    check(GetForegroundWindow() == foreground && GetForegroundWindow() != handle, "hidden fixture cannot change foreground focus");
    GUITHREADINFO gui{sizeof(GUITHREADINFO)};
    const DWORD thread = GetWindowThreadProcessId(handle, nullptr);
    check(thread != 0 && GetGUIThreadInfo(thread, &gui), "owned GUI worker state is observable");
    check(!gui.hwndActive && !gui.hwndFocus, "worker must never own an active or focused window");
    const auto style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    check((style & WS_EX_NOACTIVATE) != 0 && (style & WS_EX_TOOLWINDOW) != 0 && (style & WS_EX_LAYERED) != 0
        && (style & WS_EX_TOPMOST) != 0 && (style & WS_EX_TRANSPARENT) == 0,
        "owned companion is a nonactivating topmost per-pixel layered window, not wholly click-through");
    const auto native_style = GetWindowLongPtrW(handle, GWL_STYLE);
    check((native_style & (WS_CAPTION | WS_BORDER | WS_DLGFRAME)) == 0 && (native_style & WS_POPUP) != 0
        && (native_style & WS_THICKFRAME) != 0, "borderless popup has no caption/border while retaining native resize capability");
    RECT outer{}, client{};
    POINT origin{};
    check(GetWindowRect(handle, &outer) && GetClientRect(handle, &client) && ClientToScreen(handle, &origin),
        "owned borderless geometry is readable");
    check(outer.right - outer.left == client.right && outer.bottom - outer.top == client.bottom
        && origin.x == outer.left && origin.y == outer.top, "all non-client pixels are removed, not merely hidden under a painted titlebar");
}
LRESULT send(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    DWORD_PTR result = 0;
    check(SendMessageTimeoutW(handle, message, wparam, lparam, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result) != 0,
        "owned native-message fixture must respond within 250ms");
    return static_cast<LRESULT>(result);
}
LPARAM screen_point(HWND handle, float x, float y)
{
    RECT client{};
    check(GetClientRect(handle, &client), "owned hit-test dimensions are available");
    const float scale = std::min(static_cast<float>(client.right) / controller_view_width,
        static_cast<float>(client.bottom) / controller_view_height);
    POINT point{static_cast<LONG>((static_cast<float>(client.right) - controller_view_width * scale) / 2 + x * scale),
        static_cast<LONG>((static_cast<float>(client.bottom) - controller_view_height * scale) / 2 + y * scale)};
    check(ClientToScreen(handle, &point), "only owned client points are converted for hit testing");
    check(point.x >= -32768 && point.x <= 32767 && point.y >= -32768 && point.y <= 32767,
        "owned test coordinates fit native signed hit-test fields");
    return MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y));
}
void hit_tests(HWND handle)
{
    RECT client{};
    check(GetClientRect(handle, &client), "owned alpha-reference dimensions are available");
    ControllerViewVisual reference;
    check(reference.render(client.right, client.bottom, {}, {}), "shared renderer supplies actual alpha for native hit fixtures");
    const auto frame = reference.frame();
    const float scale = std::min(static_cast<float>(client.right) / controller_view_width,
        static_cast<float>(client.bottom) / controller_view_height);
    const float left = (static_cast<float>(client.right) - controller_view_width * scale) / 2;
    const float top = (static_cast<float>(client.bottom) - controller_view_height * scale) / 2;
    struct Hit { float x, y; LRESULT expected; };
    constexpr std::array<Hit, 18> hits{{
        {10, 10, HTTRANSPARENT}, {91, 40, HTTRANSPARENT}, {160, 40, HTTRANSPARENT},
        {160, 205, HTTRANSPARENT}, {29, 178, HTTRANSPARENT},
        {88, 97, HTCLIENT}, {231, 80, HTCLIENT}, {91, 48, HTCLIENT}, {91, 33, HTCLIENT},
        {160, 85, HTCAPTION}, {70, 170, HTCAPTION},
        {34, 178, HTLEFT}, {286, 178, HTRIGHT}, {160, 60, HTTOP},
        {90, 57, HTTOPLEFT}, {230, 57, HTTOPRIGHT}, {49, 210, HTBOTTOMLEFT}, {271, 210, HTBOTTOMRIGHT}
    }};
    for (const auto& hit : hits) {
        const int x = static_cast<int>(left + hit.x * scale), y = static_cast<int>(top + hit.y * scale);
        const auto alpha = frame.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)
            + static_cast<std::size_t>(x)] >> 24;
        if (hit.expected == HTTRANSPARENT) {
            check(alpha == 0, "pass-through hit fixtures really land on clear pixels");
        } else {
            check(alpha >= 128, "native drag/resize/control fixtures land on visibly painted pixels, not invisible HWND edges");
            unsigned painted_neighbors = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto nearby = frame.pixels[static_cast<std::size_t>(y + dy) * static_cast<std::size_t>(frame.width)
                        + static_cast<std::size_t>(x + dx)] >> 24;
                    if (nearby >= 128) { ++painted_neighbors; }
                }
            }
            check(painted_neighbors >= 7, "actionable hit fixtures lie in a real painted patch rather than an antialiased fringe");
        }
        const auto actual = send(handle, WM_NCHITTEST, 0, screen_point(handle, hit.x, hit.y));
        if (actual != hit.expected) {
            std::cerr << "Hit at " << hit.x << ',' << hit.y << ": " << actual << ", expected " << hit.expected << ".\n";
            throw std::runtime_error("visible body drag/resize and transparent/control hit boundaries differ");
        }
    }
    check(send(handle, WM_MOUSEACTIVATE, 0, MAKELPARAM(HTCAPTION, WM_LBUTTONDOWN)) == MA_NOACTIVATE,
        "a body drag hit cannot activate the companion");
    // Like the title buffer, process-lifetime storage is safe if a bounded
    // message finishes after a timeout that terminates this fixture.
    static MINMAXINFO limits{};
    send(handle, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    const int dpi = static_cast<int>(GetDpiForWindow(handle));
    check(limits.ptMinTrackSize.x == MulDiv(controller_view_min_width, dpi, 96)
        && limits.ptMinTrackSize.y == MulDiv(controller_view_min_height, dpi, 96)
        && limits.ptMaxTrackSize.x == controller_view_max_width && limits.ptMaxTrackSize.y == controller_view_max_height,
        "borderless native resizing retains bounded client dimensions");
}
std::wstring title(HWND handle)
{
    // Process-lifetime storage remains valid even if a timed-out WM_GETTEXT
    // finishes late. A timeout terminates the fixture without reading/reusing it.
    static std::array<wchar_t, 512> buffer{};
    buffer.fill(L'\0');
    DWORD_PTR copied = 0;
    check(SendMessageTimeoutW(handle, WM_GETTEXT, buffer.size(), reinterpret_cast<LPARAM>(buffer.data()),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &copied) != 0, "owned accessible status must respond within 250ms");
    check(copied < buffer.size(), "owned status text is bounded");
    return std::wstring(buffer.data(), static_cast<std::size_t>(copied));
}
void await_title(ControllerViewWindow& window, std::wstring_view expected)
{
    eventually([&] {
        if (!window.error().empty()) { std::wcerr << window.error() << L'\n'; throw std::runtime_error("native worker reported failure"); }
        return title(owned_window(window)).find(expected) != std::wstring::npos;
    }, 1000, "owned accessible status did not reflect the synthetic sample");
}
ControllerViewFrame sample(std::uint64_t sequence = 1)
{
    ControllerViewFrame frame;
    frame.sequence = sequence;
    frame.sampled_at = monotonic_now();
    frame.state.signal = ControllerViewSignal::Ready;
    frame.state.route = ControllerViewRoute::Keyboard;
    frame.state.slot = 2;
    frame.state.buttons = standard_buttons;
    frame.state.left_trigger = 128;
    frame.state.right_trigger = 255;
    frame.state.left_x = -32768; frame.state.left_y = 32767;
    frame.state.right_x = 32767; frame.state.right_y = -32768;
    return frame;
}
void start(ControllerViewWindow& window, HWND foreground)
{
    const auto before = Clock::now();
    if (!window.start(GetModuleHandleW(nullptr), false)) {
        std::wcerr << window.error() << L'\n'; throw std::runtime_error("hidden native worker did not start");
    }
    check(window.running(), "successful hidden start publishes a running worker");
    check(Clock::now() - before < std::chrono::seconds(2), "hidden startup must honor its bounded readiness wait");
    const HWND handle = owned_window(window);
    nonactivating(handle, foreground);
    RECT client{};
    check(GetClientRect(handle, &client), "owned compact client geometry is available");
    const auto dpi = static_cast<int>(GetDpiForWindow(handle));
    check(client.right == MulDiv(controller_view_width, dpi, 96)
        && client.bottom == MulDiv(controller_view_height, dpi, 96), "default native client preserves the compact 320x240-DIP canvas");
    hit_tests(handle);
}
void stop(ControllerViewWindow& window)
{
    const auto before = Clock::now();
    if (!window.stop()) { std::wcerr << window.error() << L'\n'; throw std::runtime_error("hidden native worker did not stop"); }
    check(Clock::now() - before < std::chrono::milliseconds(500), "normal shutdown must stay within the bounded join plus scheduling allowance");
    check(!window.running() && !window.handle() && window.stop(),
        "successful stop completes worker teardown and repeated stop is idempotent");
}
void lifecycle(HWND foreground)
{
    ControllerViewWindow window;
    start(window, foreground);
    const HWND handle = owned_window(window);
    await_title(window, L"Waiting for a sample");
    check(window.publish(sample()), "synthetic held controls publish");
    await_title(window, L"Live sample");
    const auto ready = title(handle);
    check(ready.find(L"Raw observed") != std::wstring::npos && ready.find(L"Keyboard") != std::wstring::npos
        && ready.find(L"Slot 3") != std::wstring::npos, "native status distinguishes observation, route and selected slot");
    await_title(window, L"Stale sample - controls cleared");
    check(title(handle).find(L"Observed") != std::wstring::npos, "independent aging clears the presentation route");
    auto stale = sample(2);
    check(stale.sampled_at >= controller_view_sample_age_ms + 1, "synthetic clock has a valid old-sample fixture");
    stale.sampled_at -= controller_view_sample_age_ms + 1;
    check(window.publish(stale), "already-stale synthetic controls publish without reviving holds");
    await_title(window, L"Stale sample - controls cleared");
    nonactivating(handle, foreground);
    const DWORD active_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    check(PostMessageW(handle, WM_CLOSE, 0, 0), "only the owned hidden companion receives manual close");
    eventually([&] { return !window.running(); }, 1000, "manual close did not finish worker-owned teardown");
    check(!window.handle() && GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) + 2 <= active_gdi,
        "terminal running status is published after the HWND and retained alpha DIB/DC are released");
    check(window.error().empty() && window.publish(sample(3)), "intentional manual close is a benign publication no-op");
    check(window.start(GetModuleHandleW(nullptr), false) && !window.running() && !window.handle(),
        "repeated start cannot reopen a manually closed companion");
    stop(window);
    check(GetForegroundWindow() == foreground, "manual close and stop leave foreground focus unchanged");
}
void publishing_during_render(HWND foreground)
{
    ControllerViewWindow window;
    start(window, foreground);
    const HWND handle = owned_window(window);
    RECT original{};
    check(GetWindowRect(handle, &original), "owned initial frame bounds are available");
    const int width = original.right - original.left, height = original.bottom - original.top;
    const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    check(resources >= 2, "worker owns actual retained alpha DIB/DC resources");
    Sleep(60);
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == resources, "idle alpha presentation does not allocate GDI resources");
    std::chrono::nanoseconds maximum{};
    const auto begin = Clock::now();
    for (unsigned step = 0; step < 100; ++step) {
        // Async resize and posted paint exercise the actual worker concurrently;
        // neither request synthesizes user input or exposes a visible window.
        const int grow = (step % 2) != 0 ? 24 : 0;
        check(SetWindowPos(handle, nullptr, 0, 0, width + grow, height + grow,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS), "owned async resize request succeeds");
        check(PostMessageW(handle, WM_PAINT, 0, 0), "owned hidden paint request is queued");
        for (unsigned iteration = 0; iteration < 16; ++iteration) {
            auto frame = sample(100 + static_cast<std::uint64_t>(step) * 16 + iteration);
            if ((iteration % 2) != 0) { frame.state.buttons = 0; frame.state.left_x = frame.state.right_y = 0; }
            const auto at = Clock::now();
            check(window.publish(frame), "publication remains healthy during owned paint/resize");
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - at);
            if (elapsed > maximum) { maximum = elapsed; }
        }
        check(maximum < std::chrono::milliseconds(50), "publication must not wait on native painting or resize resources");
        check(Clock::now() - begin < std::chrono::seconds(5), "synthetic render/publication stress is bounded");
        Sleep(2);
    }
    await_title(window, L"Stale sample - controls cleared");
    check(window.error().empty(), "hidden native paints and resizes completed without renderer failure");
    nonactivating(handle, foreground);
    hit_tests(handle);
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == resources, "resizing alpha surfaces releases replaced DIB/DC resources");
    const int dpi = static_cast<int>(GetDpiForWindow(handle));
    const int minimum_width = MulDiv(controller_view_min_width, dpi, 96);
    const int minimum_height = MulDiv(controller_view_min_height, dpi, 96);
    check(SetWindowPos(handle, nullptr, 0, 0, minimum_width, minimum_height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS), "owned minimum-size request succeeds");
    eventually([&] {
        RECT client{};
        return GetClientRect(handle, &client) && client.right == minimum_width && client.bottom == minimum_height;
    }, 1000, "owned minimum-size request did not finish");
    hit_tests(handle);
    nonactivating(handle, foreground);
    std::cout << "Maximum publish during hidden paint/resize: "
        << std::chrono::duration_cast<std::chrono::microseconds>(maximum).count() << " us (1600 synthetic publications).\n";
    stop(window);
    check(GetForegroundWindow() == foreground, "active-worker shutdown leaves foreground focus unchanged");
}
void right_click_close(HWND foreground)
{
    for (const UINT message : {WM_RBUTTONUP, WM_NCRBUTTONUP}) {
        ControllerViewWindow window;
        start(window, foreground);
        const HWND handle = owned_window(window);
        const auto position = screen_point(handle, 160, 85);
        check(PostMessageW(handle, message, message == WM_NCRBUTTONUP ? HTCAPTION : 0, position),
            "only the owned hidden controller receives its on-demand right-click-close message");
        eventually([&] { return !window.running(); }, 1000, "right-click close did not finish worker-owned teardown");
        check(!window.handle(), "finished right-click close leaves no owned window");
        check(window.error().empty() && window.publish(sample()), "right-click close is intentional, not a rendering failure");
        stop(window);
        check(GetForegroundWindow() == foreground, "right-click-close handling does not require foreground ownership");
    }
}
void runtime_failure(HWND foreground)
{
    ControllerViewWindow window;
    start(window, foreground);
    const HWND handle = owned_window(window);
    const DWORD active_gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    // An invalid, hidden, owned surface exercises real renderer failure without
    // a fault hook, external target, device access, or unsafe native input.
    check(SetWindowPos(handle, nullptr, 0, 0, controller_view_max_width + 1, controller_view_height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | SWP_NOSENDCHANGING),
        "owned invalid-size fixture request succeeds");
    eventually([&] { return !window.running(); }, 1000, "runtime failure did not finish worker teardown");
    const auto error = window.error();
    check(!error.empty() && error.find(L"dimensions") != std::wstring::npos && !window.handle()
        && !window.publish(sample()), "a finished failed worker retains its error and rejects publication");
    const auto before = Clock::now();
    check(!window.stop() && !window.stop() && window.error() == error && !window.running(),
        "reaping a failed worker preserves the original error across repeated bounded stop");
    check(Clock::now() - before < std::chrono::milliseconds(500)
        && GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) + 2 <= active_gdi,
        "failed worker reaping is bounded and releases retained alpha resources");
    check(GetForegroundWindow() == foreground, "owned runtime failure never changes foreground focus");
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2 || std::wstring_view(argv[1]) != L"--render-only") {
        std::cerr << "Usage: overworld_controller_view_window_tests --render-only\n";
        return 2;
    }
    try {
        const HWND foreground = GetForegroundWindow();
        dormant_lifecycle();
        lifecycle(foreground);
        publishing_during_render(foreground);
        right_click_close(foreground);
        runtime_failure(foreground);
        std::cout << "Owned hidden layered controller window: no non-client pixels, transparent/body/control/resize hit boundaries, "
            "alpha-resource reuse, readiness, synthetic publication, aging, paint/resize, right-click/manual close, "
            "inert construction, post-teardown running status, retained runtime errors, bounded/repeated stop and no focus changes passed. "
            "No visible native drag/menu test, mapper, controller reads, "
            "native input, microphone or cursor-visibility changes.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Owned hidden controller window failed: " << error.what() << '\n';
        return 1;
    }
}
