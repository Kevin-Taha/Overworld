#include "ControllerViewWindow.h"
#include "ControllerViewVisual.h"
#include "MonotonicClock.h"

#include <process.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

namespace overworld {
namespace {
constexpr wchar_t window_class[] = L"Overworld.ControllerView";
constexpr wchar_t window_name[] = L"Overworld - controller view";
// WS_THICKFRAME supplies native resizing, while WM_NCCALCSIZE removes its
// non-client pixels. The entire window rectangle is our per-pixel-alpha client.
constexpr DWORD window_style = WS_POPUP | WS_THICKFRAME;
constexpr DWORD window_extended_style = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
constexpr UINT_PTR refresh_timer = 1;
constexpr DWORD ready_wait_ms = 1500, stop_wait_ms = 250;

std::wstring_view controller_view_signal_text(ControllerViewSignal signal)
{
    switch (signal) {
    case ControllerViewSignal::Waiting: return L"Waiting for a sample";
    case ControllerViewSignal::Ready: return L"Live sample";
    case ControllerViewSignal::Stale: return L"Stale sample - controls cleared";
    case ControllerViewSignal::Disconnected: return L"Disconnected - controls cleared";
    case ControllerViewSignal::ReadError: return L"Read error - controls cleared";
    case ControllerViewSignal::Ambiguous: return L"Multiple controllers - not merged";
    }
    return L"Unknown signal";
}
std::wstring_view controller_view_route_text(ControllerViewRoute route)
{
    switch (route) {
    case ControllerViewRoute::Observing: return L"Observed";
    case ControllerViewRoute::Desktop: return L"Desktop control";
    case ControllerViewRoute::Keyboard: return L"Keyboard";
    case ControllerViewRoute::Paused: return L"Paused";
    }
    return L"Unknown route";
}

class LayeredSurface {
public:
    ~LayeredSurface() { release(); }
    bool update(const ControllerViewVisualFrame& frame)
    {
        if (frame.width != width || frame.height != height || !dc) {
            release();
            dc = CreateCompatibleDC(nullptr);
            if (!dc) { return false; }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = frame.width;
            info.bmiHeader.biHeight = -frame.height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!bitmap || !pixels) { release(); return false; }
            previous = SelectObject(dc, bitmap);
            if (!previous || previous == HGDI_ERROR) { release(); return false; }
            width = frame.width; height = frame.height;
        }
        std::memcpy(pixels, frame.pixels.data(), frame.pixels.size_bytes());
        return true;
    }
    HDC context() const { return dc; }
private:
    void release()
    {
        if (dc && previous && previous != HGDI_ERROR) { SelectObject(dc, previous); }
        if (bitmap) { DeleteObject(bitmap); }
        if (dc) { DeleteDC(dc); }
        dc = nullptr; bitmap = nullptr; previous = nullptr; pixels = nullptr; width = height = 0;
    }
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* pixels = nullptr;
    int width = 0, height = 0;
};
}

class ControllerViewWindow::State {
public:
    ~State()
    {
        if (ready) { CloseHandle(ready); }
        if (stop_event) { CloseHandle(stop_event); }
    }
    void fail(std::wstring message)
    {
        {
            std::lock_guard lock(error_mutex);
            if (failure.empty()) { failure = std::move(message); }
        }
        failed.store(true, std::memory_order_release);
    }
    std::wstring error() const
    {
        std::lock_guard lock(error_mutex);
        return failure;
    }
    void request_stop()
    {
        stopping.store(true, std::memory_order_release);
        if (stop_event && !SetEvent(stop_event)) { fail(L"Controller view stop event could not be signaled."); }
    }
    void unexpected_failure() noexcept
    {
        failed.store(true, std::memory_order_release);
        stopping.store(true, std::memory_order_release);
        if (stop_event) { SetEvent(stop_event); }
    }

