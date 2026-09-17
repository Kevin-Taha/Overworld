#include "Settings.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace gamepad {
namespace {
constexpr std::array<std::wstring_view, 24> keys{
    L"version", L"speed", L"precision", L"scroll", L"left_exit", L"left_enter",
    L"right_exit", L"right_enter", L"ring", L"ring_diameter", L"cursor_style", L"replace_cursor",
    L"motion_profile", L"link_hint", L"band_low", L"band_middle", L"band_high", L"automatic_game_switching",
    L"keyboard_input", L"keyboard_in_native_mode", L"experimental_navigation_filter", L"voice_enabled", L"voice_model_path",
    L"word_suggestions"};

bool number(std::wstring_view text, double& result)
{
    if (text.empty() || text.size() > 64) { return false; }
    std::string narrow;
    for (const auto c : text) {
        if (c > 127 || c <= L' ') { return false; }
        narrow += static_cast<char>(c);
    }
    double value = 0;
    const auto parsed = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != narrow.data() + narrow.size() || !std::isfinite(value)) {
        return false;
    }
    result = value;
    return true;
}

std::wstring decimal(double value)
{
    char buffer[64]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    return std::wstring(buffer, result.ptr);
}

bool integer(std::wstring_view text, int& result)
{
    double value = 0;
    if (!number(text, value) || value < 20 || value > 96 || std::floor(value) != value) { return false; }
    result = static_cast<int>(value);
    return true;
}

std::wstring split_exclusions(std::wstring_view text, live::ExecutableExclusions& names)
{
    if (text.size() > exclusion_text_limit) { return L"Exclusion list is too long (maximum 128 executable names)."; }
    while (!text.empty()) {
        const auto end = text.find(L'\n');
        auto line = text.substr(0, end);
        if (!line.empty() && line.back() == L'\r') { line.remove_suffix(1); }
        if (!line.empty()) { names.emplace_back(line); }
        if (end == text.npos) { break; }
        text.remove_prefix(end + 1);
    }
    return std::wstring{live::validate_exclusions(names)};
}

std::wstring os_error(std::wstring_view operation, DWORD code = GetLastError())
{
    return std::wstring{operation} + L" (Windows error " + std::to_wstring(code) + L").";
}

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) { CloseHandle(value_); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value_; }
private:
    HANDLE value_;
};

SettingsFile read_file(const std::filesystem::path& path)
{
    SettingsFile result;
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            result.state = LoadState::Missing;
        } else {
            result.error = os_error(L"Cannot read settings", error);
        }
        return result;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) { result.error = os_error(L"Cannot read settings size"); return result; }
    if (size.QuadPart < 0 || size.QuadPart > settings_byte_limit) {
        result.state = LoadState::Invalid;
        result.error = L"Settings file exceeds the 128 KiB limit.";
        return result;
    }
    result.bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file, result.bytes.data(), static_cast<DWORD>(result.bytes.size()), &read, nullptr)
        || read != result.bytes.size()) {
        result.error = os_error(L"Cannot read complete settings");
        return result;
    }
    result.state = LoadState::Loaded;
    return result;
}
} // namespace

bool settings_number(std::wstring_view text, double& result) { return number(text, result); }

std::wstring validate_settings(const Settings& settings)
{
    if (const auto error = live::validate_pointer_settings(settings.pointer); !error.empty()) { return std::wstring{error}; }
    if (const auto error = live::validate_exclusions(settings.exclusions); !error.empty()) { return std::wstring{error}; }
    if (settings.ring_diameter < 20 || settings.ring_diameter > 96) { return L"Cursor size must be 20-96 DIPs."; }
    if (!valid_cursor_style(settings.cursor_style)) { return L"Unknown cursor style."; }
    if (settings.voice_model_path.size() > voice_model_path_limit) { return L"Voice model path must be at most 1024 characters."; }
    for (std::size_t i = 0; i < settings.voice_model_path.size(); ++i) {
        const wchar_t value = settings.voice_model_path[i];
        if (value < 32 || (value >= 0x7F && value <= 0x9F)) { return L"Voice model path cannot contain control characters."; }
        if (value >= 0xD800 && value <= 0xDBFF) {
            if (++i == settings.voice_model_path.size() || settings.voice_model_path[i] < 0xDC00
                || settings.voice_model_path[i] > 0xDFFF) { return L"Voice model path must contain valid Unicode."; }
        } else if (value >= 0xDC00 && value <= 0xDFFF) { return L"Voice model path must contain valid Unicode."; }
    }
    return {};
}

