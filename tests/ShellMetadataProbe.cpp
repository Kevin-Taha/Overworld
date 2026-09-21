#include <Windows.h>
#include <dwmapi.h>
#include <WtsApi32.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) { CloseHandle(value); } }
};

std::wstring quoted_metadata(std::wstring_view value)
{
    std::wostringstream result;
    result << L'"';
    for (wchar_t c : value.substr(0, 256)) {
        if (c >= L' ' && c <= L'~' && c != L'"' && c != L'\\') { result << c; }
        else { result << L"\\u" << std::hex << std::setw(4) << std::setfill(L'0') << static_cast<unsigned>(c); }
    }
    result << L'"';
    return result.str();
}

bool desktop_named(HANDLE desktop, const wchar_t* expected)
{
    std::array<wchar_t, 256> name{};
    DWORD needed = 0;
    return GetUserObjectInformationW(desktop, UOI_NAME, name.data(), static_cast<DWORD>(sizeof(name)), &needed)
        && std::wstring_view(name.data()) == expected;
}

bool ordinary_unlocked_desktop()
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
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) { return false; }
    const bool ordinary = desktop_named(desktop, L"Default");
    CloseDesktop(desktop);
    return ordinary;
}

std::wstring window_metadata(std::wstring_view role, HWND window)
{
    std::wostringstream result;
    result << role << L" hwnd=" << reinterpret_cast<std::uintptr_t>(window);
    if (!window) { return result.str(); }
    DWORD process = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (!thread) { result << L" identity_error=" << GetLastError(); return result.str(); }
    const auto root = GetAncestor(window, GA_ROOT);
    const auto root_owner = GetAncestor(window, GA_ROOTOWNER);
    result << L" pid=" << process << L" tid=" << thread
        << L" root=" << reinterpret_cast<std::uintptr_t>(root)
        << L" root_owner=" << reinterpret_cast<std::uintptr_t>(root_owner)
        << L" owner=" << reinterpret_cast<std::uintptr_t>(GetWindow(window, GW_OWNER))
        << L" visible=" << (IsWindowVisible(window) != FALSE)
        << L" enabled=" << (IsWindowEnabled(window) != FALSE)
        << L" iconic=" << (IsIconic(window) != FALSE);
    std::array<wchar_t, 256> name{};
    if (GetClassNameW(window, name.data(), static_cast<int>(name.size()))) {
        result << L" class=" << quoted_metadata(name.data());
    } else { result << L" class_error=" << GetLastError(); }
    DWORD cloaked = 0;
    const auto cloak_result = root ? DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) : E_FAIL;
    if (SUCCEEDED(cloak_result)) { result << L" root_cloaked=" << cloaked; }
    else { result << L" cloak_hresult=" << static_cast<unsigned long>(cloak_result); }
    DWORD session = 0;
    if (ProcessIdToSessionId(process, &session)) { result << L" session=" << session; }
    else { result << L" session_error=" << GetLastError(); }

    Handle process_handle{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process)};
    if (!process_handle.value) { result << L" process_error=" << GetLastError(); return result.str(); }
    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process_handle.value, 0, path.data(), &length)) {
        const std::wstring_view image(path.data(), length);
        const auto separator = image.find_last_of(L"\\/");
        result << L" image_basename=" << quoted_metadata(image.substr(separator == image.npos ? 0 : separator + 1));
    } else { result << L" image_error=" << GetLastError(); }
    Handle token;
    if (!OpenProcessToken(process_handle.value, TOKEN_QUERY, &token.value)) {
        result << L" token_error=" << GetLastError(); return result.str();
    }
    alignas(TOKEN_MANDATORY_LABEL) std::array<BYTE, 4096> buffer{};
    DWORD needed = 0, ui_access = 0;
    if (GetTokenInformation(token.value, TokenIntegrityLevel, buffer.data(), static_cast<DWORD>(buffer.size()), &needed)
        && GetTokenInformation(token.value, TokenUIAccess, &ui_access, sizeof(ui_access), &needed)) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
        if (IsValidSid(label->Label.Sid) && *GetSidSubAuthorityCount(label->Label.Sid)) {
            result << L" integrity=" << *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1)
                << L" ui_access=" << ui_access;
        } else { result << L" token_invalid=1"; }
    } else { result << L" token_error=" << GetLastError(); }
    DWORD final_process = 0;
    const auto final_thread = GetWindowThreadProcessId(window, &final_process);
    result << L" identity_stable=" << (thread == final_thread && process == final_process
        && root == GetAncestor(window, GA_ROOT) && root_owner == GetAncestor(window, GA_ROOTOWNER));
    return result.str();
}

