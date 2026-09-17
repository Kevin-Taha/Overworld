#include "AppUi.h"
#include "Resources.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace gamepad;
namespace {
void check(bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } }
struct Ui {
    AppUi appearance;
    SettingsPages pages;
    HWND window = nullptr;
    ~Ui() { if (window) { DestroyWindow(window); } }
    static INT_PTR CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(window, DWLP_USER));
        if (message == WM_INITDIALOG) {
            ui = reinterpret_cast<Ui*>(lparam); ui->window = window;
            SetWindowLongPtrW(window, DWLP_USER, lparam);
            for (const int id : {IDC_MOTION_PROFILE, IDC_CURSOR_STYLE, IDC_KEYBOARD_INPUT_MODE}) {
                SendDlgItemMessageW(window, id, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(id == IDC_MOTION_PROFILE ? L"Smooth" : id == IDC_CURSOR_STYLE ? L"Modern pointer" : L"Face buttons (A/B/X/Y)"));
                SendDlgItemMessageW(window, id, CB_SETCURSEL, 0, 0);
            }
            for (int i = 0; i < 11; ++i) {
                constexpr std::array defaults{L"1100", L"25", L"960", L"20", L"24", L"23", L"27", L"28", L"10", L"35", L"100"};
                SetDlgItemTextW(window, IDC_SPEED + i, defaults[i]);
                SendDlgItemMessageW(window, IDC_SPIN_FIRST + i, UDM_SETBUDDY,
                    reinterpret_cast<WPARAM>(GetDlgItem(window, IDC_SPEED + i)), 0);
            }
            CheckDlgButton(window, IDC_AUTO_GAME_SWITCHING, BST_CHECKED);
            SetDlgItemTextW(window, IDC_VOICE_STATUS, L"Off. Enabling this option does not start the microphone.");
            SetDlgItemTextW(window, IDC_SETTINGS_RESULT, L"Changes stay in this window until you Save.");
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

void dashboard_frames(const std::filesystem::path& output)
{
    AppUi appearance;
    WNDCLASSW type{}; type.lpfnWndProc = dashboard_proc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"GamepadDesktop.RenderOnlyDashboard";
    check(RegisterClassW(&type), "dashboard render class");
    for (const int dpi : {96, 144}) {
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
        for (int theme=0;theme<2;++theme) {
            check(appearance.refresh(window,app_palette(theme==1)), "dashboard theme");
            export_view(window,appearance,output/(L"dashboard-"+std::to_wstring(theme)+L"-"+std::to_wstring(dpi)+L".bmp"),
                false,static_cast<UINT>(dpi));
        }
        DestroyWindow(window); DeleteObject(font); DeleteObject(title);
    }
    UnregisterClassW(type.lpszClassName,type.hInstance);
}

void geometry(Ui& ui, int page)
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
        if (id >= IDC_SPIN_FIRST && id < IDC_SPIN_FIRST + 11) { continue; } // Native edit buddy deliberately shares its edge.
        check(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom,
            "visible control must fit client bounds");
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
    do {
        check((GetWindowLongPtrW(current, GWL_STYLE) & WS_VISIBLE) != 0, "tab order must not visit hidden pages");
        const auto owner = settings_control_page(GetDlgCtrlID(current));
        check(owner < 0 || owner == page, "tab order stays on selected page");
        current = GetNextDlgTabItem(ui.window, current, FALSE);
        check(++steps < 100, "tab order must cycle finitely");
    } while (current && current != first);
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        check(argc == 2, "usage: gamepad_app_ui_tests owned-artifact-directory");
        const std::filesystem::path output(argv[1]); std::filesystem::create_directories(output);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_UPDOWN_CLASS | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        check(InitCommonControlsEx(&common), "common controls");
        const auto foreground = GetForegroundWindow();
        Ui ui;
        check(CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
            Ui::proc, reinterpret_cast<LPARAM>(&ui)) != nullptr, "create isolated settings resource");
        check(ui.pages.initialize(ui.window), "initialize tabs and fonts");
        RECT original{}; GetClientRect(ui.window, &original);
        const auto base_dpi = GetDpiForWindow(ui.window);
        check(MulDiv(original.bottom, 96, static_cast<int>(base_dpi)) <= 620, "Settings client must stay near600 DIP");
        RECT outer{}; GetWindowRect(ui.window, &outer);
        check(MulDiv(outer.bottom - outer.top, 96, static_cast<int>(base_dpi)) <= 672,
            "Settings including frame must fit720 DIP display with48 DIP taskbar");
        unsigned cases = 0;
        for (const UINT dpi : {96u, 144u, 192u}) {
            RECT frame{0, 0, MulDiv(original.right, static_cast<int>(dpi), static_cast<int>(base_dpi)),
                MulDiv(original.bottom, static_cast<int>(dpi), static_cast<int>(base_dpi))};
            AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(ui.window, GWL_STYLE)),
                FALSE, static_cast<DWORD>(GetWindowLongPtrW(ui.window, GWL_EXSTYLE)), dpi);
            SetWindowPos(ui.window, nullptr, -30000, -30000, frame.right - frame.left, frame.bottom - frame.top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            check(ui.pages.dpi_changed(ui.window, dpi), "DPI layout");
            for (unsigned theme = 0; theme < 3; ++theme) {
                auto palette = app_palette(theme == 1);
                if (theme == 2) { palette = {0, 0, RGB(255,255,255), RGB(255,255,255), RGB(255,255,255),
                    RGB(255,255,0), 0, false, true}; }
                check(ui.appearance.refresh(ui.window, palette), "deterministic theme without OS writes");
                for (int page = 0; page < 4; ++page) {
                    ui.pages.select(ui.window, page);
                    geometry(ui, page);
                    if (dpi != 192) {
                        export_view(ui.window, ui.appearance, output / (L"settings-" + std::to_wstring(theme) + L"-" + std::to_wstring(page)
                            + L"-" + std::to_wstring(dpi) + L".bmp"));
                    }
                    dashboard_frames(output);
                    ++cases;
                }
            }
        }
        ui.pages.select(ui.window, 2);
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
            }
        }
        check(GetForegroundWindow() == foreground, "render-only harness must never take foreground");
        std::cout << "PASS " << cases << " native tab/theme/DPI layouts; no overlapping visible controls; hidden-page tab exclusion; voice capability states; unchanged foreground\n";
        std::cout << "Settings 96-DIP-equivalent client height " << MulDiv(original.bottom, 96, static_cast<int>(base_dpi))
            << ", including frame " << MulDiv(outer.bottom-outer.top,96,static_cast<int>(base_dpi)) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
    return 0;
}