std::wstring parse_settings(std::wstring_view text, Settings& result)
{
    if (text.size() > settings_byte_limit || text.find(L'\0') != text.npos) { return L"Invalid settings text size or NUL character."; }
    Settings parsed;
    int version = 0;
    std::array<bool, keys.size()> seen{};
    while (!text.empty()) {
        const auto end = text.find(L'\n');
        auto line = text.substr(0, end);
        if (!line.empty() && line.back() == L'\r') { line.remove_suffix(1); }
        if (!line.empty()) {
            const auto equal = line.find(L'=');
            if (equal == line.npos) { return L"Expected a settings key=value line."; }
            const auto key = line.substr(0, equal);
            const auto value = line.substr(equal + 1);
            if (key == L"exclude") {
                parsed.exclusions.emplace_back(value);
                if (parsed.exclusions.size() > 128) { return L"Use at most 128 executable basenames."; }
            } else {
                const auto found = std::find(keys.begin(), keys.end(), key);
                if (found == keys.end()) { return L"Unknown settings key; file was not loaded."; }
                const auto index = static_cast<std::size_t>(found - keys.begin());
                if (seen[index]) { return L"Duplicate settings key; file was not loaded."; }
                seen[index] = true;
                if (index == 0) {
                    if (value == L"12") { version = 12; }
                    else if (value == L"11") { version = 11; }
                    else if (value == L"10") { version = 10; }
                    else if (value.size() == 1 && value.front() >= L'1' && value.front() <= L'9') {
                        version = value.front() - L'0';
                    } else {
                        return L"Unsupported settings version (expected 1 through 12).";
                    }
                } else if (index == 8) {
                    if (value != L"0" && value != L"1") { return L"ring must be 0 or 1."; }
                } else if (index == 9) {
                    if (!integer(value, parsed.ring_diameter)) { return L"Cursor size must be a whole number, 20-96 DIPs."; }
                } else if (index == 10) {
                    const auto choice = std::find_if(cursor_styles.begin(), cursor_styles.end(),
                        [value](const auto& item) { return item.key == value; });
                    if (choice == cursor_styles.end()) { return L"Unknown cursor_style; expected ring, dot, brackets or arrow."; }
                    parsed.cursor_style = choice->style;
                } else if (index == 11) {
                    if (value != L"0" && value != L"1") { return L"replace_cursor must be 0 or 1."; }
                } else if (index == 12) {
                    if (value != L"smooth" && value != L"speed_bands") { return L"motion_profile must be smooth or speed_bands."; }
                    parsed.pointer.profile = value == L"smooth" ? live::MotionProfile::Smooth : live::MotionProfile::SpeedBands;
                } else if (index == 13) {
                    // Required legacy v4 syntax, superseded by the removal of hand-cursor hints.
                    if (value != L"0" && value != L"1") { return L"link_hint must be 0 or 1."; }
                } else if (index == 17) {
                    if (value != L"0" && value != L"1") { return L"automatic_game_switching must be 0 or 1."; }
                    parsed.automatic_game_switching = value == L"1";
                } else if (index == 18) {
                    if (value != L"face_buttons" && value != L"one_handed") {
                        return L"keyboard_input must be face_buttons or one_handed.";
                    }
                    parsed.keyboard_input_mode = value == L"one_handed"
                        ? KeyboardInputMode::OneHanded : KeyboardInputMode::FaceButtons;
                } else if (index == 19) {
                    if (value != L"0" && value != L"1") { return L"keyboard_in_native_mode must be 0 or 1."; }
                    parsed.keyboard_in_native_mode = value == L"1";
                } else if (index == 20) {
                    if (value != L"0" && value != L"1") { return L"experimental_navigation_filter must be 0 or 1."; }
                    parsed.experimental_navigation_filter = value == L"1";
                } else if (index == 21) {
                    if (value != L"0" && value != L"1") { return L"voice_enabled must be 0 or 1."; }
                    parsed.voice_enabled = value == L"1";
                } else if (index == 22) {
                    parsed.voice_model_path = value;
                } else if (index == 23) {
                    if (value != L"0" && value != L"1") { return L"word_suggestions must be 0 or 1."; }
                    parsed.word_suggestions = value == L"1";
                } else if (index >= 14 && index <= 16) {
                    if (!number(value, parsed.pointer.band_rates[index - 14])) {
                        return L"Band speeds must be complete finite numbers.";
                    }
                } else {
                    const std::array<double*, 7> destinations{&parsed.pointer.speed, &parsed.pointer.precision,
                        &parsed.pointer.scroll, &parsed.pointer.left_exit, &parsed.pointer.left_enter,
                        &parsed.pointer.right_exit, &parsed.pointer.right_enter};
                    if (!number(value, *destinations[index - 1])) { return L"Settings contain an invalid or non-finite number."; }
                }
            }
        }
        if (end == text.npos) { break; }
        text.remove_prefix(end + 1);
    }
    for (std::size_t index = 0; index < seen.size(); ++index) {
        const bool required = index < 8 || index == 9 || (index == 8 && version < 3)
            || (index == 10 && version >= 2) || (index == 11 && version == 2)
            || (index == 12 && version >= 4) || (index == 13 && version == 4)
            || (index >= 14 && index <= 16 && version >= 6) || (index == 17 && version >= 7)
            || (index == 18 && version >= 8) || (index == 19 && version >= 9) || (index == 20 && version >= 10)
            || (index >= 21 && index <= 22 && version >= 11) || (index == 23 && version >= 12);
        if (required && !seen[index]) { return L"Settings file is incomplete; required key missing."; }
        if (!required && seen[index]) { return L"Settings key is not supported in this version."; }
    }
    if (version == 1) { parsed.cursor_style = CursorStyle::Ring; } // Preserve the only style available in v1.
    if (auto error = validate_settings(parsed); !error.empty()) { return error; }
    result = std::move(parsed);
    return {};
}

