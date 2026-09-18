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
};
BOOL CALLBACK enumerate_device(const DIDEVICEINSTANCEW* instance, void* context)
{
    auto& enumeration = *static_cast<Enumeration*>(context);
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
struct NativeReport {
    std::array<LONG, controller_axes> axes{};
    std::array<BYTE, controller_buttons> buttons{};
    std::array<DWORD, controller_hats> hats{};
};
struct Objects {
    std::array<DIDEVICEOBJECTINSTANCEW, controller_axes> axes{};
    std::array<bool, controller_axes> present{};
    std::array<DIOBJECTDATAFORMAT, controller_axes + controller_buttons + controller_hats> format{};
    ControllerLayout layout;
    bool valid = true;
};
BOOL CALLBACK enumerate_object(const DIDEVICEOBJECTINSTANCEW* object, void* context)
{
    auto& objects = *static_cast<Objects*>(context);
    if (object->dwType & DIDFT_AXIS) {
        if (!(object->dwType & DIDFT_ABSAXIS)) { objects.valid = false; return DIENUM_STOP; }
        const std::array<const GUID*, 6> types{&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis};
        unsigned index = controller_axes;
        for (unsigned i = 0; i < types.size(); ++i) { if (object->guidType == *types[i]) { index = i; break; } }
        if (object->guidType == GUID_Slider) { index = objects.present[6] ? 7 : 6; }
        if (index == controller_axes || objects.present[index]) { objects.valid = false; return DIENUM_STOP; }
        objects.axes[index] = *object;
        objects.present[index] = true;
    } else if (object->dwType & DIDFT_BUTTON) {
        if (objects.layout.buttons == controller_buttons) { objects.valid = false; return DIENUM_STOP; }
        const auto index = objects.layout.buttons++;
        objects.format[controller_axes + index] = {&GUID_Button,
            static_cast<DWORD>(offsetof(NativeReport, buttons)) + index, object->dwType, 0};
    } else if (object->dwType & DIDFT_POV) {
        if (objects.layout.hats == controller_hats) { objects.valid = false; return DIENUM_STOP; }
        const auto index = objects.layout.hats++;
        objects.format[controller_axes + controller_buttons + index] = {&GUID_POV,
            static_cast<DWORD>(offsetof(NativeReport, hats) + index * sizeof(DWORD)), object->dwType, 0};
    }
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
    for (unsigned i = 0; i < slot_count; ++i) { close(i); }
    if (input_) { input_->Release(); }
    if (window_) { DestroyWindow(window_); }
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
    if (!input_) {
        const auto hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
            reinterpret_cast<void**>(&input_), nullptr);
        if (FAILED(hr)) { ControllerDevices failed; failed.error = static_cast<DWORD>(hr); return failed; }
    }
    Enumeration enumeration;
    enumeration.input = input_;
    const auto hr = input_->EnumDevices(DI8DEVCLASS_GAMECTRL, enumerate_device, &enumeration, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr)) { enumeration.devices.error = static_cast<DWORD>(hr); }
    return enumeration.devices;
}
std::uint32_t NativeControllers::open(unsigned slot, const ControllerDeviceId& id)
{
    close(slot);
    auto& device = devices_[slot];
    device.description.fill(0);
    if (!input_) { return ERROR_INVALID_HANDLE; }
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
    if (!objects.valid) { return ERROR_NOT_SUPPORTED; }
    std::array<DIOBJECTDATAFORMAT, controller_axes + controller_buttons + controller_hats> format{};
    unsigned count = 0;
    for (unsigned i = 0; i < controller_axes; ++i) {
        if (!objects.present[i]) { continue; }
        auto& axis = objects.axes[i];
        DIPROPRANGE range{};
        range.diph = {sizeof(range), sizeof(range.diph), axis.dwType, DIPH_BYID};
        range.lMin = -32768; range.lMax = 32767;
        hr = device.handle->SetProperty(DIPROP_RANGE, &range.diph);
        if (FAILED(hr)) { return static_cast<DWORD>(hr); }
        DIPROPDWORD deadzone{};
        deadzone.diph = {sizeof(deadzone), sizeof(deadzone.diph), axis.dwType, DIPH_BYID};
        hr = device.handle->SetProperty(DIPROP_DEADZONE, &deadzone.diph);
        if (FAILED(hr)) { return static_cast<DWORD>(hr); }
        format[count++] = {&axis.guidType, static_cast<DWORD>(offsetof(NativeReport, axes)
            + objects.layout.axes++ * sizeof(LONG)), axis.dwType, 0};
    }
    for (unsigned i = 0; i < objects.layout.buttons; ++i) { format[count++] = objects.format[controller_axes + i]; }
    for (unsigned i = 0; i < objects.layout.hats; ++i) { format[count++] = objects.format[controller_axes + controller_buttons + i]; }
    device.layout = objects.layout;
    const std::wstring wide_guid(guid.begin(), guid.end());
    swprintf_s(device.description.data(), device.description.size(), L"HID %ls; axes %u, buttons %u, hats %u",
        wide_guid.c_str(), device.layout.axes, device.layout.buttons, device.layout.hats);
    if (const auto error = load_mappings()) { return error; }
    auto mapping = find_controller_mapping(overrides_, guid, crc);
    if (mapping.empty()) { mapping = find_controller_mapping(database_, guid, crc); }
    if (mapping.empty() || !parse_controller_mapping(mapping, device.mapping).empty()
        || !mapping_fits(device.mapping, device.layout)) { return ERROR_NOT_SUPPORTED; }
    DIDATAFORMAT data{sizeof(data), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(NativeReport), count, format.data()};
    hr = device.handle->SetDataFormat(&data);
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
    NativeReport report;
    report.hats.fill(0xffffffff);
    if (!scope.allowed()) { failed.error = ERROR_CANCELLED; return failed; }
    hr = device.handle->GetDeviceState(sizeof(report), &report);
    if (FAILED(hr)) { failed.error = static_cast<DWORD>(hr); return failed; }
    RawController raw;
    for (unsigned i = 0; i < device.layout.axes; ++i) {
        if (report.axes[i] < -32768 || report.axes[i] > 32767) { failed.error = ERROR_INVALID_DATA; return failed; }
        raw.axes[i] = static_cast<std::int16_t>(report.axes[i]);
    }
    for (unsigned i = 0; i < device.layout.buttons; ++i) { raw.buttons[i] = (report.buttons[i] & 0x80) != 0; }
    for (unsigned i = 0; i < device.layout.hats; ++i) {
        const auto angle = report.hats[i];
        if ((angle & 0xffff) == 0xffff) { continue; }
        if (angle >= 36000) { failed.error = ERROR_INVALID_DATA; return failed; }
        constexpr std::array<std::uint8_t, 8> hats{1, 3, 2, 6, 4, 12, 8, 9};
        raw.hats[i] = hats[((angle + 2250) % 36000) / 4500];
    }
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
