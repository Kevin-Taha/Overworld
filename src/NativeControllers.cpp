#include "NativeControllers.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <new>
#include <string_view>

namespace gamepad {
namespace {
struct Enumeration {
    IDirectInput8W* input = nullptr;
    ControllerDevices devices;
    unsigned visited = 0;
    const std::atomic<bool>* stop = nullptr;
};
BOOL CALLBACK enumerate_device(const DIDEVICEINSTANCEW* instance, void* context)
{
    auto& enumeration = *static_cast<Enumeration*>(context);
    if (enumeration.stop && enumeration.stop->load()) { enumeration.devices.error = ERROR_CANCELLED; return DIENUM_STOP; }
    if (++enumeration.visited > 64) { enumeration.devices.error = ERROR_MORE_DATA; return DIENUM_STOP; }
    IDirectInputDevice8W* device = nullptr;
    auto hr = enumeration.input->CreateDevice(instance->guidInstance, &device, nullptr);
    if (FAILED(hr)) { enumeration.devices.error = static_cast<DWORD>(hr); return DIENUM_STOP; }
    DIPROPGUIDANDPATH path{};
    path.diph = {sizeof(path), sizeof(path.diph), 0, DIPH_DEVICE};
    hr = device->GetProperty(DIPROP_GUIDANDPATH, &path.diph);
    device->Release();
    if (FAILED(hr)) { enumeration.devices.error = static_cast<DWORD>(hr); return DIENUM_STOP; }
    for (auto& ch : path.wszPath) { if (ch >= L'a' && ch <= L'z') { ch -= L'a' - L'A'; } }
    std::wstring_view name(path.wszPath);
    // XInput devices also enumerate through DirectInput. Do not read them twice.
    if (name.find(L"IG_") != name.npos) { return DIENUM_CONTINUE; }
    if (enumeration.devices.count == enumeration.devices.ids.size()) {
        enumeration.devices.error = ERROR_MORE_DATA;
        return DIENUM_STOP;
    }
    enumeration.devices.ids[enumeration.devices.count++] = std::bit_cast<ControllerDeviceId>(instance->guidInstance);
    return DIENUM_CONTINUE;
}
class NativeDiscovery final : public ControllerDiscoverySource {
public:
    ~NativeDiscovery() override { if (input_) { input_->Release(); } }
    ControllerDevices query(const std::atomic<bool>& stop) override
    {
        if (stop.load()) { ControllerDevices cancelled; cancelled.error = ERROR_CANCELLED; return cancelled; }
        if (!input_) {
            const auto hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                reinterpret_cast<void**>(&input_), nullptr);
            if (FAILED(hr)) { ControllerDevices failed; failed.error = static_cast<DWORD>(hr); return failed; }
        }
        Enumeration enumeration;
        enumeration.input = input_;
        enumeration.stop = &stop;
        if (stop.load()) { enumeration.devices.error = ERROR_CANCELLED; return enumeration.devices; }
        const auto hr = input_->EnumDevices(DI8DEVCLASS_GAMECTRL, enumerate_device, &enumeration, DIEDFL_ATTACHEDONLY);
        if (FAILED(hr)) { enumeration.devices.error = static_cast<DWORD>(hr); }
        return enumeration.devices;
    }
private:
    IDirectInput8W* input_ = nullptr;
};
struct Objects {
    DirectInputLayout layout;
    bool valid = true;
};
BOOL CALLBACK enumerate_object(const DIDEVICEOBJECTINSTANCEW* object, void* context)
{
    auto& objects = *static_cast<Objects*>(context);
    if (!objects.layout.add(*object)) { objects.valid = false; return DIENUM_STOP; }
    return DIENUM_CONTINUE;
}
DWORD read_mapping_file(const std::filesystem::path& path, bool optional, std::string& bytes)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (optional && ec == std::errc::no_such_file_or_directory) { return 0; }
    if (ec) { return ERROR_READ_FAULT; }
    if (size > 2 * 1024 * 1024) { return ERROR_FILE_TOO_LARGE; }
    std::ifstream file(path, std::ios::binary);
    if (!file) { return ERROR_READ_FAULT; }
    bytes.resize(static_cast<std::size_t>(size));
    if (!file.read(bytes.data(), static_cast<std::streamsize>(size)) || file.peek() != std::char_traits<char>::eof()) {
        bytes.clear();
        return ERROR_READ_FAULT;
    }
    if (bytes.starts_with("\xef\xbb\xbf")) { bytes.erase(0, 3); }
    return 0;
}
}

