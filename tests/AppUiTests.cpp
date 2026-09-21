#include "AppUi.h"
#include "Resources.h"
#include "RingPixels.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace overworld;
namespace {
void check(bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } }
struct Ui {
    AppUi appearance;
    SettingsPages pages;
    HWND window = nullptr;
    RECT resource_client{}, resource_outer{}, resource_units{0, 0, 4, 8};
    LOGFONTW resource_font{};
    bool initialized = false;
    ~Ui() { if (window) { DestroyWindow(window); } }
    static INT_PTR CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(window, DWLP_USER));
        if (message == WM_INITDIALOG) {
            ui = reinterpret_cast<Ui*>(lparam); ui->window = window;
            SetWindowLongPtrW(window, DWLP_USER, lparam);
            for (const auto label : {L"Smooth", L"Speed bands"}) {
                SendDlgItemMessageW(window, IDC_MOTION_PROFILE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            }
            for (const auto& choice : cursor_styles) {
                SendDlgItemMessageW(window, IDC_CURSOR_STYLE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label));
            }
            for (const auto label : {L"Face buttons (A/B/X/Y)", L"One-handed (stick flicks)"}) {
                SendDlgItemMessageW(window, IDC_KEYBOARD_INPUT_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            }
            for (const int id : {IDC_MOTION_PROFILE, IDC_CURSOR_STYLE, IDC_KEYBOARD_INPUT_MODE}) {
                SendDlgItemMessageW(window, id, CB_SETCURSEL, 0, 0);
            }
            for (int i = 0; i < 11; ++i) {
                constexpr std::array defaults{L"1100", L"25", L"960", L"20", L"24", L"23", L"27", L"28", L"10", L"35", L"100"};
                SetDlgItemTextW(window, IDC_SPEED + i, defaults[i]);
                SendDlgItemMessageW(window, IDC_SPIN_FIRST + i, UDM_SETBUDDY,
                    reinterpret_cast<WPARAM>(GetDlgItem(window, IDC_SPEED + i)), 0);
            }
            CheckDlgButton(window, IDC_AUTO_GAME_SWITCHING, BST_CHECKED);
            CheckDlgButton(window, IDC_WORD_SUGGESTIONS, BST_CHECKED);
            SetDlgItemTextW(window, IDC_VOICE_STATUS, L"Off. Enabling this option does not start the microphone.");
            SetDlgItemTextW(window, IDC_SETTINGS_RESULT, L"Changes stay in this window until you Save.");
            ui->initialized = GetClientRect(window, &ui->resource_client) && GetWindowRect(window, &ui->resource_outer)
                && MapDialogRect(window, &ui->resource_units)
                && GetObjectW(reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)),
                    sizeof(ui->resource_font), &ui->resource_font) == sizeof(ui->resource_font)
                && ui->pages.initialize(window);
            return TRUE;
        }
        if (!ui) { return FALSE; }
        if (message == WM_PAINT) { ui->appearance.paint(window, true); return TRUE; }
        if (message == WM_PRINTCLIENT) { ui->appearance.paint_to(window, reinterpret_cast<HDC>(wparam), true); return TRUE; }
        if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT
            || message == WM_CTLCOLORBTN || message == WM_CTLCOLORLISTBOX) {
            return reinterpret_cast<INT_PTR>(ui->appearance.control_color(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam)));
        }
        if (message == WM_NOTIFY) {
            const auto& header = *reinterpret_cast<NMHDR*>(lparam);
            LRESULT result{};
            if (ui->appearance.custom_draw(header, result)) { SetWindowLongPtrW(window, DWLP_MSGRESULT, result); return TRUE; }
            if (header.code == TCN_SELCHANGE && header.idFrom == IDC_SETTINGS_TABS) {
                ui->pages.select(window, TabCtrl_GetCurSel(header.hwndFrom)); return TRUE;
            }
        }
        if (message == WM_DRAWITEM) { return ui->appearance.draw_item(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); }
        if (message == WM_CLOSE) { DestroyWindow(window); return TRUE; }
        if (message == WM_NCDESTROY) { ui->window = nullptr; }
        return FALSE;
    }
};

