#include "AppUi.h"
#include "Resources.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include <algorithm>

namespace gamepad {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::array<const wchar_t*, 4> pages{L"General", L"Pointer", L"Typing", L"Advanced"};
D2D1_COLOR_F color(COLORREF value)
{
    return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f);
}
bool is_class(HWND window, const wchar_t* expected)
{
    wchar_t name[64]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return lstrcmpiW(name, expected) == 0;
}
}

AppPalette app_palette(bool dark)
{
    return dark
        ? AppPalette{RGB(29, 33, 40), RGB(39, 45, 54), RGB(241, 244, 248), RGB(181, 192, 205),
            RGB(70, 82, 96), RGB(151, 198, 245), RGB(21, 39, 58), true, false}
        : AppPalette{RGB(246, 247, 249), RGB(255, 255, 255), RGB(36, 43, 53), RGB(87, 99, 114),
            RGB(215, 223, 232), RGB(37, 93, 148), RGB(255, 255, 255), false, false};
}

std::span<const DashboardControl> dashboard_controls()
{
    static constexpr DashboardControl controls[]{
        {AppTitle, L"STATIC", L"Gamepad Desktop", SS_LEFT, 24, 20, 440, 36},
        {Banner, L"STATIC", L"Ready", SS_LEFT, 36, 82, 408, 34},
        {Selection, L"STATIC", L"Controller: automatic", SS_LEFT, 36, 122, 408, 24},
        {LiveStatus, L"STATIC", L"Move a controller stick to use the desktop.", SS_LEFT, 36, 157, 408, 58},
        {ResumeAutomatic, L"BUTTON", L"&Desktop control", BS_PUSHBUTTON | WS_TABSTOP, 24, 234, 210, 32},
        {RetryCleanup, L"BUTTON", L"&Retry release", BS_PUSHBUTTON | WS_TABSTOP, 250, 234, 214, 32},
        {OpenSettings, L"BUTTON", L"&Settings", BS_PUSHBUTTON | WS_TABSTOP, 24, 282, 138, 32},
        {Diagnostics, L"BUTTON", L"&Details", BS_PUSHBUTTON | WS_TABSTOP, 176, 282, 138, 32},
        {Exit, L"BUTTON", L"E&xit", BS_PUSHBUTTON | WS_TABSTOP, 328, 282, 136, 32}};
    return controls;
}

int settings_control_page(int id)
{
    if (id == IDC_AUTO_GAME_SWITCHING || id == IDC_EXCLUSIONS || (id >= 410 && id <= 413)) { return 0; }
    if (id == IDC_MOTION_PROFILE || (id >= IDC_SPEED && id <= IDC_SCROLL)
        || id == IDC_DIAMETER || (id >= 320 && id <= 322) || id == 327
        || id == IDC_CURSOR_STYLE || id == IDC_CURSOR_PREVIEW
        || (id >= 420 && id <= 423) || id == 429 || id == 430) { return 1; }
    if (id == IDC_KEYBOARD_INPUT_MODE || (id >= 440 && id <= 444) || (id >= 360 && id <= 379)) { return 2; }
    if ((id >= IDC_LEFT_EXIT && id <= IDC_RIGHT_ENTER) || (id >= IDC_BAND_LOW && id <= IDC_BAND_HIGH)
        || (id >= 323 && id <= 326) || (id >= 328 && id <= 330) || (id >= 424 && id <= 428)
        || (id >= 450 && id <= 456) || id == IDC_NATIVE_KEYBOARD || id == IDC_NAVIGATION_FILTER
        || id == IDC_RESTORE_CURSOR || id == IDC_RELOAD) { return 3; }
    return -1;
}

