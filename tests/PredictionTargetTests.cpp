#include "PredictionTarget.h"

#include <chrono>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace gamepad;
namespace {
void check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
Tick now()
{
    return static_cast<Tick>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
TextWindow identity(HWND window)
{
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(window, &process);
    return {reinterpret_cast<std::uintptr_t>(window), process, thread};
}
class Window {
public:
    explicit Window(HWND window) : window_(window) { check(window != nullptr, "owned window creation"); }
    ~Window() { DestroyWindow(window_); }
    operator HWND() const { return window_; }
private:
    HWND window_;
};
unsigned text_queries = 0, selection_queries = 0, length_queries = 0;
bool delay_selection = false;
bool inconsistent_length = false, invalid_selection = false;
WNDPROC original = nullptr;
LRESULT CALLBACK observed_edit(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_GETTEXT) { ++text_queries; }
    if (message == WM_GETTEXTLENGTH) {
        ++length_queries;
        if (inconsistent_length) {
            return CallWindowProcW(original, window, message, wparam, lparam) + (length_queries % 2);
        }
    }
    if (message == EM_GETSEL) { ++selection_queries; if (delay_selection) { Sleep(50); } }
    if (message == EM_GETSEL && invalid_selection) { return MAKELRESULT(200, 200); }
    return CallWindowProcW(original, window, message, wparam, lparam);
}
PredictionEvidence await(PredictionTarget& probe, const TextTarget& target, std::uint64_t revision)
{
    const auto deadline = now() + 1000;
    while (now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageW(&message); }
        const auto result = probe.observe(target, revision, now());
        if (result.sampled_at && result.revision == revision) { return result; }
        Sleep(1);
    }
    throw std::runtime_error("bounded metadata probe timed out");
}
LRESULT CALLBACK manual_field_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CREATE) {
        const auto label = CreateWindowExW(0, L"Static",
            L"Developer plain-text field. Click below; open Daisywheel; hold D-pad Down to trust.\r\n"
            L"No automatic input or microphone. Do not type secrets. Close to discard.",
            WS_CHILD | WS_VISIBLE, 12, 12, 680, 44, window, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
        const auto edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP
            | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 12, 62, 680, 270,
            window, reinterpret_cast<HMENU>(2), GetModuleHandleW(nullptr), nullptr);
        if (!label || !edit) { return -1; }
        for (auto child : {label, edit}) { SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE); }
        return 0;
    }
    if (message == WM_SIZE) {
        const int width = LOWORD(lparam), height = HIWORD(lparam);
        MoveWindow(GetDlgItem(window, 1), 12, 12, (std::max)(1, width - 24), 44, TRUE);
        MoveWindow(GetDlgItem(window, 2), 12, 62, (std::max)(1, width - 24), (std::max)(1, height - 74), TRUE);
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
int manual_field()
{
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = manual_field_proc;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    type.lpszClassName = L"GamepadPredictionManualField";
    check(RegisterClassW(&type) != 0, "manual field class");
    {
        Window window(CreateWindowExW(0, type.lpszClassName, L"Plain-text prediction trial - developer fixture",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 740, 440, nullptr, nullptr, type.hInstance, nullptr));
        ShowWindow(window, SW_SHOWNORMAL);
        MSG message{};
        int result = 0;
        while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        check(result != -1, "manual field message loop");
    }
    check(UnregisterClassW(type.lpszClassName, type.hInstance) != FALSE, "manual field class cleanup");
    return 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--practice-field") { return manual_field(); }
        check(argc == 2 && std::wstring_view(argv[1]) == L"--allow-owned-metadata",
            "requires --allow-owned-metadata; creates only offscreen owned controls, no input injection");
        const auto foreground = GetForegroundWindow();
        const auto focus = GetFocus();
        Window root(CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Static", L"Owned metadata test",
            WS_POPUP, -32000, -32000, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
        ShowWindow(root, SW_SHOWNOACTIVATE);
        const auto child = [&](const wchar_t* type, DWORD style) {
            return CreateWindowExW(0, type, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 200, 100,
                root, nullptr, GetModuleHandleW(nullptr), nullptr);
        };
        Window plain(child(L"Edit", ES_MULTILINE));
        Window single(child(L"Edit", 0));
        Window password(child(L"Edit", ES_PASSWORD));
        Window readonly(child(L"Edit", ES_MULTILINE | ES_READONLY));
        Window custom(child(L"Static", 0));
        WNDCLASSW derived{};
        check(GetClassInfoW(nullptr, L"Edit", &derived) != FALSE, "system Edit class metadata");
        derived.hInstance = GetModuleHandleW(nullptr);
        derived.lpszClassName = L"PredictionTestDerivedEdit";
        check(RegisterClassW(&derived) != 0, "owned superclass registration");
        {
            Window super(child(derived.lpszClassName, ES_MULTILINE));
            check(prediction_target_support(identity(super)) == PredictionSupport::Unsupported,
                "custom-named Edit superclass is not mistaken for standard Edit");
        }
        check(UnregisterClassW(derived.lpszClassName, derived.hInstance) != FALSE, "owned superclass cleanup");
        check(prediction_target_support(identity(plain)) == PredictionSupport::PlainEdit, "classic multiline Edit eligible");
        check(prediction_target_support(identity(single)) == PredictionSupport::Unsupported, "singleline always excluded");
        check(prediction_target_support(identity(password)) == PredictionSupport::Protected, "password style excluded");
        check(prediction_target_support(identity(readonly)) == PredictionSupport::Unsupported, "readonly excluded");
        check(prediction_target_support(identity(custom)) == PredictionSupport::Unsupported, "custom non-Edit excluded");
        auto mismatch = identity(plain); ++mismatch.process;
        check(prediction_target_support(mismatch) == PredictionSupport::Unavailable, "PID mismatch fails closed");
        EnableWindow(plain, FALSE);
        check(prediction_target_support(identity(plain)) == PredictionSupport::Unsupported, "disabled excluded");
        EnableWindow(plain, TRUE);
        ShowWindow(plain, SW_HIDE);
        check(prediction_target_support(identity(plain)) == PredictionSupport::Unsupported, "hidden excluded");
        ShowWindow(plain, SW_SHOWNOACTIVATE);
        const auto style = GetWindowLongPtrW(plain, GWL_STYLE);
        SetWindowLongPtrW(plain, GWL_STYLE, style | ES_PASSWORD);
        check(prediction_target_support(identity(plain)) == PredictionSupport::Protected, "dynamic protected style immediately excludes");
        SetWindowLongPtrW(plain, GWL_STYLE, style);
        original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(plain, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(observed_edit)));
        check(original != nullptr, "owned message observer");
        PredictionTarget probe;
        probe.start();
        TextTarget target;
        target.windows = {identity(root), identity(root), identity(plain)};
        auto evidence = await(probe, target, 0);
        check(evidence.support == PredictionSupport::PlainEdit && evidence.caret == 0u, "real owned non-password selection proof");
        SetWindowTextW(plain, L"authored test only");
        SendMessageW(plain, EM_SETSEL, 3, 3);
        evidence = await(probe, target, 1);
        check(evidence.caret == 3u, "worker reads only exact collapsed selection index");
        SendMessageW(plain, EM_SETSEL, 1, 3);
        evidence = await(probe, target, 2);
        check(!evidence.caret && evidence.support == PredictionSupport::PlainEdit, "selection is not an append proof");
        delay_selection = true;
        evidence = await(probe, target, 3);
        check(evidence.support == PredictionSupport::Unavailable && !evidence.caret, "20ms metadata timeout fails closed");
        delay_selection = false;
        inconsistent_length = true;
        evidence = await(probe, target, 4);
        check(evidence.support == PredictionSupport::Unavailable && !evidence.caret, "changing length rejects batch");
        inconsistent_length = false;
        invalid_selection = true;
        evidence = await(probe, target, 5);
        check(evidence.support == PredictionSupport::Unavailable && !evidence.caret, "selection cannot exceed scalar length");
        invalid_selection = false;
        SendMessageW(plain, EM_LIMITTEXT, 100000, 0);
        const std::wstring large(70001, L'a');
        check(SetWindowTextW(plain, large.c_str()) != FALSE, "owned large fixture text");
        SendMessageW(plain, EM_SETSEL, 70000, 70000);
        DWORD large_start = 0, large_end = 0;
        SendMessageW(plain, EM_GETSEL, reinterpret_cast<WPARAM>(&large_start), reinterpret_cast<LPARAM>(&large_end));
        check(large_start == 70000 && large_end == 70000, "owned fixture really exercises positions above 65535");
        evidence = await(probe, target, 6);
        check(evidence.support == PredictionSupport::Unavailable && !evidence.caret,
            "large field length must exclude actual 70000-to-4464 packed caret wrap, not rely on overflow sentinel");
        probe.close();
        check(!probe.observe(target, 7, now()).caret, "close invalidates old cached caret/generation");
        target.windows[2] = identity(password);
        check(probe.observe(target, 7, now()).support == PredictionSupport::Protected, "worker never authorizes password");
        probe.close();
        check(text_queries == 0 && selection_queries >= 3 && length_queries >= 6,
            "no text value read, only scalar length and EM_GETSEL metadata");
        check(foreground == GetForegroundWindow() && focus == GetFocus(), "metadata test never changes focus");
        std::cout << "PASS native multiline/password/unknown/readonly/identity/style metadata, bounded caret worker, no text reads, unchanged focus\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