NativeControllers::~NativeControllers()
{
    shutdown();
}
bool NativeControllers::shutdown()
{
    const bool stopped = !discovery_ || discovery_->stop();
    for (unsigned i = 0; i < slot_count; ++i) { close(i); }
    if (input_) { input_->Release(); input_ = nullptr; }
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
    return stopped;
}
DWORD NativeControllers::load_mappings()
{
    if (mappings_loaded_) { return mapping_error_; }
    const auto finish = [&](DWORD error) {
        mappings_loaded_ = true;
        mapping_error_ = error;
        return error;
    };
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length == executable.size()) { return finish(ERROR_BAD_PATHNAME); }
    const auto directory = std::filesystem::path(executable.data()).parent_path() / L"controllers";
    const auto builtin_error = read_mapping_file(directory / L"gamecontrollerdb.txt", false, database_);
    const auto user_error = read_mapping_file(directory / L"user-mappings.txt", true, overrides_);
    if (builtin_error) { return finish(builtin_error); }
    if (user_error) { return finish(user_error); }
    std::string_view remaining = overrides_;
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        auto line = remaining.substr(0, newline);
        remaining = newline == remaining.npos ? std::string_view{} : remaining.substr(newline + 1);
        if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
        if (line.empty() || line.front() == '#') { continue; }
        ControllerMapping parsed;
        if (!parse_controller_mapping(line, parsed).empty()) { return finish(ERROR_INVALID_DATA); }
    }
    return finish(0);
}
ControllerDevices NativeControllers::discover()
{
    if (!discovery_) { discovery_ = std::make_unique<ControllerDiscovery>(std::make_unique<NativeDiscovery>()); }
    return discovery_->latest();
}
std::uint32_t NativeControllers::open(unsigned slot, const ControllerDeviceId& id)
{
    close(slot);
    auto& device = devices_[slot];
    device.description.fill(0);
    if (!input_) {
        const auto created = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
            reinterpret_cast<void**>(&input_), nullptr);
        if (FAILED(created)) { return static_cast<DWORD>(created); }
    }
    auto hr = input_->CreateDevice(std::bit_cast<GUID>(id), &device.handle, nullptr);
    if (FAILED(hr)) { return static_cast<DWORD>(hr); }
    DIPROPDWORD product{};
    product.diph = {sizeof(product), sizeof(product.diph), 0, DIPH_DEVICE};
    hr = device.handle->GetProperty(DIPROP_VIDPID, &product.diph);
    if (FAILED(hr)) { return static_cast<DWORD>(hr); }
    DIDEVICEINSTANCEW instance{};
    instance.dwSize = sizeof(instance);
    hr = device.handle->GetDeviceInfo(&instance);
    if (FAILED(hr)) { return static_cast<DWORD>(hr); }
    std::array<char, MAX_PATH * 4> name{};
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, instance.tszProductName, -1,
        name.data(), static_cast<int>(name.size()), nullptr, nullptr)) { return ERROR_INVALID_DATA; }
    const auto guid = directinput_mapping_guid(static_cast<std::uint16_t>(product.dwData),
        static_cast<std::uint16_t>(product.dwData >> 16), name.data());
    const auto crc = controller_name_crc(name.data());
    Objects objects;
    hr = device.handle->EnumObjects(enumerate_object, &objects, DIDFT_ALL);
    if (FAILED(hr)) { return static_cast<DWORD>(hr); }
    if (!objects.valid) {
        wcscpy_s(device.description.data(), device.description.size(), L"HID object layout unsupported or exceeds control limits.");
        return ERROR_NOT_SUPPORTED;
    }
    objects.layout.order();
    for (unsigned i = 0; i < objects.layout.controls.axes; ++i) {
        const auto& axis = objects.layout.axes[i];
        DIPROPRANGE range{};
        range.diph = {sizeof(range), sizeof(range.diph), axis.type, DIPH_BYID};
        range.lMin = -32768; range.lMax = 32767;
        hr = device.handle->SetProperty(DIPROP_RANGE, &range.diph);
        if (FAILED(hr)) { return static_cast<DWORD>(hr); }
        DIPROPDWORD deadzone{};
        deadzone.diph = {sizeof(deadzone), sizeof(deadzone.diph), axis.type, DIPH_BYID};
        hr = device.handle->SetProperty(DIPROP_DEADZONE, &deadzone.diph);
        if (FAILED(hr)) { return static_cast<DWORD>(hr); }
    }
    device.layout = objects.layout;
    const std::wstring wide_guid(guid.begin(), guid.end());
    swprintf_s(device.description.data(), device.description.size(), L"HID %ls; axes %u, buttons %u, hats %u",
        wide_guid.c_str(), device.layout.controls.axes, device.layout.controls.buttons, device.layout.controls.hats);
    if (const auto error = load_mappings()) { return error; }
    auto mapping = find_controller_mapping(overrides_, guid, crc);
    if (mapping.empty()) { mapping = find_controller_mapping(database_, guid, crc); }
    if (mapping.empty() || !parse_controller_mapping(mapping, device.mapping).empty()
        || !mapping_fits(device.mapping, device.layout.controls)) { return ERROR_NOT_SUPPORTED; }
    hr = device.handle->SetDataFormat(&c_dfDIJoystick2);
    if (FAILED(hr)) { return static_cast<DWORD>(hr); }
    if (!window_) {
        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"Controller reader",
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) { return GetLastError(); }
    }
    hr = device.handle->SetCooperativeLevel(window_, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (FAILED(hr)) { return static_cast<DWORD>(hr); }
    hr = device.handle->Acquire();
    return FAILED(hr) ? static_cast<DWORD>(hr) : 0;
}
Controller NativeControllers::sample(unsigned slot)
{
    return sample(slot, {});
}
Controller NativeControllers::sample(unsigned slot, ControllerReadScope scope)
{
    Controller failed;
    failed.connection = Connection::Error;
    auto& device = devices_[slot];
    if (!device.handle) { failed.error = ERROR_INVALID_HANDLE; return failed; }
    if (!scope.allowed()) { failed.error = ERROR_CANCELLED; return failed; }
    auto hr = device.handle->Poll();
    if (FAILED(hr)) { failed.error = static_cast<DWORD>(hr); return failed; }
    DIJOYSTATE2 report{};
    std::fill(std::begin(report.rgdwPOV), std::end(report.rgdwPOV), 0xffffffff);
    if (!scope.allowed()) { failed.error = ERROR_CANCELLED; return failed; }
    hr = device.handle->GetDeviceState(sizeof(report), &report);
    if (FAILED(hr)) { failed.error = static_cast<DWORD>(hr); return failed; }
    RawController raw;
    if (!directinput_report(device.layout, report, raw)) { failed.error = ERROR_INVALID_DATA; return failed; }
    auto result = mapped_controller(device.mapping, raw);
    result.packet = ++device.packet;
    return result;
}
void NativeControllers::close(unsigned slot)
{
    auto& handle = devices_[slot].handle;
    if (handle) { handle->Unacquire(); handle->Release(); handle = nullptr; }
}
void NativeControllers::describe(unsigned slot, Controller& value) const
{
    value.description = devices_[slot].description;
}
void NativeControllers::service_messages()
{
    if (!window_) { return; }
    MSG message{};
    for (unsigned i = 0; i < 32 && PeekMessageW(&message, window_, 0, 0, PM_REMOVE); ++i) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
} // namespace gamepad