void update_voice_settings_controls(HWND window, bool supported)
{
    const bool enabled = IsDlgButtonChecked(window, IDC_VOICE_ENABLED) == BST_CHECKED;
    EnableWindow(GetDlgItem(window, IDC_VOICE_ENABLED), supported);
    EnableWindow(GetDlgItem(window, IDC_VOICE_MODEL_PATH), supported && enabled);
    EnableWindow(GetDlgItem(window, IDC_VOICE_BROWSE), supported && enabled);
    SetDlgItemTextW(window, IDC_VOICE_STATUS, !supported
        ? L"Local voice is unavailable in this build."
        : enabled ? L"Only released tiny.en is accepted; other models are rejected before parsing."
        : L"Off. Enabling this option does not start the microphone.");
}

class AppUi::Resources {
public:
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
};

AppUi::AppUi() : resources_(std::make_unique<Resources>()), palette_(app_palette(false)) {}
AppUi::~AppUi()
{
    if (background_) { DeleteObject(background_); }
    if (surface_) { DeleteObject(surface_); }
}

bool AppUi::refresh(HWND window, std::optional<AppPalette> override_palette)
{
    AppPalette next;
    if (override_palette) { next = *override_palette; }
    else {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)) {
            error_ = L"App appearance could not read Windows high contrast.";
            return false;
        }
        DWORD light = 1, size = sizeof(light);
        const auto result = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND) {
            error_ = L"App appearance could not read Windows theme.";
            return false;
        }
        next = app_palette(light == 0);
        if (contrast.dwFlags & HCF_HIGHCONTRASTON) {
            next = {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_HIGHLIGHT),
                GetSysColor(COLOR_HIGHLIGHTTEXT), false, true};
        }
    }
    const auto background = CreateSolidBrush(next.background);
    const auto surface = CreateSolidBrush(next.surface);
    if (!background || !surface) {
        if (background) { DeleteObject(background); }
        if (surface) { DeleteObject(surface); }
        error_ = L"App appearance could not allocate its colors.";
        return false;
    }
    if (background_) { DeleteObject(background_); }
    if (surface_) { DeleteObject(surface_); }
    background_ = background;
    surface_ = surface;
    palette_ = next;
    error_.clear();
    // Documented Windows 11 attributes are optional on earlier Windows builds.
    const BOOL dark_caption = next.dark && !next.high_contrast;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_caption, sizeof(dark_caption));
    style_controls(window);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    return true;
}

