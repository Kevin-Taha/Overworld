#include "CursorRing.h"

#include <Windows.h>
#include <windowsx.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
constexpr wchar_t surface_class[] = L"GamepadDesktop.RingSmokeSurface";

void check(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

LRESULT CALLBACK surface_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

int surface(const wchar_t* token)
{
    WNDCLASSEXW klass{sizeof(klass)};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpfnWndProc = surface_proc;
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klass.lpszClassName = surface_class;
    if (!RegisterClassExW(&klass)) { return 2; }
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) { return 2; }
    const auto window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        surface_class, token, WS_POPUP, monitor.rcWork.left + 24, monitor.rcWork.top + 24, 240, 240,
        nullptr, nullptr, klass.hInstance, nullptr);
    if (!window) { return 2; }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    if (!SetTimer(window, 1, 15000, nullptr)) { DestroyWindow(window); return 2; }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_TIMER) { DestroyWindow(window); }
        else { DispatchMessageW(&message); }
    }
    return 0;
}

struct ChildSurface {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
    ~ChildSurface()
    {
        if (window) { PostMessageW(window, WM_CLOSE, 0, 0); }
        if (process.hProcess) {
            // Only our newly-created smoke child. It also has a 15-second self-expiry.
            WaitForSingleObject(process.hProcess, 2000);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
    }
};

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring_view{argv[1]} == L"--surface") { return surface(argv[2]); }
    if (argc != 2 || std::wstring_view{argv[1]} != L"--run") {
        std::cout << "Explicit non-injecting native smoke only: gamepad_ring_smoke --run\n"
                     "Creates a temporary nonactivating surface in its own child process and four cursor overlays.\n"
                     "No controller, live adapter, native cursor hiding/showing, SendInput, SetCursorPos, hooks, or user-app operations.\n";
        return 2;
    }
    try {
        const auto foreground = GetForegroundWindow();
        const auto capture = GetCapture();
        POINT cursor{};
        check(GetCursorPos(&cursor) != FALSE, "read initial cursor");
        ChildSurface child;
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        check(length && length < std::size(executable), "executable path");
        const auto token = L"Ring smoke " + std::to_wstring(GetCurrentProcessId());
        std::wstring command = L"\"" + std::wstring{executable} + L"\" --surface \"" + token + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &child.process) != FALSE, "create own surface process");
        // Console-subsystem test child may not support WaitForInputIdle; discover only its known HWND.
        for (int attempt = 0; attempt < 100; ++attempt) {
            child.window = FindWindowW(surface_class, token.c_str());
            if (child.window) { break; }
            Sleep(20);
        }
        check(child.window != nullptr, "own child surface ready");
        DWORD process_id = 0;
        GetWindowThreadProcessId(child.window, &process_id);
        check(process_id == child.process.dwProcessId, "surface belongs to smoke child, not another app");
        RECT bounds{};
        check(GetWindowRect(child.window, &bounds) != FALSE, "own surface bounds");
        const gamepad::live::Point hotspot{bounds.left + 120, bounds.top + 120};
        check(WindowFromPoint({hotspot.x, hotspot.y}) == child.window, "own surface hit before overlay");
        check(GetForegroundWindow() == foreground, "surface did not activate");
        const auto resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        {
            gamepad::CursorRing ring;
            for (unsigned dpi : {96u, 144u, 192u, 96u}) {
                for (int nominal : {24, 28, 32, 40}) {
                    for (const auto& choice : gamepad::cursor_styles) {
                        check(ring.show_at(hotspot, dpi, nominal, choice.style), "show/switch per-pixel-alpha cursor");
                        const auto cached_resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
                        for (int offset : {0, 2, 4, 0}) {
                            const gamepad::live::Point position{hotspot.x + offset, hotspot.y + offset};
                            check(ring.show_at(position, dpi, nominal, choice.style), "move overlay using cached pixels");
                            check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == cached_resources, "position update allocates no new GDI objects");
                            const auto style = GetWindowLongPtrW(ring.window(), GWL_EXSTYLE);
                            constexpr LONG_PTR required = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
                            check((style & required) == required && !(style & WS_EX_APPWINDOW), "layered transparent nonactivating tool window");
                            check(SendMessageW(ring.window(), WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "defensive no activate");
                            check(SendMessageW(ring.window(), WM_NCHITTEST, 0, MAKELPARAM(position.x, position.y)) == HTTRANSPARENT, "defensive hit test");
                            const auto geometry = *gamepad::cursor_geometry(position, dpi, nominal, choice.style);
                            RECT actual{};
                            check(GetWindowRect(ring.window(), &actual) && actual.left == geometry.left && actual.top == geometry.top
                                && actual.right - actual.left == geometry.side && actual.bottom - actual.top == geometry.side, "native geometry and hotspot");
                            const auto pixels = gamepad::cursor_pixels(dpi, nominal, choice.style);
                            bool tested_opaque = false;
                            for (int y = 0; y < geometry.side; ++y) {
                                for (int x = 0; x < geometry.side; ++x) {
                                    const auto pixel = pixels[static_cast<std::size_t>(y) * geometry.side + x];
                                    if ((pixel >> 24) == 255) {
                                        check(WindowFromPoint({geometry.left + x, geometry.top + y}) == child.window,
                                            "opaque cursor pixels must not mask underlying cross-process recipient");
                                        tested_opaque = true;
                                    }
                                }
                            }
                            check(tested_opaque && WindowFromPoint({position.x, position.y}) == child.window, "actual hotspot pass-through");
                            check(GetForegroundWindow() == foreground && GetCapture() == capture, "cursor never changed foreground/capture");
                        }
                    }
                }
                ring.hide();
                check(!IsWindowVisible(ring.window()), "hide immediate");
            }
            check(ring.show_at(hotspot, 96, 40, gamepad::CursorStyle::Ring), "failure retention fixture");
            check(!ring.show_at(hotspot, 0, 40) && IsWindowVisible(ring.window()) && !ring.error().empty(),
                "failed render retains last published visual until coordinator can restore native pointer");
            ring.hide();
            ring.reset();
            check(ring.error().empty(), "explicit reset");
        }
        check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == resources, "no retained GDI resources");
        POINT after{};
        check(GetCursorPos(&after) && cursor.x == after.x && cursor.y == after.y, "no cursor movement (user movement also invalidates this smoke)");
        check(GetForegroundWindow() == foreground, "foreground invariant through cleanup");
        std::cout << "PASS all four cursor styles at 24/28/32/40 DIPs, cached positioning, alpha publication,\n"
                     "cross-process WindowFromPoint at opaque pixels/hotspot,\n"
                     "foreground/capture/cursor invariance, immediate hide, DPI/resize/style switch, failed-render retention, and GDI cleanup.\n"
                     "Not a live-input trial or physical mixed-monitor validation.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (Win32 " << GetLastError() << ")\n";
        return 1;
    }
    return 0;
}