std::wstring snapshot()
{
    if (!ordinary_unlocked_desktop()) { return L"normal_unlocked_desktop=0; topology withheld"; }
    const auto foreground = GetForegroundWindow();
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(foreground, &process);
    GUITHREADINFO gui{sizeof(gui)};
    SetLastError(ERROR_SUCCESS);
    const bool gui_known = thread && GetGUIThreadInfo(thread, &gui);
    const DWORD gui_error = gui_known ? 0 : GetLastError();
    POINT point{};
    const auto hit = GetCursorPos(&point) ? WindowFromPoint(point) : nullptr;
    std::wostringstream result;
    result << L"normal_unlocked_desktop=1 gui_known=" << gui_known << L" gui_error=" << gui_error;
    if (gui_known) { result << L" gui_flags=" << gui.flags; }
    CURSORINFO cursor{sizeof(cursor)};
    if (GetCursorInfo(&cursor)) { result << L" native_cursor_flags=" << cursor.flags; }
    else { result << L" cursor_error=" << GetLastError(); }
    result << L'\n' << window_metadata(L"foreground", foreground);
    if (gui_known) {
        result << L'\n' << window_metadata(L"active", gui.hwndActive)
            << L'\n' << window_metadata(L"focus", gui.hwndFocus)
            << L'\n' << window_metadata(L"capture", gui.hwndCapture)
            << L'\n' << window_metadata(L"menu", gui.hwndMenuOwner);
    }
    result << L'\n' << window_metadata(L"pointer_hit", hit)
        << L'\n' << window_metadata(L"pointer_hit_root", hit ? GetAncestor(hit, GA_ROOT) : nullptr);
    GUITHREADINFO final_gui{sizeof(final_gui)};
    const bool final_gui_known = thread && GetGUIThreadInfo(thread, &final_gui);
    result << L"\ngui_route_stable=" << (gui_known && final_gui_known
        && gui.hwndActive == final_gui.hwndActive && gui.hwndFocus == final_gui.hwndFocus
        && gui.hwndCapture == final_gui.hwndCapture && gui.hwndMenuOwner == final_gui.hwndMenuOwner);
    if (!ordinary_unlocked_desktop() || foreground != GetForegroundWindow()) {
        return L"desktop_or_foreground_changed_during_sample=1; topology withheld";
    }
    return result.str();
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2 || std::wstring_view(argv[1]) != L"--metadata-only") {
        std::wcout << L"Usage: overworld_shell_probe --metadata-only\n"
            L"Explicit opt-in: after a 3-second delay, sample at most 20 times over 10 seconds.\n"
            L"An OS metadata call that stalls can extend the sampling duration; no cross-process messages are sent.\n"
            L"Console output only: foreground/focus/capture/menu/hit identities, process basenames,\n"
            L"window classes/ownership/cloak, session/integrity/UIAccess and query errors.\n"
            L"No window titles, text, key data, coordinates, audio, screenshot, UIA, focus changes,\n"
            L"input, hooks, cursor visibility changes, settings or file writes.\n"
            L"Run on the affected device only when ready; do not treat probe metadata as permission to inject.\n";
        return 2;
    }
    try {
        std::wcout << L"Metadata-only shell probe. No shell interaction is automated.\n"
            L"Starting in 3 seconds; you may reproduce the issue yourself.\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::seconds(10);
        std::wstring previous;
        for (unsigned i = 0; i < 20 && std::chrono::steady_clock::now() < deadline; ++i) {
            const auto observation = snapshot();
            if (observation != previous) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
                std::wcout << L"\n--- sample_ms=" << elapsed.count() << L" ---\n" << observation << L'\n' << std::flush;
                previous = observation;
            }
            std::this_thread::sleep_until((std::min)(deadline, started + std::chrono::milliseconds(500 * (i + 1))));
        }
        std::wcout << L"Done. This probe cannot prove the origin of controller input or elevated-band rendering.\n"
            L"For filter decisions, separately inspect Overworld Details: Build, Eligibility,\n"
            L"Last missed GAMEPAD down, policy gate/age, and Keyboard dismissal/recovery status.\n";
    } catch (const std::exception&) {
        std::wcerr << L"Metadata probe failed; no input was sent.\n";
        return 1;
    }
}