    class Worker {
    public:
        explicit Worker(std::shared_ptr<State> shared) : state(std::move(shared)) {}
        ~Worker() { if (window) { DestroyWindow(window); } }
        void fail(std::wstring message)
        {
            state->fail(std::move(message));
            if (window) { DestroyWindow(window); }
        }
        bool refresh()
        {
            if (presenting) { return true; }
            if (!window || state->failed.load(std::memory_order_acquire)) { return false; }
            {
                std::unique_lock lock(state->mailbox_mutex, std::try_to_lock);
                if (lock.owns_lock()) { latest = state->latest; }
            }
            const auto presented = controller_view_present(latest, monotonic_now());
            const auto canonical = presented.signal == ControllerViewSignal::Ready
                ? presented : controller_view_clear(presented, presented.signal);
            if (!status || status->signal != canonical.signal || status->route != canonical.route || status->slot != canonical.slot) {
                std::wstring title = std::wstring(window_name) + L" | Raw observed | "
                    + std::wstring(controller_view_route_text(canonical.route)) + L" | "
                    + (canonical.slot ? L"Slot " + std::to_wstring(*canonical.slot + 1) : std::wstring(L"No slot"))
                    + L" | " + std::wstring(controller_view_signal_text(canonical.signal));
                if (!SetWindowTextW(window, title.c_str())) { fail(L"Controller view accessible status could not be updated."); return false; }
                status = canonical;
            }
            RECT client{};
            if (!GetClientRect(window, &client)) { fail(L"Controller view client bounds could not be read."); return false; }
            if (IsIconic(window) || client.right == 0 || client.bottom == 0) { return true; }
            if (!visual.render(client.right, client.bottom, canonical, theme)) { fail(visual.error()); return false; }
            const auto frame = visual.frame();
            if (frame.rasterizations != presented_rasterizations) {
                if (!surface.update(frame)) { fail(L"Controller view alpha presentation resources could not be created."); return false; }
                SIZE size{frame.width, frame.height};
                POINT origin{};
                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                presenting = true;
                const BOOL updated = UpdateLayeredWindow(window, nullptr, nullptr, &size,
                    surface.context(), &origin, 0, &blend, ULW_ALPHA);
                presenting = false;
                if (!updated) { fail(L"Controller view per-pixel alpha presentation failed."); return false; }
                presented_rasterizations = frame.rasterizations;
            }
            return true;
        }
        void paint()
        {
            PAINTSTRUCT paint{};
            const HWND owned = window;
            HDC dc = BeginPaint(owned, &paint);
            bool success = dc != nullptr;
            if (dc && !EndPaint(owned, &paint)) { success = false; }
            if (!success) { fail(L"Controller view native paint validation failed."); }
        }
        LRESULT hit_test(LPARAM position) const
        {
            POINT cursor{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
            if (!ScreenToClient(window, &cursor)) { return HTNOWHERE; }
            const auto frame = visual.frame();
            if (cursor.x < 0 || cursor.y < 0 || cursor.x >= frame.width || cursor.y >= frame.height) { return HTTRANSPARENT; }
            const auto pixel = frame.pixels[static_cast<std::size_t>(cursor.y) * static_cast<std::size_t>(frame.width)
                + static_cast<std::size_t>(cursor.x)];
            if ((pixel >> 24) == 0) { return HTTRANSPARENT; }
            const float x = static_cast<float>(cursor.x), y = static_cast<float>(cursor.y);
            for (const auto& control : frame.controls) {
                if (x >= control.bounds.left && x < control.bounds.right
                    && y >= control.bounds.top && y < control.bounds.bottom) { return HTCLIENT; }
            }
            const auto body = frame.body_bounds;
            if (x < body.left || x > body.right || y < body.top || y > body.bottom) { return HTCLIENT; }
            // Resize bands live on painted body/grip pixels, not the fully
            // transparent outer HWND margins that cannot receive mouse hits.
            const float scale = std::min(static_cast<float>(frame.width) / controller_view_width,
                static_cast<float>(frame.height) / controller_view_height);
            const float edge = 8 * scale;
            const float third = (body.right - body.left) / 3;
            if (y <= body.top + edge) {
                return x < body.left + third ? HTTOPLEFT : x > body.right - third ? HTTOPRIGHT : HTTOP;
            }
            if (y >= body.bottom - edge) {
                return x < body.left + third ? HTBOTTOMLEFT : x > body.right - third ? HTBOTTOMRIGHT : HTBOTTOM;
            }
            if (x <= body.left + edge) { return HTLEFT; }
            if (x >= body.right - edge) { return HTRIGHT; }
            return HTCAPTION;
        }
        void track_limits(MINMAXINFO& limits) const
        {
            const UINT dpi = window ? GetDpiForWindow(window) : GetDpiForSystem();
            limits.ptMinTrackSize = {MulDiv(controller_view_min_width, static_cast<int>(dpi), 96),
                MulDiv(controller_view_min_height, static_cast<int>(dpi), 96)};
            limits.ptMaxTrackSize = {controller_view_max_width, controller_view_max_height};
        }
        LRESULT message(UINT message, WPARAM wparam, LPARAM lparam)
        {
            switch (message) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_NCCALCSIZE: return 0;
            case WM_NCPAINT: return 0;
            case WM_NCACTIVATE: return TRUE;
            case WM_NCHITTEST: return hit_test(lparam);
            case WM_RBUTTONDOWN:
            case WM_NCRBUTTONDOWN:
            case WM_CONTEXTMENU: return 0;
            case WM_ERASEBKGND: return 1;
            case WM_GETMINMAXINFO:
                track_limits(*reinterpret_cast<MINMAXINFO*>(lparam)); return 0;
            case WM_TIMER:
                if (wparam == refresh_timer) {
                    // Window timers also run in native move/size modal loops.
                    if (state->stopping.load(std::memory_order_acquire)) { DestroyWindow(window); }
                    else { refresh(); }
                    return 0;
                }
                break;
            case WM_SIZE:
                if (initialized && wparam != SIZE_MINIMIZED) { refresh(); }
                return 0;
            case WM_PAINT:
                if (initialized) {
                    // A delayed paint must age the original sample too, even
                    // when WM_PAINT is dispatched ahead of a due WM_TIMER.
                    if (refresh()) { paint(); }
                    return 0;
                }
                break;
            case WM_THEMECHANGED:
            case WM_SYSCOLORCHANGE:
            case WM_SETTINGCHANGE:
                if (initialized) {
                    const HRESULT result = read_keyboard_theme(theme);
                    if (FAILED(result)) { fail(L"Controller view system appearance could not be read."); }
                    else { refresh(); }
                }
                return 0;
            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                if (!SetWindowPos(window, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER)) { fail(L"Controller view DPI placement failed."); }
                return 0;
            }
            case WM_CLOSE:
            // A right-click closes only this companion. Unlike a native popup
            // menu, this needs neither foreground ownership nor a focus lease.
            case WM_RBUTTONUP:
            case WM_NCRBUTTONUP:
                DestroyWindow(window); return 0;
            case WM_DESTROY:
                KillTimer(window, refresh_timer);
                state->closed.store(true, std::memory_order_release);
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY: {
                const HWND owned = window;
                SetWindowLongPtrW(owned, GWLP_USERDATA, 0);
                state->window.store(nullptr, std::memory_order_release);
                window = nullptr;
                return DefWindowProcW(owned, message, wparam, lparam);
            }
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }
        static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
        {
            auto* worker = reinterpret_cast<Worker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                worker = static_cast<Worker*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
                worker->window = window;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(worker));
            }
            if (!worker) { return DefWindowProcW(window, message, wparam, lparam); }
            try {
                return worker->message(message, wparam, lparam);
            } catch (...) {
                worker->state->unexpected_failure();
                return 0;
            }
        }
        bool create()
        {
            if (FAILED(read_keyboard_theme(theme))) { fail(L"Controller view system appearance could not be read."); return false; }
            WNDCLASSEXW description{};
            description.cbSize = sizeof(description);
            description.hInstance = state->instance;
            description.lpfnWndProc = dispatch;
            description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            description.lpszClassName = window_class;
            if (!RegisterClassExW(&description) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                fail(L"Controller view native class registration failed."); return false;
            }
            const UINT dpi = GetDpiForSystem();
            const int width = MulDiv(controller_view_width, static_cast<int>(dpi), 96);
            const int height = MulDiv(controller_view_height, static_cast<int>(dpi), 96);
            if (!CreateWindowExW(window_extended_style, window_class, window_name, window_style,
                MulDiv(32, static_cast<int>(dpi), 96), MulDiv(64, static_cast<int>(dpi), 96), width, height,
                nullptr, nullptr, state->instance, this)) {
                fail(L"Controller view native window creation failed."); return false;
            }
            state->window.store(window, std::memory_order_release);
            initialized = true;
            if (!SetTimer(window, refresh_timer, 16, nullptr)) { fail(L"Controller view freshness timer could not be started."); return false; }
            if (!refresh()) { return false; }
            if (state->stopping.load(std::memory_order_acquire)) { return false; }
            if (state->show) {
                ShowWindow(window, SW_SHOWNOACTIVATE);
                if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
                    fail(L"Controller view nonactivating placement failed."); return false;
                }
            }
            return true;
        }
        void run()
        {
            if (!create()) { return; }
            state->started.store(true, std::memory_order_release);
            if (!SetEvent(state->ready)) { fail(L"Controller view readiness could not be signaled."); return; }
            bool quit = false;
            while (!quit && !state->stopping.load(std::memory_order_acquire)) {
                const DWORD result = MsgWaitForMultipleObjectsEx(1, &state->stop_event, INFINITE,
                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (result == WAIT_OBJECT_0) { break; }
                if (result != WAIT_OBJECT_0 + 1) { fail(L"Controller view message wait failed."); break; }
                MSG message{};
                // A bounded batch preserves stop checks even with a busy queue.
                for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                    if (message.message == WM_QUIT) { quit = true; break; }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                    if (state->stopping.load(std::memory_order_acquire)) { break; }
                }
            }
        }
        std::shared_ptr<State> state;
        ControllerViewVisual visual;
        LayeredSurface surface;
        ControllerViewFrame latest{};
        KeyboardTheme theme{};
        std::optional<ControllerViewState> status;
        HWND window = nullptr;
        std::uint64_t presented_rasterizations = 0;
        bool initialized = false, presenting = false;
    };

    static unsigned __stdcall entry(void* context) noexcept
    {
        std::unique_ptr<std::shared_ptr<State>> transfer(static_cast<std::shared_ptr<State>*>(context));
        auto state = std::move(*transfer);
        transfer.reset();
        try {
            Worker worker(state);
            worker.run();
        } catch (...) {
            state->unexpected_failure();
        }
        // Also unblocks start when creation or a resource allocation failed.
        if (!SetEvent(state->ready)) { state->unexpected_failure(); }
        // Worker destruction above includes its HWND, alpha DIB/DC, renderer,
        // WIC/D2D interfaces and COM apartment. Only then may the Host reap it.
        state->running.store(false, std::memory_order_release);
        return 0;
    }
    HINSTANCE instance = nullptr;
    bool show = true;
    HANDLE ready = nullptr, stop_event = nullptr;
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> failed{false}, stopping{false}, started{false}, closed{false}, running{false};
    std::mutex mailbox_mutex;
    ControllerViewFrame latest{};
    mutable std::mutex error_mutex;
    std::wstring failure;
};