std::wstring serialize_settings(const Settings& settings)
{
    const auto& p = settings.pointer;
    const std::array<double, 7> values{p.speed, p.precision, p.scroll, p.left_exit, p.left_enter, p.right_exit, p.right_enter};
    std::wstring text = L"version=12\r\n";
    for (std::size_t i = 0; i < values.size(); ++i) { text += std::wstring{keys[i + 1]} + L"=" + decimal(values[i]) + L"\r\n"; }
    text += L"ring_diameter=" + std::to_wstring(settings.ring_diameter) + L"\r\n";
    for (const auto& choice : cursor_styles) {
        if (choice.style == settings.cursor_style) { text += L"cursor_style=" + std::wstring{choice.key} + L"\r\n"; }
    }
    text += p.profile == live::MotionProfile::Smooth ? L"motion_profile=smooth\r\n" : L"motion_profile=speed_bands\r\n";
    for (std::size_t i = 0; i < p.band_rates.size(); ++i) {
        text += std::wstring{keys[14 + i]} + L"=" + decimal(p.band_rates[i]) + L"\r\n";
    }
    text += settings.automatic_game_switching ? L"automatic_game_switching=1\r\n" : L"automatic_game_switching=0\r\n";
    text += settings.keyboard_input_mode == KeyboardInputMode::OneHanded
        ? L"keyboard_input=one_handed\r\n" : L"keyboard_input=face_buttons\r\n";
    text += settings.keyboard_in_native_mode ? L"keyboard_in_native_mode=1\r\n" : L"keyboard_in_native_mode=0\r\n";
    text += settings.experimental_navigation_filter ? L"experimental_navigation_filter=1\r\n" : L"experimental_navigation_filter=0\r\n";
    text += settings.voice_enabled ? L"voice_enabled=1\r\n" : L"voice_enabled=0\r\n";
    text += L"voice_model_path=" + settings.voice_model_path + L"\r\n";
    text += settings.word_suggestions ? L"word_suggestions=1\r\n" : L"word_suggestions=0\r\n";
    for (const auto& name : settings.exclusions) { text += L"exclude=" + name + L"\r\n"; }
    return text;
}

SettingsFields settings_fields(const Settings& settings)
{
    const auto& p = settings.pointer;
    // Shortest round-trip decimal keeps the normal defaults readable (25, 20, 24).
    const auto display = [](double value) {
        char buffer[64]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::wstring(buffer, result.ptr);
    };
    return {display(p.speed), display(p.precision * 100), display(p.scroll),
        display(p.left_exit * 100), display(p.left_enter * 100),
        display(p.right_exit * 100), display(p.right_enter * 100), std::to_wstring(settings.ring_diameter),
        display(p.band_rates[0] * 100), display(p.band_rates[1] * 100), display(p.band_rates[2] * 100)};
}

std::wstring exclusion_text(const Settings& settings)
{
    std::wstring text;
    for (const auto& name : settings.exclusions) { text += name + L"\r\n"; }
    return text;
}

