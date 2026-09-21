#include "PredictionTarget.h"

#include <Windows.h>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace overworld;
namespace {
void check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
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
unsigned target_messages = 0;
WNDPROC original = nullptr;
LRESULT CALLBACK observed_edit(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    ++target_messages;
    return CallWindowProcW(original, window, message, wparam, lparam);
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        check(argc == 2 && std::wstring_view(argv[1]) == L"--allow-owned-metadata",
            "requires --allow-owned-metadata; creates only offscreen owned controls, no input injection");
        const auto foreground = GetForegroundWindow();
        const auto focus = GetFocus();
        const auto rich_library = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(rich_library != nullptr, "system RichEdit library");
        {
            Window root(CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Static", L"Owned privacy test",
                WS_POPUP, -32000, -32000, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
            const auto child = [&](const wchar_t* type, DWORD style) {
                return CreateWindowExW(0, type, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 200, 100,
                    root, nullptr, GetModuleHandleW(nullptr), nullptr);
            };
            Window plain(child(L"Edit", ES_MULTILINE));
            Window single(child(L"Edit", 0));
            Window password(child(L"Edit", ES_PASSWORD));
            Window readonly(child(L"Edit", ES_MULTILINE | ES_READONLY));
            Window custom(child(L"Static", 0));
            Window rich(child(L"RICHEDIT50W", ES_MULTILINE));
            Window rich_password(child(L"RICHEDIT50W", ES_PASSWORD));
            for (HWND window : {static_cast<HWND>(plain), static_cast<HWND>(single), static_cast<HWND>(readonly),
                    static_cast<HWND>(custom), static_cast<HWND>(rich)}) {
                check(prediction_target_privacy(identity(window)) == PredictionPrivacy::Unknown,
                    "normal Edit, singleline, readonly, custom and RichEdit privacy is unknown, not a support allowlist");
            }
            for (HWND window : {static_cast<HWND>(password), static_cast<HWND>(rich_password)}) {
                check(prediction_target_privacy(identity(window)) == PredictionPrivacy::Protected,
                    "known Edit and RichEdit password metadata suppresses suggestions");
            }
            SendMessageW(single, EM_SETPASSWORDCHAR, L'*', 0);
            check(prediction_target_privacy(identity(single)) == PredictionPrivacy::Protected, "dynamic Edit password masking suppressed");
            SendMessageW(single, EM_SETPASSWORDCHAR, 0, 0);
            check(prediction_target_privacy(identity(single)) == PredictionPrivacy::Unknown, "unmasking is still unverified privacy");
            auto mismatch = identity(plain); ++mismatch.process;
            check(prediction_target_privacy(mismatch) == PredictionPrivacy::Unavailable, "PID mismatch fails explicitly");
            check(prediction_target_privacy({}) == PredictionPrivacy::Unavailable, "missing target cannot authorize prediction");
            WNDCLASSW type{};
            type.lpfnWndProc = DefWindowProcW;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"PredictionTestWebContent";
            check(RegisterClassW(&type) != 0, "owned browser-style custom class");
            {
                Window web(child(type.lpszClassName, 0));
                check(prediction_target_privacy(identity(web)) == PredictionPrivacy::Unknown,
                    "browser-style native host does not require Edit class or selection APIs");
            }
            check(UnregisterClassW(type.lpszClassName, type.hInstance) != FALSE, "owned class cleanup");
            original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(plain, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(observed_edit)));
            check(original != nullptr, "owned message observer");
            target_messages = 0;
            for (unsigned i = 0; i < 20; ++i) {
                check(prediction_target_privacy(identity(plain)) == PredictionPrivacy::Unknown, "subclassed Edit remains unverified");
            }
            check(target_messages == 0, "privacy check sends no text, length, selection or other target messages");
            SetWindowLongPtrW(plain, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
            check(foreground == GetForegroundWindow() && focus == GetFocus(), "owned privacy test never changes focus");
        }
        check(FreeLibrary(rich_library) != FALSE, "system RichEdit module cleanup");
        std::cout << "PASS Edit/RichEdit password suppression; generic/browser/singleline privacy unverified; zero target messages; unchanged focus\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
