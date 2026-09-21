#include "CursorRing.h"

#include <ShellScalingApi.h>
#include <cstring>
#include <new>

namespace overworld {
namespace {
constexpr wchar_t ring_class[] = L"Overworld.CursorRing";
}

CursorRing::~CursorRing()
{
    if (window_) { DestroyWindow(window_); }
    release_bitmap();
}

void CursorRing::release_bitmap()
{
    if (previous_bitmap_) { SelectObject(dc_, previous_bitmap_); previous_bitmap_ = nullptr; }
    if (bitmap_) { DeleteObject(bitmap_); bitmap_ = nullptr; }
    if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
    dpi_ = 0;
}

void CursorRing::hide()
{
    if (window_ && visible_) { ShowWindow(window_, SW_HIDE); }
    visible_ = false;
}

void CursorRing::reset()
{
    hide();
    error_.clear();
}

bool CursorRing::fail(std::wstring_view operation)
{
    const DWORD error = GetLastError();
    // Retain the last successfully published window until the coordinator restores native visibility.
    error_ = L"Cursor overlay unavailable: " + std::wstring{operation} + L" (Windows error " + std::to_wstring(error)
        + L"). Apply settings to retry after resolving any pointer restore failure.";
    return false;
}

bool CursorRing::present(int diameter, CursorStyle style)
{
    if (!error_.empty()) { return false; }
    POINT point{};
    if (!GetCursorPos(&point)) { return fail(L"cursor position"); }
    const auto monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    UINT x = 0, y = 0;
    const auto hr = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y);
    if (FAILED(hr)) { SetLastError(static_cast<DWORD>(hr)); return fail(L"monitor DPI"); }
    return show_at({point.x, point.y}, x, diameter, style);
}

bool CursorRing::show_at(live::Point hotspot, unsigned dpi, int diameter, CursorStyle style)
{
    if (!error_.empty()) { return false; }
    const auto geometry = cursor_geometry(hotspot, dpi, diameter, style);
    if (!geometry) { SetLastError(ERROR_INVALID_PARAMETER); return fail(L"unsupported geometry or DPI"); }
    if (!window_) {
        WNDCLASSEXW klass{sizeof(klass)};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = window_proc;
        klass.lpszClassName = ring_class;
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { return fail(L"register overlay"); }
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            ring_class, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, klass.hInstance, nullptr);
        if (!window_) { return fail(L"create overlay"); }
    }
    const bool changed = dpi_ != dpi || diameter_ != diameter || style_ != style;
    if (changed) {
        release_bitmap();
        std::vector<std::uint32_t> pixels;
        try {
            pixels = cursor_pixels(dpi, diameter, style);
        }
        catch (const std::bad_alloc&) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return fail(L"pixel allocation"); }
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) { return fail(L"create memory DC"); }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = geometry->side;
        info.bmiHeader.biHeight = -geometry->side;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap_) { return fail(L"create bitmap"); }
        previous_bitmap_ = SelectObject(dc_, bitmap_);
        if (!previous_bitmap_ || previous_bitmap_ == HGDI_ERROR) {
            previous_bitmap_ = nullptr;
            return fail(L"select bitmap");
        }
        memcpy(bits, pixels.data(), pixels.size() * sizeof(pixels[0]));
    }
    if (changed) {
        POINT position{geometry->left, geometry->top};
        POINT source{};
        SIZE size{geometry->side, geometry->side};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window_, nullptr, &position, &size, dc_, &source, 0, &blend, ULW_ALPHA)) {
            return fail(L"publish alpha bitmap");
        }
        dpi_ = dpi;
        diameter_ = diameter;
        style_ = style;
    }
    if (changed || !visible_ || position_ != hotspot) {
        if (!SetWindowPos(window_, HWND_TOPMOST, geometry->left, geometry->top, geometry->side, geometry->side,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER)) { return fail(L"position overlay"); }
        position_ = hotspot;
        visible_ = true;
    }
    return true;
}

LRESULT CALLBACK CursorRing::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_MOUSEACTIVATE) { return MA_NOACTIVATE; }
    if (message == WM_NCHITTEST) { return HTTRANSPARENT; }
    if (message == WM_DPICHANGED) { return 0; } // Next cursor update uses the hotspot's monitor, not the rectangle majority.
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace overworld