void AppUi::style_controls(HWND parent)
{
    for (HWND window = GetWindow(parent, GW_CHILD); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (is_class(window, WC_TABCONTROLW)) {
            SetWindowTheme(window, L"", L"");
            SetWindowSubclass(window, tab_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        } else if (is_class(window, L"ComboBox") || is_class(window, L"Edit") || is_class(window, L"ListBox")) {
            SetWindowTheme(window, palette_.dark ? L"" : nullptr, palette_.dark ? L"" : nullptr);
            if (is_class(window, L"ComboBox")) {
                SetWindowSubclass(window, tab_proc, 1, reinterpret_cast<DWORD_PTR>(this));
            }
        } else if (is_class(window, UPDOWN_CLASSW)) {
            SetWindowSubclass(window, tab_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }
}

bool AppUi::rounded(HDC dc, const RECT& bounds, COLORREF fill, COLORREF border, float radius)
{
    auto& r = *resources_;
    HRESULT hr = S_OK;
    if (!r.factory) { hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, r.factory.GetAddressOf()); }
    if (SUCCEEDED(hr) && !r.target) {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        hr = r.factory->CreateDCRenderTarget(&properties, r.target.GetAddressOf());
        if (SUCCEEDED(hr)) { hr = r.target->CreateSolidColorBrush(color(fill), r.brush.GetAddressOf()); }
    }
    if (SUCCEEDED(hr)) { hr = r.target->BindDC(dc, &bounds); }
    if (FAILED(hr)) {
        r.brush.Reset(); r.target.Reset();
        error_ = L"App vector drawing resources are unavailable."; return false;
    }
    r.target->BeginDraw();
    r.target->SetTransform(D2D1::Matrix3x2F::Identity());
    const auto rect = D2D1::RoundedRect({0.5f, 0.5f, static_cast<float>(bounds.right - bounds.left) - 0.5f,
        static_cast<float>(bounds.bottom - bounds.top) - 0.5f}, radius, radius);
    r.brush->SetColor(color(fill));
    r.target->FillRoundedRectangle(rect, r.brush.Get());
    r.brush->SetColor(color(border));
    r.target->DrawRoundedRectangle(rect, r.brush.Get(), 1);
    hr = r.target->EndDraw();
    if (FAILED(hr)) {
        r.brush.Reset(); r.target.Reset();
        error_ = L"App vector drawing failed; native controls remain available.";
        return false;
    }
    return true;
}

void AppUi::paint(HWND window, bool settings)
{
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    paint_to(window, dc, settings);
    EndPaint(window, &paint);
}

void AppUi::paint_to(HWND window, HDC dc, bool settings, UINT render_dpi)
{
    RECT bounds{};
    GetClientRect(window, &bounds);
    FillRect(dc, &bounds, background_ ? background_ : GetSysColorBrush(COLOR_WINDOW));
    const UINT dpi = render_dpi ? render_dpi : GetDpiForWindow(window);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    if (!settings && bounds.right > scale(64)) {
        const int x = GetScrollPos(window, SB_HORZ), y = GetScrollPos(window, SB_VERT);
        const RECT card{scale(20) - x, scale(66) - y, bounds.right - scale(20) - x, scale(224) - y};
        rounded(dc, card, palette_.surface, palette_.border, static_cast<float>(scale(20)));
    }
    if (!error_.empty()) {
        SetTextColor(dc, palette_.text); SetBkMode(dc, TRANSPARENT);
        RECT notice{scale(20), 0, bounds.right-scale(20), scale(18)};
        DrawTextW(dc, error_.c_str(), -1, &notice, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

HBRUSH AppUi::control_color(HDC dc, HWND control)
{
    const bool editor = is_class(control, L"Edit") || is_class(control, L"ListBox") || is_class(control, L"ComboBox")
        || is_class(control, L"ComboLBox");
    const int id = GetDlgCtrlID(control);
    const bool status_card = !is_class(GetParent(control), L"#32770") && (id == Banner || id == Selection || id == LiveStatus);
    SetTextColor(dc, IsWindowEnabled(control) ? palette_.text : palette_.secondary);
    SetBkColor(dc, editor || status_card ? palette_.surface : palette_.background);
    SetBkMode(dc, editor ? OPAQUE : TRANSPARENT);
    return editor || status_card ? surface_ : background_;
}

bool AppUi::custom_draw(const NMHDR& header, LRESULT& result)
{
    if (header.code != NM_CUSTOMDRAW || !is_class(header.hwndFrom, L"Button")) { return false; }
    const auto& item = reinterpret_cast<const NMCUSTOMDRAW&>(header);
    if (item.dwDrawStage != CDDS_PREPAINT) { return false; }
    const auto style = GetWindowLongPtrW(header.hwndFrom, GWL_STYLE) & BS_TYPEMASK;
    if (style != BS_PUSHBUTTON && style != BS_DEFPUSHBUTTON && style != BS_AUTOCHECKBOX) { return false; }
    RECT bounds = item.rc;
    FillRect(item.hdc, &bounds, background_);
    const auto dpi = GetDpiForWindow(header.hwndFrom);
    const int inset = MulDiv(10, static_cast<int>(dpi), 96);
    const bool enabled = IsWindowEnabled(header.hwndFrom) != FALSE;
    const bool primary = style == BS_DEFPUSHBUTTON;
    const bool pressed = (item.uItemState & CDIS_SELECTED) != 0;
    COLORREF ink = enabled ? (primary ? palette_.accent_text : palette_.text) : palette_.secondary;
    if (style == BS_AUTOCHECKBOX) {
        const int side = MulDiv(18, static_cast<int>(dpi), 96);
        RECT box{bounds.left, bounds.top + (bounds.bottom - bounds.top - side) / 2,
            bounds.left + side, bounds.top + (bounds.bottom - bounds.top + side) / 2};
        const bool checked = SendMessageW(header.hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED;
        rounded(item.hdc, box, checked ? palette_.accent : palette_.surface, palette_.border, 4);
        if (checked) {
            SetTextColor(item.hdc, palette_.accent_text);
            SetBkMode(item.hdc, TRANSPARENT);
            DrawTextW(item.hdc, L"\u2713", 1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        bounds.left += side + inset;
    } else {
        rounded(item.hdc, bounds, primary ? palette_.accent : pressed ? palette_.background : palette_.surface,
            palette_.border, static_cast<float>(inset));
        InflateRect(&bounds, -inset, 0);
    }
    wchar_t title[256]{};
    GetWindowTextW(header.hwndFrom, title, static_cast<int>(std::size(title)));
    SetTextColor(item.hdc, ink);
    SetBkMode(item.hdc, TRANSPARENT);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(header.hwndFrom, WM_GETFONT, 0, 0));
    const auto old = SelectObject(item.hdc, font);
    const UINT flags = DT_SINGLELINE | DT_VCENTER | (style == BS_AUTOCHECKBOX ? DT_LEFT : DT_CENTER)
        | ((SendMessageW(header.hwndFrom, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    DrawTextW(item.hdc, title, -1, &bounds, flags);
    if (GetFocus() == header.hwndFrom && !(SendMessageW(header.hwndFrom, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT focus = item.rc; InflateRect(&focus, -3, -3); DrawFocusRect(item.hdc, &focus);
    }
    SelectObject(item.hdc, old);
    result = CDRF_SKIPDEFAULT;
    return true;
}

void AppUi::draw_tabs(HWND window, HDC dc)
{
    RECT client{}; GetClientRect(window, &client); FillRect(dc, &client, background_);
    const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    for (int index = 0; index < TabCtrl_GetItemCount(window); ++index) {
        RECT rect{}; TabCtrl_GetItemRect(window, index, &rect);
        const bool selected = index == TabCtrl_GetCurSel(window);
        if (selected) { rounded(dc, rect, palette_.surface, palette_.border, 8); }
        wchar_t name[64]{}; TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = name; item.cchTextMax = 64;
        TabCtrl_GetItem(window, index, &item);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, selected ? palette_.text : palette_.secondary);
        DrawTextW(dc, name, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        if (selected && GetFocus() == window) { InflateRect(&rect, -4, -4); DrawFocusRect(dc, &rect); }
    }
    SelectObject(dc, old);
}

void AppUi::draw_field(HWND window, HDC dc)
{
    RECT bounds{}; GetClientRect(window, &bounds); FillRect(dc, &bounds, surface_);
    const bool spinner = is_class(window, UPDOWN_CLASSW);
    rounded(dc, bounds, palette_.surface, palette_.border, 4);
    const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, IsWindowEnabled(window) ? palette_.text : palette_.secondary);
    if (spinner) {
        RECT upper = bounds, lower = bounds;
        upper.bottom = (bounds.top + bounds.bottom) / 2; lower.top = upper.bottom;
        DrawTextW(dc, L"\u2303", 1, &upper, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        DrawTextW(dc, L"\u2304", 1, &lower, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        wchar_t text[512]{};
        const auto index = SendMessageW(window, CB_GETCURSEL, 0, 0);
        if (index != CB_ERR) {
            const auto length = SendMessageW(window, CB_GETLBTEXTLEN, index, 0);
            if (length >= 0 && length < static_cast<LRESULT>(std::size(text))) {
                SendMessageW(window, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
            }
        }
        RECT label = bounds;
        label.left += 9; label.right -= 26;
        DrawTextW(dc, text, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT arrow = bounds; arrow.left = arrow.right - 24;
        DrawTextW(dc, L"\u2304", 1, &arrow, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        if (GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            InflateRect(&label, -2, -2); DrawFocusRect(dc, &label);
        }
    }
    SelectObject(dc, old);
}

bool AppUi::draw_item(const DRAWITEMSTRUCT& item)
{
    if (item.CtlType != ODT_TAB) { return false; }
    draw_tabs(item.hwndItem, item.hDC);
    return true;
}

LRESULT CALLBACK AppUi::tab_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data)
{
    auto* ui = reinterpret_cast<AppUi*>(data);
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        if (is_class(window, WC_TABCONTROLW)) { ui->draw_tabs(window, dc); }
        else { ui->draw_field(window, dc); }
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_PRINTCLIENT) {
        if (is_class(window, WC_TABCONTROLW)) { ui->draw_tabs(window, reinterpret_cast<HDC>(wparam)); }
        else { ui->draw_field(window, reinterpret_cast<HDC>(wparam)); }
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == BM_SETCHECK || message == CB_SETCURSEL) {
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        InvalidateRect(window, nullptr, FALSE); return result;
    }
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(window, tab_proc, 1); }
    return DefSubclassProc(window, message, wparam, lparam);
}

SettingsPages::~SettingsPages()
{
    if (font_) { DeleteObject(font_); }
    if (title_) { DeleteObject(title_); }
}

bool SettingsPages::initialize(HWND window)
{
    controls_.clear();
    if (!SetDialogDpiChangeBehavior(window, DDC_DISABLE_ALL, DDC_DISABLE_ALL)) { return false; }
    const auto dpi = GetDpiForWindow(window);
    RECT units{0, 0, 4, 8}, client{};
    LOGFONTW resource_font{};
    if (!dpi || !MapDialogRect(window, &units) || units.right <= 0 || units.bottom <= 0
        || !GetClientRect(window, &client)
        || GetObjectW(reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)),
            sizeof(resource_font), &resource_font) != sizeof(resource_font)) { return false; }
    // Dialog units round with the resource font, not proportionally with DPI.
    // Keep a measured 96-DPI layout instead of preserving the creation monitor's rounding.
    resource_font.lfHeight = MulDiv(resource_font.lfHeight, 96, static_cast<int>(dpi));
    resource_font.lfWidth = MulDiv(resource_font.lfWidth, 96, static_cast<int>(dpi));
    const auto reference_font = CreateFontIndirectW(&resource_font);
    const auto dc = GetDC(window);
    if (!reference_font || !dc) {
        if (reference_font) { DeleteObject(reference_font); }
        if (dc) { ReleaseDC(window, dc); }
        return false;
    }
    const auto old = SelectObject(dc, reference_font);
    if (!old || old == HGDI_ERROR) { ReleaseDC(window, dc); DeleteObject(reference_font); return false; }
    TEXTMETRICW metrics{}; SIZE alphabet{};
    const bool measured = GetTextMetricsW(dc, &metrics) && GetTextExtentPoint32W(dc,
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &alphabet);
    SelectObject(dc, old); ReleaseDC(window, dc); DeleteObject(reference_font);
    const int unit_x = (alphabet.cx / 26 + 1) / 2, unit_y = metrics.tmHeight;
    if (!measured || unit_x <= 0 || unit_y <= 0) { return false; }
    const auto normalize = [&](RECT rect) {
        return RECT{MulDiv(rect.left, unit_x, units.right), MulDiv(rect.top, unit_y, units.bottom),
            MulDiv(rect.right, unit_x, units.right), MulDiv(rect.bottom, unit_y, units.bottom)};
    };
    client = normalize(client);
    client_ = {client.right, client.bottom};
    for (HWND control = GetWindow(window, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT)) {
        RECT rect{};
        if (!GetWindowRect(control, &rect)) { return false; }
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
        controls_.push_back({control, GetDlgCtrlID(control), normalize(rect)});
    }
    const auto tabs = GetDlgItem(window, IDC_SETTINGS_TABS);
    TabCtrl_DeleteAllItems(tabs);
    for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
        TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(pages[i]);
        if (TabCtrl_InsertItem(tabs, i, &item) == -1) { return false; }
    }
    select(window, 0);
    return fit_window(window, dpi) && dpi_changed(window, dpi);
}

bool SettingsPages::window_bounds(HWND window, UINT dpi, const RECT& suggested,
    const RECT& work_area, RECT& bounds) const
{
    if (!dpi || client_.cx <= 0 || client_.cy <= 0) { return false; }
    RECT frame{0, 0, MulDiv(client_.cx, static_cast<int>(dpi), 96), MulDiv(client_.cy, static_cast<int>(dpi), 96)};
    if (!AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), dpi)) { return false; }
    const LONG width = frame.right - frame.left, height = frame.bottom - frame.top;
    if (width > work_area.right - work_area.left || height > work_area.bottom - work_area.top) { return false; }
    const LONG left = std::clamp(suggested.left, work_area.left, work_area.right - width);
    const LONG top = std::clamp(suggested.top, work_area.top, work_area.bottom - height);
    bounds = {left, top, left + width, top + height};
    return true;
}

bool SettingsPages::fit_window(HWND window, UINT dpi, const RECT* suggested)
{
    RECT position{}, bounds{};
    if (suggested) { position = *suggested; }
    else if (!GetWindowRect(window, &position)) { return false; }
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&position, MONITOR_DEFAULTTONEAREST), &monitor)
        || !window_bounds(window, dpi, position, monitor.rcWork, bounds)) { return false; }
    return SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
}

bool SettingsPages::dpi_changed(HWND window, UINT dpi)
{
    const auto font = CreateFontW(-MulDiv(14, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    const auto title = CreateFontW(-MulDiv(26, static_cast<int>(dpi), 96), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (!font || !title) {
        if (font) { DeleteObject(font); } if (title) { DeleteObject(title); } return false;
    }
    for (const auto& control : controls_) {
        const auto& rect = control.rect;
        const RECT scaled{MulDiv(rect.left, static_cast<int>(dpi), 96), MulDiv(rect.top, static_cast<int>(dpi), 96),
            MulDiv(rect.right, static_cast<int>(dpi), 96), MulDiv(rect.bottom, static_cast<int>(dpi), 96)};
        if (!MoveWindow(control.window, scaled.left, scaled.top, scaled.right - scaled.left, scaled.bottom - scaled.top, FALSE)) {
            DeleteObject(font); DeleteObject(title); return false;
        }
    }
    for (const auto& control : controls_) {
        SendMessageW(control.window, WM_SETFONT,
            reinterpret_cast<WPARAM>(control.id == IDC_SETTINGS_TITLE ? title : font), TRUE);
    }
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    if (font_) { DeleteObject(font_); } if (title_) { DeleteObject(title_); }
    font_ = font; title_ = title;
    RECT rect{}; GetClientRect(GetDlgItem(window, IDC_SETTINGS_TABS), &rect);
    TabCtrl_SetItemSize(GetDlgItem(window, IDC_SETTINGS_TABS), (rect.right - 8) / 4, MulDiv(30, static_cast<int>(dpi), 96));
    return true;
}

void SettingsPages::select(HWND window, int page)
{
    if (page < 0 || page >= static_cast<int>(pages.size())) { return; }
    const auto focus = GetFocus();
    if (focus && IsChild(window, focus)) {
        const auto previous = settings_control_page(GetDlgCtrlID(focus));
        if (previous >= 0 && previous != page) { SetFocus(GetDlgItem(window, IDC_SETTINGS_TABS)); }
    }
    selected_ = page;
    TabCtrl_SetCurSel(GetDlgItem(window, IDC_SETTINGS_TABS), page);
    for (const auto& control : controls_) {
        const auto owner = settings_control_page(control.id);
        ShowWindow(control.window, owner < 0 || owner == page ? SW_SHOWNA : SW_HIDE);
    }
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

bool SettingsPages::shortcut(HWND window, MSG& message)
{
    if (message.message != WM_KEYDOWN || message.wParam != VK_TAB || !(GetKeyState(VK_CONTROL) & 0x8000)
        || (message.hwnd != window && !IsChild(window, message.hwnd))) { return false; }
    select(window, (selected_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? 3 : 1)) % 4);
    return true;
}
} // namespace gamepad
