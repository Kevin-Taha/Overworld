#include "AppUi.h"
#include "KeyboardPlacement.h"
#include "KeyboardVisual.h"

#include <TlHelp32.h>
#include <WtsApi32.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
enum class Role { Other, Application, Shell };
struct Process {
    HANDLE handle = nullptr;
    Role role = Role::Other;
    std::wstring image, version;
    ~Process() { if (handle) { CloseHandle(handle); } }
};
struct Window {
    HWND handle = nullptr;
    DWORD pid = 0, tid = 0;
    Role role = Role::Other;
    unsigned order = 0;
    RECT bounds{}, frame{};
    bool bounds_known = false, frame_known = false, popup = false, host = false;
    bool direct = false, child = false;
};
struct Probe {
    std::map<DWORD, std::unique_ptr<Process>> processes;
    std::vector<Window> windows;
    Clock::time_point deadline;
    HWND foreground = nullptr;
    unsigned visited = 0;
};
std::wstring safe(std::wstring_view value)
{
    std::wstring result;
    for (const auto ch : value.substr(0, 512)) {
        result += ch >= L' ' && ch <= L'~' && ch != L'"' ? ch : L'?';
    }
    return result;
}
void rectangle(std::wstring_view label, RECT value)
{
    std::wcout << label << L"=[" << value.left << L"," << value.top << L"," << value.right << L"," << value.bottom << L"]";
}
bool desktop_named(HANDLE object, const wchar_t* expected)
{
    std::array<wchar_t, 256> name{};
    DWORD needed = 0;
    return GetUserObjectInformationW(object, UOI_NAME, name.data(), static_cast<DWORD>(sizeof(name)), &needed)
        && std::wstring_view(name.data()) == expected;
}
bool ordinary_desktop()
{
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) { return false; }
    PWTSINFOEXW info = nullptr;
    DWORD bytes = 0;
    const bool queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx,
        reinterpret_cast<LPWSTR*>(&info), &bytes) != FALSE;
    const bool unlocked = queried && info && bytes >= sizeof(WTSINFOEXW) && info->Level == 1
        && info->Data.WTSInfoExLevel1.SessionState == WTSActive
        && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    if (info) { WTSFreeMemory(info); }
    if (!unlocked || !desktop_named(GetProcessWindowStation(), L"WinSta0")
        || !desktop_named(GetThreadDesktop(GetCurrentThreadId()), L"Default")) { return false; }
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) { return false; }
    const bool ordinary = desktop_named(desktop, L"Default");
    CloseDesktop(desktop);
    return ordinary;
}
std::wstring version(const wchar_t* path)
{
    const auto size = GetFileVersionInfoSizeW(path, nullptr);
    if (!size || size > 1024 * 1024) { return L"unavailable"; }
    std::vector<BYTE> bytes(size);
    if (!GetFileVersionInfoW(path, 0, size, bytes.data())) { return L"unavailable"; }
    struct Translation { WORD language, page; };
    Translation* translations = nullptr;
    UINT count = 0;
    if (!VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &count)
        || count < sizeof(Translation)) { return L"unavailable"; }
    wchar_t query[96]{};
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\ProductVersion", translations[0].language, translations[0].page);
    wchar_t* text = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), query, reinterpret_cast<void**>(&text), &length) || !length) { return L"unavailable"; }
    return safe(std::wstring_view(text, length - 1));
}
bool inventory(Probe& probe)
{
    for (auto i = probe.processes.begin(); i != probe.processes.end();) {
        if (WaitForSingleObject(i->second->handle, 0) != WAIT_TIMEOUT) { i = probe.processes.erase(i); }
        else { ++i; }
    }
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { return false; }
    PROCESSENTRY32W entry{sizeof(entry)};
    bool more = Process32FirstW(snapshot, &entry) != FALSE;
    unsigned inspected = 0;
    while (more && ++inspected <= 4096 && Clock::now() < probe.deadline) {
        const bool application = _wcsicmp(entry.szExeFile, L"GamepadDesktop.exe") == 0;
        const bool shell = gamepad::keyboard_shell_image(entry.szExeFile);
        if ((application || shell) && !probe.processes.contains(entry.th32ProcessID)) {
            auto process = std::make_unique<Process>();
            process->handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (process->handle) {
                std::array<wchar_t, 32768> path{};
                DWORD length = static_cast<DWORD>(path.size());
                if (QueryFullProcessImageNameW(process->handle, 0, path.data(), &length)) {
                    const std::wstring_view full(path.data(), length);
                    const auto separator = full.find_last_of(L"\\/");
                    const auto name = full.substr(separator == full.npos ? 0 : separator + 1);
                    if ((application && _wcsicmp(std::wstring(name).c_str(), L"GamepadDesktop.exe") == 0)
                        || (shell && gamepad::keyboard_shell_image(name))) {
                        process->role = application ? Role::Application : Role::Shell;
                        process->image = name;
                        if (application) { process->version = version(path.data()); }
                        probe.processes.emplace(entry.th32ProcessID, std::move(process));
                    }
                }
            }
        }
        more = Process32NextW(snapshot, &entry) != FALSE;
    }
    CloseHandle(snapshot);
    return true;
}
BOOL CALLBACK window(HWND hwnd, LPARAM context)
{
    auto& probe = *reinterpret_cast<Probe*>(context);
    if (++probe.visited > 1024 || probe.windows.size() >= 64 || Clock::now() >= probe.deadline) { return FALSE; }
    Window item;
    item.handle = hwnd;
    item.order = probe.visited;
    item.tid = GetWindowThreadProcessId(hwnd, &item.pid);
    const auto process = probe.processes.find(item.pid);
    if (!item.tid || process == probe.processes.end()) { return TRUE; }
    item.role = process->second->role;
    wchar_t name[256]{};
    if (!GetClassNameW(hwnd, name, static_cast<int>(std::size(name)))) { return TRUE; }
    item.popup = item.role == Role::Application && std::wstring_view(name) == L"GamepadDesktopKeyboard";
    item.host = item.role == Role::Application && std::wstring_view(name) == L"GamepadDesktopM0";
    if (item.role == Role::Application && !item.popup && !item.host) { return TRUE; }
    item.bounds_known = GetWindowRect(hwnd, &item.bounds) != FALSE;
    item.frame_known = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &item.frame, sizeof(item.frame)));
    probe.windows.push_back(item);
    return TRUE;
}
void foreground_window(Probe& probe)
{
    const auto root = probe.foreground ? GetAncestor(probe.foreground, GA_ROOT) : nullptr;
    if (!root) { return; }
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(root, &pid)) { return; }
    // Immersive shell roots can be absent from EnumWindows; query this identity directly.
    if (!probe.processes.contains(pid)) {
        auto process = std::make_unique<Process>();
        process->handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (process->handle) {
            std::array<wchar_t, 32768> path{};
            DWORD length = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process->handle, 0, path.data(), &length)) {
                const std::wstring_view full(path.data(), length);
                const auto separator = full.find_last_of(L"\\/");
                const auto name = full.substr(separator == full.npos ? 0 : separator + 1);
                if (gamepad::keyboard_shell_image(name)) {
                    process->role = Role::Shell;
                    process->image = name;
                    probe.processes.emplace(pid, std::move(process));
                }
            }
        }
    }
    if (std::none_of(probe.windows.begin(), probe.windows.end(), [&](const auto& item) { return item.handle == root; })) {
        const auto before = probe.windows.size();
        window(root, reinterpret_cast<LPARAM>(&probe));
        if (probe.windows.size() > before) { probe.windows.back().direct = true; probe.windows.back().order = 0; }
    }
    const auto process = probe.processes.find(pid);
    if (process == probe.processes.end() || process->second->role != Role::Shell) { return; }
    struct Children { Probe& probe; unsigned visited = 0; } children{probe};
    EnumChildWindows(root, [](HWND child, LPARAM context) -> BOOL {
        auto& state = *reinterpret_cast<Children*>(context);
        if (++state.visited > 32 || Clock::now() >= state.probe.deadline) { return FALSE; }
        const auto before = state.probe.windows.size();
        const auto result = window(child, reinterpret_cast<LPARAM>(&state.probe));
        if (state.probe.windows.size() > before) {
            state.probe.windows.back().child = true;
            state.probe.windows.back().order = 0;
        }
        return result;
    }, reinterpret_cast<LPARAM>(&children));
}
void status(const Window& host)
{
    const auto control = GetDlgItem(host.handle, gamepad::LiveStatus);
    DWORD pid = 0;
    wchar_t type[32]{};
    if (!control || !GetWindowThreadProcessId(control, &pid) || pid != host.pid
        || GetParent(control) != host.handle || !GetClassNameW(control, type, static_cast<int>(std::size(type)))
        || (_wcsicmp(type, L"STATIC") && _wcsicmp(type, L"EDIT"))) {
        std::wcout << L" app_status=unavailable";
        return;
    }
    std::array<wchar_t, 513> text{};
    DWORD_PTR copied = 0;
    if (!SendMessageTimeoutW(control, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &copied)) {
        std::wcout << L" app_status_error=" << GetLastError();
        return;
    }
    DWORD final_pid = 0;
    if (GetWindowThreadProcessId(host.handle, &final_pid) != host.tid || final_pid != host.pid) {
        std::wcout << L" app_status=identity_changed";
        return;
    }
    std::wcout << L" app_status=\"" << safe(std::wstring_view(text.data(), std::min<std::size_t>(copied, text.size() - 1))) << L"\"";
}
const wchar_t* hit_role(HWND hwnd, const Probe& probe)
{
    const auto root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    for (const auto& item : probe.windows) {
        if (item.handle == root) { return item.popup ? L"keyboard" : item.host ? L"app" : L"shell"; }
    }
    return root ? L"other" : L"none";
}
void output(Probe& probe)
{
    std::wcout << L"foreground_role=" << hit_role(probe.foreground, probe)
        << L" enumerated=" << probe.visited << L" matched=" << probe.windows.size() << L"\n";
    for (const auto& item : probe.windows) {
        DWORD pid = 0;
        if (GetWindowThreadProcessId(item.handle, &pid) != item.tid || pid != item.pid) {
            std::wcout << L"window_identity_changed\n"; continue;
        }
        const auto& process = *probe.processes.at(item.pid);
        wchar_t type[256]{};
        GetClassNameW(item.handle, type, static_cast<int>(std::size(type)));
        DWORD cloak = 0;
        const auto cloak_result = DwmGetWindowAttribute(item.handle, DWMWA_CLOAKED, &cloak, sizeof(cloak));
        const auto style = GetWindowLongPtrW(item.handle, GWL_EXSTYLE);
        std::wcout << L"role=" << (item.popup ? L"keyboard" : item.host ? L"app" : L"shell")
            << L" image=" << safe(process.image) << L" pid=" << item.pid
            << L" hwnd=" << reinterpret_cast<std::uintptr_t>(item.handle)
            << L" class=" << safe(type) << L" enum_order=" << item.order
            << L" source=" << (item.child ? L"shell_child" : item.direct ? L"direct_foreground" : L"enum_windows")
            << L" foreground=" << (item.handle == probe.foreground)
            << L" visible=" << (IsWindowVisible(item.handle) != FALSE)
            << L" iconic=" << (IsIconic(item.handle) != FALSE)
            << L" topmost=" << ((style & WS_EX_TOPMOST) != 0)
            << L" noactivate=" << ((style & WS_EX_NOACTIVATE) != 0)
            << L" layered=" << ((style & WS_EX_LAYERED) != 0)
            << L" dpi=" << GetDpiForWindow(item.handle)
            << L" cloak_hresult=" << static_cast<unsigned long>(cloak_result) << L" cloak=" << cloak;
        if (item.bounds_known) { std::wcout << L" "; rectangle(L"window", item.bounds); }
        if (item.frame_known) { std::wcout << L" "; rectangle(L"frame", item.frame); }
        RECT region{};
        const auto region_kind = GetWindowRgnBox(item.handle, &region);
        std::wcout << L" region_kind=" << region_kind;
        if (region_kind == SIMPLEREGION || region_kind == COMPLEXREGION) {
            std::wcout << L" "; rectangle(L"region_relative", region);
        }
        if (item.host) { std::wcout << L" build=\"" << process.version << L"\""; status(item); }
        if (item.role == Role::Shell && !item.child && item.handle == probe.foreground) {
            GUITHREADINFO gui{sizeof(gui)};
            SetLastError(ERROR_SUCCESS);
            if (GetGUIThreadInfo(item.tid, &gui)) {
                std::wcout << L" gui_flags=" << gui.flags
                    << L" active_present=" << (gui.hwndActive != nullptr)
                    << L" focus_present=" << (gui.hwndFocus != nullptr)
                    << L" focus_role=" << hit_role(gui.hwndFocus, probe)
                    << L" capture_present=" << (gui.hwndCapture != nullptr)
                    << L" menu_present=" << (gui.hwndMenuOwner != nullptr);
            } else { std::wcout << L" gui_error=" << GetLastError(); }
        }
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(item.handle, MONITOR_DEFAULTTONEAREST), &monitor)) {
            std::wcout << L" "; rectangle(L"work", monitor.rcWork);
            if (item.role == Role::Shell && !item.child && item.frame_known && IsWindowVisible(item.handle) && SUCCEEDED(cloak_result) && !cloak) {
                const auto extent = item.bounds_known && region_kind != NULLREGION
                    ? gamepad::keyboard_shell_obstruction(item.frame, item.bounds,
                        region_kind == SIMPLEREGION || region_kind == COMPLEXREGION ? std::optional<RECT>{region} : std::nullopt)
                    : std::nullopt;
                std::wcout << L" ";
                if (extent) { rectangle(L"shell_extent", *extent); }
                else { std::wcout << L"shell_extent=unavailable"; }
                for (const int height : {gamepad::keyboard_visual_height, gamepad::keyboard_prediction_height}) {
                    const auto candidate = extent ? gamepad::keyboard_popup_placement(monitor.rcWork, GetDpiForWindow(item.handle), height, {}, *extent) : std::nullopt;
                    std::wcout << L" candidate_" << height << L"=";
                    if (candidate) { rectangle(L"", candidate->bounds); std::wcout << L"@" << candidate->percent << L"%"; }
                    else { std::wcout << L"no_space_or_unavailable"; }
                }
                std::wcout << L" work_hit_grid_7x5=";
                for (LONG y = 0; y < 5; ++y) {
                    for (LONG x = 0; x < 7; ++x) {
                        const POINT point{static_cast<LONG>(monitor.rcWork.left + 1
                            + (static_cast<long long>(monitor.rcWork.right) - monitor.rcWork.left - 2) * x / 6),
                            static_cast<LONG>(monitor.rcWork.top + 1
                            + (static_cast<long long>(monitor.rcWork.bottom) - monitor.rcWork.top - 2) * y / 4)};
                        std::wcout << hit_role(WindowFromPoint(point), probe) << L",";
                    }
                    std::wcout << L"/";
                }
            }
        }
        if (item.popup && item.bounds_known && IsWindowVisible(item.handle)) {
            std::wcout << L" hit_grid=";
            for (LONG y = 1; y <= 3; ++y) {
                for (LONG x = 1; x <= 3; ++x) {
                    const POINT point{static_cast<LONG>(item.bounds.left
                        + (static_cast<long long>(item.bounds.right) - item.bounds.left) * x / 4),
                        static_cast<LONG>(item.bounds.top
                        + (static_cast<long long>(item.bounds.bottom) - item.bounds.top) * y / 4)};
                    std::wcout << hit_role(WindowFromPoint(point), probe) << L",";
                }
            }
        }
        DWORD final_pid = 0;
        std::wcout << L" identity_stable=" << (GetWindowThreadProcessId(item.handle, &final_pid) == item.tid
            && final_pid == item.pid && WaitForSingleObject(process.handle, 0) == WAIT_TIMEOUT);
        std::wcout << L"\n";
    }
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2 || std::wstring_view(argv[1]) != L"--metadata-only") {
        std::wcout << L"Usage: gamepad_keyboard_placement_probe --metadata-only\n"
            L"Explicit user-run diagnostic: five-second delay, then 20 seconds of app/Start/Search window metadata.\n"
            L"Reads only Gamepad Desktop's own status control, not search fields, window titles, or typed text.\n"
            L"No screenshots, controller sampling, input output, focus changes, hooks, or settings changes.\n";
        return 2;
    }
    try {
        std::wcout << L"Keyboard placement diagnostic v4; prepare only until countdown completes.\n"
            L"Windows enumeration and point hit tests do not prove compositor visibility.\n";
        for (unsigned remaining = 5; remaining; --remaining) {
            std::wcout << L"Countdown: " << remaining << L"\n" << std::flush;
            Sleep(1000);
        }
        Probe probe;
        const auto start = Clock::now();
        probe.deadline = start + std::chrono::seconds(20);
        for (unsigned i = 0; i < 40 && Clock::now() < probe.deadline; ++i) {
            if (!ordinary_desktop()) { std::wcout << L"Protected/locked/unavailable desktop; stopping without metadata.\n"; return 1; }
            probe.foreground = GetForegroundWindow();
            if (!inventory(probe)) { std::wcerr << L"Process inventory failed: " << GetLastError() << L"\n"; return 1; }
            probe.windows.clear(); probe.visited = 0;
            SetLastError(ERROR_SUCCESS);
            const bool enumerated = EnumWindows(window, reinterpret_cast<LPARAM>(&probe)) != FALSE;
            foreground_window(probe);
            std::wcout << L"\n--- sample_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()
                << L" enumeration_complete=" << enumerated << L" ---\n";
            if (ordinary_desktop() && GetForegroundWindow() == probe.foreground) { output(probe); }
            else { std::wcout << L"Desktop/foreground changed; snapshot withheld.\n"; }
            std::wcout << std::flush;
            Sleep(500);
        }
        std::wcout << L"Finished. No input was generated.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Diagnostic failure: " << error.what() << '\n';
        return 1;
    }
}
