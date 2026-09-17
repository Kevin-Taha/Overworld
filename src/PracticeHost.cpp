#include "InstanceGuard.h"
#include "PracticeLaunch.h"
#include "PracticeReader.h"
#include "XInputReader.h"

#include <Shellapi.h>
#include <new>
#include <system_error>

namespace {
class PracticeBackend final : public gamepad::practice::ReaderBackend {
public:
    gamepad::Tick now() override { return gamepad::monotonic_now(); }
    bool focused(HWND window) override { return GetForegroundWindow() == window && !IsIconic(window); }
    gamepad::Controller slot(unsigned index) override { return reader_.read_slot(index); }
private:
    gamepad::XInputReader reader_;
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    using namespace gamepad::practice;
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        MessageBoxW(nullptr, L"Could not read development harness arguments. No controller reader was started.",
            window_title, MB_OK | MB_ICONERROR);
        return 1;
    }
    const std::array<std::wstring_view, 1> argument{argc == 2 ? argv[1] : L""};
    const auto mode = argc == 1 ? launch({}) : argc == 2 ? launch(argument) : Launch::Invalid;
    LocalFree(argv);
    if (mode != Launch::Run) {
        MessageBoxW(nullptr,
            L"Usage: GamepadDesktopPractice.exe [--help]\r\n"
            L"Development-only component harness, not a product feature or release asset.\r\n"
            L"No desktop input, microphone, model, settings writes, or external text delivery.\r\n"
            L"Close other instances before running. Select the window and center the controller.",
            window_title, MB_OK | (mode == Launch::Help ? MB_ICONINFORMATION : MB_ICONERROR));
        if (mode == Launch::Invalid) { return 2; }
        return 0;
    }
    gamepad::MapperInstanceGuard singleton;
    if (!singleton.acquired()) {
        MessageBoxW(nullptr, singleton.already_running()
            ? L"Exit the running Gamepad Desktop instance or development harness first. No controller reader was started."
            : L"The shared mapper instance guard is unavailable. No controller reader was started.",
            window_title, MB_OK | MB_ICONWARNING);
        return 1;
    }
    int result = 1;
    try {
        Reader reader(std::make_shared<PracticeBackend>());
        if (reader.start()) {
            result = run(instance, show, reader);
            if (!reader.stop()) { result = 1; }
        }
        if (!reader.error().empty()) {
            MessageBoxW(nullptr, reader.error().c_str(), window_title, MB_OK | MB_ICONERROR);
        }
    } catch (const std::system_error&) {
        result = 1;
        MessageBoxW(nullptr, L"Could not start the development-only controller reader.",
            window_title, MB_OK | MB_ICONERROR);
    } catch (const std::bad_alloc&) {
        result = 1;
        MessageBoxW(nullptr, L"Could not allocate the development-only controller reader.",
            window_title, MB_OK | MB_ICONERROR);
    }
    return result;
}