void export_view(HWND window, AppUi& appearance, const std::filesystem::path& path, bool settings = true, UINT dpi = 0)
{
    RECT client{}; GetClientRect(window, &client);
    const int width = client.right, height = client.bottom;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    const auto dc = CreateCompatibleDC(nullptr); void* pixels = nullptr;
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    check(dc && bitmap && pixels, "frame allocation");
    const auto old = SelectObject(dc, bitmap);
    appearance.paint_to(window, dc, settings, dpi);
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) { continue; }
        RECT rect{}; GetWindowRect(child, &rect); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
        const int saved = SaveDC(dc);
        SetViewportOrgEx(dc, rect.left, rect.top, nullptr);
        IntersectClipRect(dc, 0, 0, rect.right - rect.left, rect.bottom - rect.top);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN | PRF_NONCLIENT);
        RestoreDC(dc, saved);
    }
    check(appearance.error().empty(), "rendering must not report a drawing failure");
    BITMAPFILEHEADER header{}; header.bfType = 0x4D42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + width * height * 4;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    output.write(static_cast<const char*>(pixels), static_cast<std::streamsize>(width) * height * 4);
    check(output.good(), "frame export");
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}

LRESULT CALLBACK dashboard_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* ui = reinterpret_cast<AppUi*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        ui = static_cast<AppUi*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    if (!ui) { return DefWindowProcW(window, message, wparam, lparam); }
    if (message == WM_PAINT) { ui->paint(window, false); return 0; }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
        return reinterpret_cast<LRESULT>(ui->control_color(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam)));
    }
    if (message == WM_NOTIFY) {
        LRESULT result{};
        if (ui->custom_draw(*reinterpret_cast<NMHDR*>(lparam), result)) { return result; }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void text_bounds(HWND control, UINT dpi, int expected_font = 0);

void dashboard_frames(const std::filesystem::path& output)
{
    AppUi appearance;
    WNDCLASSW type{}; type.lpfnWndProc = dashboard_proc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"Overworld.RenderOnlyDashboard";
    check(RegisterClassW(&type), "dashboard render class");
    for (const int dpi : {96, 144, 192}) {
        const auto window = CreateWindowExW(0, type.lpszClassName, L"Render-only dashboard", WS_POPUP | WS_CLIPCHILDREN,
            -30000, -30000, MulDiv(488,dpi,96), MulDiv(324,dpi,96), nullptr, nullptr, type.hInstance, &appearance);
        check(window != nullptr, "render-only dashboard");
        const auto font = CreateFontW(-MulDiv(16,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        const auto title = CreateFontW(-MulDiv(25,dpi,96),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        for (const auto& control : dashboard_controls()) {
            const auto child = CreateWindowExW(0,control.window_class,control.title,WS_CHILD|WS_VISIBLE|control.style,
                MulDiv(control.x,dpi,96),MulDiv(control.y,dpi,96),MulDiv(control.width,dpi,96),MulDiv(control.height,dpi,96),
                window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(control.id)),type.hInstance,nullptr);
            check(child != nullptr, "dashboard standard control");
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(
                control.id == AppTitle || control.id == Banner ? title : font),FALSE);
            if (control.id == ResumeAutomatic || control.id == RetryCleanup) { ShowWindow(child, SW_HIDE); }
            check(control.x >= 0 && control.y >= 0 && control.x+control.width<=488 && control.y+control.height<=324,
                "dashboard control containment");
        }
        for (bool release_tap : {false, true}) {
            const auto label = GetDlgItem(window, AppTitle);
            SetWindowTextW(label, dashboard_title(release_tap));
            SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(release_tap ? font : title), FALSE);
            for (int theme = 0; theme < 3; ++theme) {
                auto palette = app_palette(theme == 1);
                if (theme == 2) {
                    palette = {RGB(0,0,0), RGB(0,0,0), RGB(255,255,255), RGB(255,255,255),
                        RGB(255,255,255), RGB(255,255,0), RGB(0,0,0), false, true};
                }
                check(appearance.refresh(window, palette), "dashboard theme");
                text_bounds(label, static_cast<UINT>(dpi), release_tap ? 16 : 25);
                export_view(window,appearance,output/(L"dashboard-"+std::to_wstring(release_tap)+L"-"
                    +std::to_wstring(theme)+L"-"+std::to_wstring(dpi)+L".bmp"), false,static_cast<UINT>(dpi));
            }
        }
        DestroyWindow(window); DeleteObject(font); DeleteObject(title);
    }
    UnregisterClassW(type.lpszClassName,type.hInstance);
}

void text_bounds(HWND control, UINT dpi, int expected_font)
{
    wchar_t type[64]{}, text[512]{};
    GetClassNameW(control, type, static_cast<int>(std::size(type)));
    GetWindowTextW(control, text, static_cast<int>(std::size(text)));
    const auto style = GetWindowLongPtrW(control, GWL_STYLE);
    const int id = GetDlgCtrlID(control);
    if (lstrcmpiW(type, UPDOWN_CLASSW) == 0
        || (lstrcmpiW(type, L"Static") == 0 && (style & SS_TYPEMASK) == SS_OWNERDRAW)) { return; }
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    LOGFONTW description{};
    check(GetObjectW(font, sizeof(description), &description) == sizeof(description), "control font");
    check(description.lfHeight == -MulDiv(expected_font ? expected_font : id == IDC_SETTINGS_TITLE ? 26 : 14, static_cast<int>(dpi), 96),
        "DPI fitting must preserve readable font sizes");
    RECT available{}; GetClientRect(control, &available);
    UINT flags = DT_SINGLELINE | DT_NOPREFIX;
    if (lstrcmpiW(type, L"Static") == 0) {
        if ((style & SS_TYPEMASK) == SS_OWNERDRAW) { return; }
        flags = DT_WORDBREAK | DT_NOPREFIX;
    } else if (lstrcmpiW(type, L"Button") == 0) {
        const int inset = MulDiv(10, static_cast<int>(GetDpiForWindow(control)), 96);
        if ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX) {
            available.left += MulDiv(18, static_cast<int>(GetDpiForWindow(control)), 96) + inset;
        } else { InflateRect(&available, -inset, 0); }
        flags = DT_SINGLELINE;
    } else if (lstrcmpiW(type, L"Edit") == 0) {
        // These fields intentionally scroll long paths, lists and status messages.
        if (id == IDC_VOICE_MODEL_PATH || (style & ES_MULTILINE)) { return; }
        SendMessageW(control, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&available));
    } else if (lstrcmpiW(type, L"ComboBox") == 0) {
        const auto index = SendMessageW(control, CB_GETCURSEL, 0, 0);
        check(index != CB_ERR && SendMessageW(control, CB_GETLBTEXTLEN, index, 0) < static_cast<LRESULT>(std::size(text)),
            "selected combo label");
        SendMessageW(control, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
        available.left += 9; available.right -= 26;
    } else if (lstrcmpiW(type, WC_TABCONTROLW) == 0) {
        const auto dc = GetDC(control); check(dc != nullptr, "tab measurement DC");
        const auto old = SelectObject(dc, font);
        for (int i = 0; i < TabCtrl_GetItemCount(control); ++i) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = text; item.cchTextMax = static_cast<int>(std::size(text));
            RECT tab{}, extent{};
            check(TabCtrl_GetItem(control, i, &item) && TabCtrl_GetItemRect(control, i, &tab), "tab label and bounds");
            check(DrawTextW(dc, text, -1, &extent, DT_CALCRECT | DT_SINGLELINE) != 0, "tab text measurement");
            check(tab.left >= 0 && tab.top >= 0 && tab.right <= available.right && tab.bottom <= available.bottom
                && extent.right <= tab.right - tab.left && extent.bottom <= tab.bottom - tab.top, "tab text must fit");
        }
        SelectObject(dc, old); ReleaseDC(control, dc);
        return;
    } else { return; }
    if (!text[0]) { return; }
    const auto dc = GetDC(control); check(dc != nullptr, "text measurement DC");
    const auto old = SelectObject(dc, font);
    RECT extent{0, 0, available.right - available.left, 0};
    const auto measured = DrawTextW(dc, text, -1, &extent, flags | DT_CALCRECT);
    SelectObject(dc, old); ReleaseDC(control, dc);
    if (!measured || extent.right > available.right - available.left || extent.bottom > available.bottom - available.top) {
        std::cerr << "text bounds id " << id << " dpi " << dpi << " needs " << extent.right << 'x' << extent.bottom
            << " available " << available.right - available.left << 'x' << available.bottom - available.top << '\n';
        throw std::runtime_error("control text must fit without clipping or ellipsis");
    }
}

