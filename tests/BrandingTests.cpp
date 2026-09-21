#include "AppUi.h"
#include "Compatibility.h"
#include "Desktop.h"
#include "PracticeLaunch.h"
#include "Settings.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace overworld;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
void version_info(const wchar_t* executable)
{
    const auto size = GetFileVersionInfoSizeW(executable, nullptr);
    check(size != 0 && size <= 1024 * 1024, "product has bounded readable version resources");
    std::vector<BYTE> bytes(size);
    check(GetFileVersionInfoW(executable, 0, size, bytes.data()) != FALSE, "version resources can be read without launching the mapper");
    const auto text = [&](const wchar_t* key) {
        const std::wstring query = std::wstring(L"\\StringFileInfo\\040904b0\\") + key;
        wchar_t* value = nullptr;
        UINT length = 0;
        check(VerQueryValueW(bytes.data(), query.c_str(), reinterpret_cast<void**>(&value), &length) && value && length,
            "branded version field exists");
        return std::wstring(value, length - 1);
    };
    check(text(L"ProductName") == L"Overworld" && text(L"InternalName") == L"Overworld",
        "Windows product/internal names use Overworld");
    check(text(L"OriginalFilename") == L"Overworld.exe" && std::filesystem::path(executable).filename() == L"Overworld.exe",
        "shipping executable and resource filename match");
    check(text(L"FileDescription") == L"Overworld - controller utility", "Windows description is rebranded");
    const auto identity = text(L"ProductVersion");
    check(identity == text(L"FileVersion") && identity.starts_with(L"0.2.0+")
        && identity.find(L".src.") != identity.npos && identity.find(L".voice-") != identity.npos,
        "revision/architecture/flavor provenance remains intact");
    const auto module = LoadLibraryExW(executable, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    check(module != nullptr, "application resources load as data only");
    struct ResourceModule {
        HMODULE value;
        ~ResourceModule() { FreeLibrary(value); }
    } owner{module};
    const auto manifest = FindResourceW(module, MAKEINTRESOURCEW(1), RT_MANIFEST);
    check(manifest != nullptr, "embedded manifest exists");
    const auto resource = LoadResource(module, manifest);
    const auto content = static_cast<const char*>(resource ? LockResource(resource) : nullptr);
    const auto length = SizeofResource(module, manifest);
    check(content && length, "manifest bytes available");
    const std::string_view xml(content, length);
    check(xml.find("name=\"Overworld\"") != xml.npos && xml.find("uiAccess=\"false\"") != xml.npos
        && xml.find("level=\"asInvoker\"") != xml.npos && xml.find("PerMonitorV2") != xml.npos,
        "assembly identity changes without altering privilege or DPI contracts");
}
void compatibility()
{
    check(std::wstring_view(mapper_mutex_name) == L"Local\\GamepadDesktop.M1.SingleMapper",
        "old and new product builds retain the same exclusion boundary");
    check(std::wstring_view(settings_directory) == L"GamepadDesktop", "settings retain their prior location");
    std::filesystem::path path;
    check(settings_path(path).empty() && path.filename() == L"settings.ini"
        && path.parent_path().filename() == settings_directory, "path resolver uses stable settings directory without reading or rewriting settings");
    check(live::output_tag == 0x47445031, "synthetic input ownership tag does not change with branding");
    check(std::wstring_view(dashboard_title(false)) == L"Overworld"
        && std::wstring_view(dashboard_title(true)) == L"Overworld - release-tap", "dashboard and tray titles are rebranded");
    check(std::wstring_view(practice::window_title).starts_with(L"Overworld Practice")
        && std::wstring_view(practice::window_title).find(L"no desktop input") != std::wstring_view::npos,
        "development harness keeps its safety label under the new brand");
    bool title = false;
    for (const auto& control : dashboard_controls()) {
        if (control.id == AppTitle) { title = std::wstring_view(control.title) == L"Overworld"; }
    }
    check(title, "native accessible dashboard title is branded");
    unsigned queries = 0;
    check(existing_mapper_window([&](std::wstring_view name) {
        ++queries;
        return name == L"OverworldM0" ? 1 : 0;
    }) == 1 && queries == 1, "existing branded host has priority without legacy lookup");
    queries = 0;
    check(existing_mapper_window([&](std::wstring_view name) {
        ++queries;
        return name == L"GamepadDesktopM0" ? 2 : 0;
    }) == 2 && queries == 2, "pre-rename mapper remains reachable for bounded cursor recovery");
    queries = 0;
    check(existing_mapper_window([&](std::wstring_view) { ++queries; return 0; }) == 0 && queries == 2,
        "missing hosts remain missing rather than choosing an unrelated window");
}
}
int wmain(int argc, wchar_t** argv)
{
    try {
        check(argc == 2, "requires the built Overworld executable path");
        compatibility();
        version_info(argv[1]);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS Overworld UI/resource identity and legacy settings/mutex/input-tag compatibility; no app launch or input.\n";
}