ControllerViewWindow::ControllerViewWindow() = default;
ControllerViewWindow::~ControllerViewWindow()
{
    stop();
    // Closing a thread handle does not destroy its shared worker-owned state.
    if (worker_) { CloseHandle(worker_); }
}
bool ControllerViewWindow::start(HINSTANCE instance, bool show)
{
    // Closing the companion is intentional and never implicitly reopens it.
    if (state_) { return state_->started.load(std::memory_order_acquire) && !state_->failed.load(std::memory_order_acquire); }
    state_ = std::make_shared<State>();
    state_->instance = instance;
    state_->show = show;
    state_->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state_->ready || !state_->stop_event) { state_->fail(L"Controller view lifecycle events could not be created."); return false; }
    auto transfer = std::make_unique<std::shared_ptr<State>>(state_);
    state_->running.store(true, std::memory_order_release);
    const auto thread = _beginthreadex(nullptr, 0, State::entry, transfer.get(), 0, nullptr);
    if (!thread) {
        state_->fail(L"Controller view worker could not be started.");
        state_->running.store(false, std::memory_order_release);
        return false;
    }
    transfer.release();
    worker_ = reinterpret_cast<HANDLE>(thread);
    const DWORD result = WaitForSingleObject(state_->ready, ready_wait_ms);
    if (result != WAIT_OBJECT_0) {
        state_->fail(result == WAIT_TIMEOUT ? L"Controller view startup timed out." : L"Controller view readiness wait failed.");
        stop(); return false;
    }
    if (!state_->started.load(std::memory_order_acquire) || state_->failed.load(std::memory_order_acquire)) {
        if (!state_->failed.load(std::memory_order_acquire)) { state_->fail(L"Controller view did not become ready."); }
        stop(); return false;
    }
    return true;
}
bool ControllerViewWindow::publish(const ControllerViewFrame& frame)
{
    if (!state_ || state_->failed.load(std::memory_order_acquire)) { return false; }
    if (state_->closed.load(std::memory_order_acquire) || state_->stopping.load(std::memory_order_acquire)) { return true; }
    std::unique_lock lock(state_->mailbox_mutex, std::try_to_lock);
    if (lock.owns_lock()) { state_->latest = frame; }
    return true;
}
bool ControllerViewWindow::stop()
{
    if (!state_) { return true; }
    if (!worker_) { return !state_->failed.load(std::memory_order_acquire); }
    state_->request_stop();
    const DWORD result = WaitForSingleObject(worker_, stop_wait_ms);
    if (result != WAIT_OBJECT_0) {
        state_->fail(result == WAIT_TIMEOUT ? L"Controller view shutdown timed out; worker-owned resources are retained."
            : L"Controller view shutdown wait failed; worker-owned resources are retained.");
        return false;
    }
    CloseHandle(worker_); worker_ = nullptr;
    return !state_->failed.load(std::memory_order_acquire);
}
std::wstring ControllerViewWindow::error() const
{
    if (!state_) { return L"Controller view has not been started."; }
    const auto message = state_->error();
    if (!message.empty()) { return message; }
    return state_->failed.load(std::memory_order_acquire) ? L"Controller view worker failed unexpectedly." : std::wstring{};
}
HWND ControllerViewWindow::handle() const
{
    return state_ ? state_->window.load(std::memory_order_acquire) : nullptr;
}
bool ControllerViewWindow::running() const noexcept
{
    if (!state_) { return false; }
    if (state_->running.load(std::memory_order_acquire)) { return true; }
    if (!worker_) { return false; }
    // The fast path above stays atomic-only until resource teardown completes.
    // Also fence CRT/thread exit, so Host-side destruction never joins live work.
    const DWORD result = WaitForSingleObject(worker_, 0);
    if (result == WAIT_TIMEOUT) { return true; }
    if (result != WAIT_OBJECT_0) { state_->unexpected_failure(); }
    return false;
}
} // namespace overworld