void combo_choices(HWND control, UINT dpi)
{
    const auto selected = SendMessageW(control, CB_GETCURSEL, 0, 0);
    const auto count = SendMessageW(control, CB_GETCOUNT, 0, 0);
    check(count > 0, "combo choices must exist");
    for (LRESULT index = 0; index < count; ++index) {
        SendMessageW(control, CB_SETCURSEL, index, 0);
        text_bounds(control, dpi);
    }
    SendMessageW(control, CB_SETCURSEL, selected, 0);
}

void geometry(Ui& ui, int page, UINT dpi)
{
    RECT client{}; GetClientRect(ui.window, &client);
    struct Box { int id; RECT rect; };
    std::vector<Box> visible;
    for (HWND child = GetWindow(ui.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const int id = GetDlgCtrlID(child);
        const auto style = GetWindowLongPtrW(child, GWL_STYLE);
        const auto owner = settings_control_page(id);
        check(((style & WS_VISIBLE) != 0) == (owner < 0 || owner == page), "only selected page may be visible");
        if (!(style & WS_VISIBLE)) { continue; }
        RECT rect{}; GetWindowRect(child, &rect); MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT*>(&rect), 2);
        if (rect.left < 0 || rect.top < 0 || rect.right > client.right || rect.bottom > client.bottom) {
            std::cerr << "control " << id << " dpi " << dpi << " bounds " << rect.left << ',' << rect.top << ','
                << rect.right << ',' << rect.bottom << " client " << client.right << 'x' << client.bottom << '\n';
        }
        check(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom,
            "visible control must fit client bounds");
        text_bounds(child, dpi);
        if (id >= IDC_SPIN_FIRST && id < IDC_SPIN_FIRST + 11) { continue; } // Native edit buddy deliberately shares its edge.
        for (const auto& box : visible) {
            RECT overlap{};
            if (IntersectRect(&overlap, &box.rect, &rect)) {
                std::cerr << "overlap " << id << " with " << box.id << '\n';
                throw std::runtime_error("visible controls must not overlap");
            }
        }
        visible.push_back({id, rect});
    }
    HWND first = GetNextDlgTabItem(ui.window, nullptr, FALSE);
    check(first != nullptr, "keyboard tab order exists");
    HWND current = first; unsigned steps = 0;
    std::vector<HWND> tab_order;
    do {
        check((GetWindowLongPtrW(current, GWL_STYLE) & WS_VISIBLE) != 0, "tab order must not visit hidden pages");
        const auto owner = settings_control_page(GetDlgCtrlID(current));
        check(owner < 0 || owner == page, "tab order stays on selected page");
        check(IsWindowEnabled(current) != FALSE, "tab order must not visit disabled controls");
        tab_order.push_back(current);
        current = GetNextDlgTabItem(ui.window, current, FALSE);
        check(++steps < 100, "tab order must cycle finitely");
    } while (current && current != first);
    for (HWND child = GetWindow(ui.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const auto style = GetWindowLongPtrW(child, GWL_STYLE);
        if ((style & (WS_VISIBLE | WS_TABSTOP)) == (WS_VISIBLE | WS_TABSTOP) && IsWindowEnabled(child)) {
            check(std::find(tab_order.begin(), tab_order.end(), child) != tab_order.end(),
                "every enabled visible tab stop must remain reachable");
        }
    }
    current = first;
    for (auto it = tab_order.rbegin(); it != tab_order.rend(); ++it) {
        current = GetNextDlgTabItem(ui.window, current, TRUE);
        check(current == *it, "reverse tab order must reach the same controls");
    }
    for (const int id : {IDC_DEFAULTS, IDC_APPLY, IDCANCEL}) {
        check((GetWindowLongPtrW(GetDlgItem(ui.window, id), GWL_STYLE) & WS_VISIBLE) && IsWindowEnabled(GetDlgItem(ui.window, id)),
            "footer actions must stay visible and enabled");
    }
}

void dpi_layout(Ui& ui, UINT dpi, SIZE logical)
{
    const int width = MulDiv(logical.cx, static_cast<int>(dpi), 96);
    const int height = MulDiv(logical.cy, static_cast<int>(dpi), 96);
    RECT expected{0, 0, width, height};
    check(AdjustWindowRectExForDpi(&expected, static_cast<DWORD>(GetWindowLongPtrW(ui.window, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(ui.window, GWL_EXSTYLE)), dpi), "DPI nonclient calculation");
    check(MulDiv(height, 96, static_cast<int>(dpi)) <= 620, "every DPI client must stay near600 DIP");
    check(MulDiv(expected.bottom - expected.top, 96, static_cast<int>(dpi)) <= 672,
        "every DPI frame must fit720 DIP display with48 DIP taskbar");
    for (const POINT origin : {POINT{0, 0}, POINT{-1920, -1080}, POINT{1280, 48}}) {
        const RECT work{origin.x, origin.y, origin.x + MulDiv(1280, static_cast<int>(dpi), 96),
            origin.y + MulDiv(672, static_cast<int>(dpi), 96)};
        for (const POINT offset : {POINT{-40, -40}, POINT{20, 20}, POINT{1240, 680}}) {
            const RECT suggested{work.left + MulDiv(offset.x, static_cast<int>(dpi), 96),
                work.top + MulDiv(offset.y, static_cast<int>(dpi), 96), 0, 0};
            RECT bounds{};
            check(ui.pages.window_bounds(ui.window, dpi, suggested, work, bounds), "compact work-area fit");
            check(bounds.left >= work.left && bounds.top >= work.top && bounds.right <= work.right && bounds.bottom <= work.bottom,
                "full Settings window must remain inside the work area");
            check(bounds.right - bounds.left == expected.right - expected.left
                && bounds.bottom - bounds.top == expected.bottom - expected.top, "work-area fitting must not shrink the client");
        }
    }
    RECT unchanged{1, 2, 3, 4}, too_small{0, 0, 100, 100}, suggested{};
    check(!ui.pages.window_bounds(ui.window, dpi, suggested, too_small, unchanged)
        && unchanged.left == 1 && unchanged.top == 2 && unchanged.right == 3 && unchanged.bottom == 4,
        "unusable work area must fail without returning clipped bounds");
    check(ui.pages.dpi_changed(ui.window, dpi), "DPI layout");
    // Synthetic font/layout DPI does not change this HWND's real monitor DPI.
    // Use its actual nonclient metrics for the hidden surface, separately from the target-DPI frame checks above.
    RECT actual{0, 0, width, height};
    check(AdjustWindowRectExForDpi(&actual, static_cast<DWORD>(GetWindowLongPtrW(ui.window, GWL_STYLE)),
        FALSE, static_cast<DWORD>(GetWindowLongPtrW(ui.window, GWL_EXSTYLE)), GetDpiForWindow(ui.window)), "native test frame");
    check(SetWindowPos(ui.window, nullptr, -30000, -30000, actual.right - actual.left, actual.bottom - actual.top,
        SWP_NOACTIVATE | SWP_NOZORDER), "hidden test surface size");
    RECT client{}; GetClientRect(ui.window, &client);
    check(client.right == width && client.bottom == height, "synthetic DPI client must have exact requested pixels");
    std::cout << "DPI " << dpi << " client " << width << 'x' << height << " target outer "
        << expected.right - expected.left << 'x' << expected.bottom - expected.top << '\n';
}

void voice_capabilities(Ui& ui, UINT dpi)
{
    check(settings_control_page(IDC_WORD_SUGGESTIONS) == 2, "word suggestions belongs only to Typing");
    for (const bool enabled : {false, true}) {
        CheckDlgButton(ui.window, IDC_WORD_SUGGESTIONS, enabled ? BST_CHECKED : BST_UNCHECKED);
        check(IsWindowEnabled(GetDlgItem(ui.window, IDC_WORD_SUGGESTIONS)), "word suggestions available without voice");
        geometry(ui, 2, dpi);
    }
    for (const bool supported : {false, true}) {
        for (const bool enabled : {false, true}) {
            CheckDlgButton(ui.window, IDC_VOICE_ENABLED, enabled ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemTextW(ui.window, IDC_VOICE_MODEL_PATH, L"C:\\Models\\tiny.en.bin");
            update_voice_settings_controls(ui.window, supported);
            check((IsWindowEnabled(GetDlgItem(ui.window, IDC_VOICE_ENABLED)) != FALSE) == supported,
                "unsupported build must disable voice opt-in");
            check((IsWindowEnabled(GetDlgItem(ui.window, IDC_VOICE_MODEL_PATH)) != FALSE) == (supported && enabled)
                && (IsWindowEnabled(GetDlgItem(ui.window, IDC_VOICE_BROWSE)) != FALSE) == (supported && enabled),
                "model editor follows actual capability and opt-in");
            check((IsDlgButtonChecked(ui.window, IDC_VOICE_ENABLED) == BST_CHECKED) == enabled,
                "unsupported capability must not erase saved authorization preference");
            geometry(ui, 2, dpi);
        }
    }
}

void reference_creation(SIZE expected)
{
    struct ThreadDpi {
        DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
        ~ThreadDpi() { if (previous) { SetThreadDpiAwarenessContext(previous); } }
    } context;
    check(context.previous != nullptr, "test-only 96-DPI thread context");
    Ui ui;
    check(CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
        Ui::proc, reinterpret_cast<LPARAM>(&ui)) != nullptr && ui.initialized, "96-DPI resource initialization");
    check(GetDpiForWindow(ui.window) == 96, "owned reference dialog must use 96 DPI");
    RECT client{}; GetClientRect(ui.window, &client);
    check(client.right == expected.cx && client.bottom == expected.cy,
        "normalized client must not depend on the resource creation DPI");
    std::vector<RECT> reference;
    for (const UINT dpi : {96u, 144u, 192u, 96u}) {
        dpi_layout(ui, dpi, expected);
        check(ui.appearance.refresh(ui.window, app_palette(false)), "reference appearance");
        for (int page = 0; page < 4; ++page) {
            ui.pages.select(ui.window, page);
            geometry(ui, page, dpi);
        }
        if (dpi != 96) { continue; }
        std::vector<RECT> current;
        for (HWND child = GetWindow(ui.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT rect{}; GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT*>(&rect), 2);
            current.push_back(rect);
        }
        if (reference.empty()) { reference = current; }
        else {
            check(reference.size() == current.size() && std::equal(reference.begin(), reference.end(), current.begin(),
                [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }),
                "repeated DPI transitions must not accumulate layout rounding");
        }
    }
    check(!(GetWindowLongPtrW(ui.window, GWL_STYLE) & WS_VISIBLE), "reference dialog must remain hidden");
    std::cout << "PASS 96-DPI resource creation and 96/144/192/96 roundtrip\n";
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        check(argc == 2, "usage: overworld_app_ui_tests owned-artifact-directory");
        const std::filesystem::path output(argv[1]); std::filesystem::create_directories(output);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_UPDOWN_CLASS | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        check(InitCommonControlsEx(&common), "common controls");
        const auto foreground = GetForegroundWindow();
        Ui ui;
        check(CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
            Ui::proc, reinterpret_cast<LPARAM>(&ui)) != nullptr, "create isolated settings resource");
        check(ui.initialized, "initialize tabs, fonts and frame during WM_INITDIALOG");
        const auto& resource_client = ui.resource_client;
        const auto& resource_outer = ui.resource_outer;
        const auto& resource_units = ui.resource_units;
        auto resource_font = ui.resource_font;
        std::cout << "Resource DPI " << GetDpiForWindow(ui.window) << " client "
            << resource_client.right << 'x' << resource_client.bottom << " dialog units "
            << resource_units.right << 'x' << resource_units.bottom << " font height " << resource_font.lfHeight
            << " outer " << resource_outer.right-resource_outer.left << 'x' << resource_outer.bottom-resource_outer.top << '\n';
        resource_font.lfHeight = MulDiv(resource_font.lfHeight, 96, static_cast<int>(GetDpiForWindow(ui.window)));
        const auto reference_font = CreateFontIndirectW(&resource_font);
        const auto reference_dc = GetDC(ui.window);
        check(reference_font && reference_dc, "reference font measurement");
        const auto old_font = SelectObject(reference_dc, reference_font);
        TEXTMETRICW metrics{}; SIZE alphabet{};
        check(GetTextMetricsW(reference_dc, &metrics) && GetTextExtentPoint32W(reference_dc,
            L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &alphabet), "reference dialog metrics");
        std::cout << "Reference 96 DPI dialog units " << (alphabet.cx / 26 + 1) / 2 << 'x' << metrics.tmHeight << '\n';
        SelectObject(reference_dc, old_font); ReleaseDC(ui.window, reference_dc); DeleteObject(reference_font);
        RECT original{}; GetClientRect(ui.window, &original);
        const auto base_dpi = GetDpiForWindow(ui.window);
        std::cout << "Initialized client " << original.right << 'x' << original.bottom << '\n';
        check(MulDiv(original.bottom, 96, static_cast<int>(base_dpi)) <= 620, "Settings client must stay near600 DIP");
        RECT outer{}; GetWindowRect(ui.window, &outer);
        check(MulDiv(outer.bottom - outer.top, 96, static_cast<int>(base_dpi)) <= 672,
            "Settings including frame must fit720 DIP display with48 DIP taskbar");
        MONITORINFO monitor{sizeof(monitor)};
        check(GetMonitorInfoW(MonitorFromWindow(ui.window, MONITOR_DEFAULTTONEAREST), &monitor)
            && outer.left >= monitor.rcWork.left && outer.top >= monitor.rcWork.top
            && outer.right <= monitor.rcWork.right && outer.bottom <= monitor.rcWork.bottom,
            "initial native frame must be placed inside its actual work area");
        unsigned cases = 0;
        const SIZE logical{MulDiv(original.right, 96, static_cast<int>(base_dpi)),
            MulDiv(original.bottom, 96, static_cast<int>(base_dpi))};
        for (const UINT dpi : {96u, 144u, 192u}) {
            dpi_layout(ui, dpi, logical);
            for (unsigned theme = 0; theme < 3; ++theme) {
                auto palette = app_palette(theme == 1);
                if (theme == 2) { palette = {0, 0, RGB(255,255,255), RGB(255,255,255), RGB(255,255,255),
                    RGB(255,255,0), 0, false, true}; }
                check(ui.appearance.refresh(ui.window, palette), "deterministic theme without OS writes");
                for (int page = 0; page < 4; ++page) {
                    ui.pages.select(ui.window, page);
                    geometry(ui, page, dpi);
                    if (page == 1) {
                        combo_choices(GetDlgItem(ui.window, IDC_MOTION_PROFILE), dpi);
                        combo_choices(GetDlgItem(ui.window, IDC_CURSOR_STYLE), dpi);
                    }
                    if (page == 2) { combo_choices(GetDlgItem(ui.window, IDC_KEYBOARD_INPUT_MODE), dpi); }
                    if (page == 2) { voice_capabilities(ui, dpi); }
                    export_view(ui.window, ui.appearance, output / (L"settings-" + std::to_wstring(theme) + L"-" + std::to_wstring(page)
                        + L"-" + std::to_wstring(dpi) + L".bmp"));
                    ++cases;
                }
            }
        }
        dashboard_frames(output);
        reference_creation(logical);
        check(!(GetWindowLongPtrW(ui.window, GWL_STYLE) & WS_VISIBLE), "render-only dialog must remain hidden");
        check(GetForegroundWindow() == foreground, "render-only harness must never take foreground");
        std::cout << "PASS " << cases << " native tab/theme/DPI layouts; text/control bounds; all combo labels; forward/reverse tab access; "
            "36 voice capability states; work-area fit; unchanged foreground\n";
        std::cout << "Settings 96-DIP-equivalent client height " << MulDiv(original.bottom, 96, static_cast<int>(base_dpi))
            << ", including frame " << MulDiv(outer.bottom-outer.top,96,static_cast<int>(base_dpi)) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
    return 0;
}
