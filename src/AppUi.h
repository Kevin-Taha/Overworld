#pragma once

#include <Windows.h>
#include <CommCtrl.h>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gamepad {
enum ControlId {
    Banner = 100, Intro, Slot0, Slot1, Slot2, Slot3, SlotChoice, Select, Enable, Pause, Exit,
    Selection, StatusLine, SimulationGroup, Context, Session, Baseline, Modifier, MouseButton,
    Takeover, ContextSwitch, SimulationNotice, Infrastructure, EventHeading, Events, Show,
    ResumeAutomatic, RetryCleanup, LiveStatus = 129, LiveNotice, OpenSettings, Diagnostics, ConfigNotice, RestorePointer,
    AppTitle
};
struct DashboardControl {
    int id;
    const wchar_t* window_class;
    const wchar_t* title;
    DWORD style;
    int x, y, width, height;
};
[[nodiscard]] std::span<const DashboardControl> dashboard_controls();
struct AppPalette {
    COLORREF background, surface, text, secondary, border, accent, accent_text;
    bool dark = false;
    bool high_contrast = false;
};
[[nodiscard]] AppPalette app_palette(bool dark);
[[nodiscard]] int settings_control_page(int id); // -1: common footer/header.
void update_voice_settings_controls(HWND window, bool supported);

// Rendering only: standard HWND controls retain keyboard behavior and accessibility.
class AppUi {
public:
    AppUi();
    ~AppUi();
    AppUi(const AppUi&) = delete;
    AppUi& operator=(const AppUi&) = delete;
    bool refresh(HWND window, std::optional<AppPalette> override_palette = {});
    void style_controls(HWND parent);
    void paint(HWND window, bool settings);
    void paint_to(HWND window, HDC dc, bool settings, UINT render_dpi = 0);
    HBRUSH control_color(HDC dc, HWND control);
    bool draw_item(const DRAWITEMSTRUCT& item);
    bool custom_draw(const NMHDR& header, LRESULT& result);
    [[nodiscard]] const AppPalette& colors() const { return palette_; }
    [[nodiscard]] const std::wstring& error() const { return error_; }
private:
    static LRESULT CALLBACK tab_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void draw_tabs(HWND window, HDC dc);
    void draw_field(HWND window, HDC dc);
    bool rounded(HDC dc, const RECT& rect, COLORREF fill, COLORREF border, float radius);
    class Resources;
    std::unique_ptr<Resources> resources_;
    AppPalette palette_;
    HBRUSH background_ = nullptr, surface_ = nullptr;
    std::wstring error_;
};

class SettingsPages {
public:
    ~SettingsPages();
    bool initialize(HWND window);
    void select(HWND window, int page);
    bool shortcut(HWND window, MSG& message);
    bool dpi_changed(HWND window, UINT dpi);
    bool fit_window(HWND window, UINT dpi, const RECT* suggested = nullptr);
    [[nodiscard]] bool window_bounds(HWND window, UINT dpi, const RECT& suggested,
        const RECT& work_area, RECT& bounds) const;
    [[nodiscard]] int selected() const { return selected_; }
private:
    struct Control { HWND window; int id; RECT rect; };
    std::vector<Control> controls_;
    HFONT font_ = nullptr, title_ = nullptr;
    SIZE client_{};
    int selected_ = 0;
};
} // namespace gamepad