std::wstring parse_settings_fields(const SettingsFields& fields,
    std::wstring_view exclusions, const Settings& original, Settings& result)
{
    Settings parsed = original;
    const auto previous = settings_fields(original);
    const std::array<double*, 7> destinations{&parsed.pointer.speed, &parsed.pointer.precision, &parsed.pointer.scroll,
        &parsed.pointer.left_exit, &parsed.pointer.left_enter, &parsed.pointer.right_exit, &parsed.pointer.right_enter};
    const std::array<std::wstring_view, 7> labels{L"Pointer speed", L"Precision", L"Scroll speed", L"Left stop",
        L"Left start", L"Right stop", L"Right start"};
    for (std::size_t i = 0; i < destinations.size(); ++i) {
        double value = 0;
        if (!number(fields[i], value)) { return std::wstring{labels[i]} + L": enter a complete finite number (decimal point, no spaces)."; }
        // Retain the exact underlying double when an unchanged percentage is applied.
        if (fields[i] != previous[i]) { *destinations[i] = (i == 0 || i == 2) ? value : value / 100; }
    }
    if (!integer(fields[7], parsed.ring_diameter)) { return L"Cursor size must be a whole number, 20-96 DIPs."; }
    for (std::size_t i = 0; i < parsed.pointer.band_rates.size(); ++i) {
        double value = 0;
        if (!number(fields[8 + i], value)) { return L"Band speeds: enter complete finite percentages (1-100)."; }
        if (fields[8 + i] != previous[8 + i]) { parsed.pointer.band_rates[i] = value / 100; }
    }
    parsed.exclusions.clear();
    if (auto error = split_exclusions(exclusions, parsed.exclusions); !error.empty()) { return error; }
    if (auto error = validate_settings(parsed); !error.empty()) { return error; }
    result = std::move(parsed);
    return {};
}

std::wstring settings_path(std::filesystem::path& result)
{
    PWSTR folder = nullptr;
    const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &folder);
    if (FAILED(hr)) { return L"Cannot locate LocalAppData (HRESULT " + std::to_wstring(hr) + L")."; }
    result = std::filesystem::path{folder} / L"GamepadDesktop" / L"settings.ini";
    CoTaskMemFree(folder);
    return {};
}

SettingsFile load_settings(const std::filesystem::path& path)
{
    auto result = read_file(path);
    if (result.state != LoadState::Loaded) { return result; }
    auto bytes = std::string_view{result.bytes};
    if (bytes.starts_with("\xef\xbb\xbf")) { bytes.remove_prefix(3); }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!length) {
        result.state = LoadState::Invalid;
        result.error = L"Settings must be a nonempty UTF-8 file.";
        return result;
    }
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), length)) {
        result.state = LoadState::Invalid;
        result.error = os_error(L"Cannot decode settings");
        return result;
    }
    result.error = parse_settings(text, result.settings);
    if (!result.error.empty()) { result.state = LoadState::Invalid; }
    return result;
}

std::wstring save_settings(const std::filesystem::path& path, const Settings& settings, SettingsFile& baseline)
{
    if (auto error = validate_settings(settings); !error.empty()) { return error; }
    if (baseline.state != LoadState::Loaded && baseline.state != LoadState::Missing) {
        return L"Original settings were not readable/valid. Repair or move the file, then Reload file before saving.";
    }
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) { return L"Cannot create settings directory (error " + std::to_wstring(directory_error.value()) + L")."; }
    const auto temporary = std::filesystem::path{path.native() + L".tmp"};
    Handle lock{CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
    if (lock == INVALID_HANDLE_VALUE) { return os_error(L"Cannot create settings.ini.tmp; remove a stale temp file only when no instance is saving"); }

    std::wstring error;
    const auto current = read_file(path);
    if (current.state != baseline.state || (current.state == LoadState::Loaded && current.bytes != baseline.bytes)) {
        error = L"Settings changed externally or became unreadable. Reload file before saving; nothing was replaced.";
    }
    const auto text = serialize_settings(settings);
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>((std::max)(count, 0)), '\0');
    if (error.empty() && (!count || bytes.size() > settings_byte_limit
        || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            bytes.data(), count, nullptr, nullptr))) {
        error = L"Settings could not be encoded as bounded UTF-8.";
    }
    DWORD written = 0;
    if (error.empty() && (!WriteFile(lock, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        || written != bytes.size() || !FlushFileBuffers(lock))) { error = os_error(L"Cannot write complete settings"); }
    if (!error.empty()) {
        // FILE_DISPOSITION_INFO removes only this newly-created file when its handle closes.
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(lock, FileDispositionInfo, &disposition, sizeof(disposition))) {
            error += L" Temporary file remains; remove settings.ini.tmp before retrying.";
        }
        return error;
    }
    // Rename via the handle keeps the temporary file exclusively owned through publication.
    const auto destination = std::filesystem::absolute(path).native();
    const auto name_bytes = destination.size() * sizeof(wchar_t);
    std::vector<std::byte> rename_buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(rename_buffer.data());
    rename->ReplaceIfExists = baseline.state == LoadState::Loaded;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    memcpy(rename->FileName, destination.data(), name_bytes);
    if (!SetFileInformationByHandle(lock, FileRenameInfo, rename, static_cast<DWORD>(rename_buffer.size()))) {
        error = os_error(L"Cannot atomically replace settings");
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(lock, FileDispositionInfo, &disposition, sizeof(disposition))) {
            error += L" Temporary file remains; remove settings.ini.tmp before retrying.";
        }
        return error;
    }
    baseline = {LoadState::Loaded, settings, std::move(bytes), {}};
    return {};
}
} // namespace gamepad
