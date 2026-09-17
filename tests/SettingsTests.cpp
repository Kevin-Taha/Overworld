#include "Settings.h"
#include "RingPixels.h"

#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace gamepad;
namespace {
void check(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

struct Scratch {
    std::filesystem::path directory;
    std::filesystem::path file;
    explicit Scratch(const std::filesystem::path& root)
    {
        directory = root / std::to_wstring(GetCurrentProcessId());
        check(std::filesystem::create_directory(directory), "unique scratch directory creation");
        file = directory / L"settings.ini";
    }
    ~Scratch()
    {
        SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove(file);
        std::filesystem::remove(std::filesystem::path{file.native() + L".tmp"});
        std::filesystem::remove(directory);
    }
    void write(std::string_view bytes) const
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        check(stream.good(), "scratch file write");
    }
};

std::wstring changed(std::wstring text, std::wstring_view key, std::wstring_view value)
{
    const auto begin = text.find(std::wstring{key} + L"=");
    check(begin != text.npos, "test key exists");
    const auto end = text.find(L'\r', begin);
    text.replace(begin, end - begin, std::wstring{key} + L"=" + std::wstring{value});
    return text;
}

Settings unusual()
{
    Settings value;
    value.pointer = {1234.56789, .3141592653589793, 765.4321, .123456789012345, .33333333333333, .29, .47};
    value.exclusions = {L"Game.exe", L"\u6e38\u620f.EXE"};
    value.cursor_style = CursorStyle::Ring;
    value.ring_diameter = 67;
    return value;
}

std::wstring without(std::wstring text, std::wstring_view key)
{
    const auto begin = text.find(std::wstring{key} + L"=");
    check(begin != text.npos, "fixture key exists");
    text.erase(begin, text.find(L'\n', begin) + 1 - begin);
    return text;
}

std::wstring version_eleven(const Settings& settings)
{
    return changed(without(serialize_settings(settings), L"word_suggestions"), L"version", L"11");
}

std::wstring version_ten(const Settings& settings)
{
    return changed(without(without(version_eleven(settings), L"voice_enabled"), L"voice_model_path"), L"version", L"10");
}

std::wstring version_six(const Settings& settings)
{
    return without(without(without(without(changed(version_ten(settings), L"version", L"6"), L"automatic_game_switching"), L"keyboard_input"), L"keyboard_in_native_mode"), L"experimental_navigation_filter");
}

std::wstring version_seven(const Settings& settings)
{
    return without(without(without(changed(version_ten(settings), L"version", L"7"), L"keyboard_input"), L"keyboard_in_native_mode"), L"experimental_navigation_filter");
}

std::wstring version_eight(const Settings& settings)
{
    return without(without(changed(version_ten(settings), L"version", L"8"), L"keyboard_in_native_mode"), L"experimental_navigation_filter");
}

std::wstring version_nine(const Settings& settings)
{
    return without(changed(version_ten(settings), L"version", L"9"), L"experimental_navigation_filter");
}

std::wstring version_five(const Settings& settings)
{
    auto text = changed(version_six(settings), L"version", L"5");
    for (const auto key : {L"band_low", L"band_middle", L"band_high"}) { text = without(text, key); }
    return text;
}

std::wstring version_three(const Settings& settings)
{
    return without(changed(version_five(settings), L"version", L"3"), L"motion_profile");
}

std::wstring version_four(const Settings& settings, bool hint)
{
    return changed(version_five(settings), L"version", L"4") + (hint ? L"link_hint=1\r\n" : L"link_hint=0\r\n");
}

std::wstring version_one(const Settings& settings)
{
    auto text = changed(version_three(settings), L"version", L"1");
    const auto begin = text.find(L"cursor_style=");
    const auto end = text.find(L'\n', begin) + 1;
    text.erase(begin, end - begin);
    return text + L"ring=1\r\n";
}

std::wstring version_two(const Settings& settings, bool replace, bool visible)
{
    return changed(version_three(settings), L"version", L"2")
        + L"ring=" + (visible ? L"1" : L"0") + L"\r\nreplace_cursor=" + (replace ? L"1" : L"0") + L"\r\n";
}

std::string utf8(std::wstring_view text)
{
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    check(size > 0, "fixture utf8 size");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        bytes.data(), size, nullptr, nullptr) == size, "fixture utf8 encode");
    return bytes;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) { std::cerr << "Supply the CMake test scratch directory.\n"; return 2; }
    const std::filesystem::path root{argv[1]};
    std::filesystem::create_directories(root);
    unsigned count = 0;
    const auto test = [&](const char* name, const std::function<void()>& body) {
        body();
        ++count;
        std::cout << "PASS " << name << '\n';
    };
    try {
        test("exact product defaults", [] {
            const Settings s;
            check(s.pointer == live::PointerSettings{}, "pointer defaults preserved");
            check(s.word_suggestions && !s.voice_enabled, "word suggestions default on; voice remains opt-in");
            check(s.ring_diameter == 28 && s.exclusions.empty(), "compact ring/exclusion defaults");
            check(s.cursor_style == CursorStyle::Arrow && s.pointer.profile == live::MotionProfile::Smooth, "modern pointer and Smooth defaults");
            check(settings_fields(s) == SettingsFields{L"1100", L"25", L"960", L"20", L"24", L"23", L"27", L"28",
                L"10", L"35", L"100"}, "friendly default units");
        });
        test("versioned text lossless round trip", [] {
            for (const auto& s : {Settings{}, unusual()}) {
                Settings result;
                check(parse_settings(serialize_settings(s), result).empty() && result == s, "exact double/unicode round trip");
            }
        });
        test("schema strictness preserves destination", [] {
            const auto text = serialize_settings({});
            for (const auto& bad : {std::wstring{}, changed(text, L"version", L"13"), changed(text, L"version", L"1.0"),
                text + L"version=1\n", text + L"enabled=1\n", text + L"slot=0\n", text.substr(text.find(L'\n') + 1),
                text + L"broken line\n", text + L"ring=true\n", text + std::wstring(1, L'\0')}) {
                Settings output = unusual();
                check(!parse_settings(bad, output).empty() && output == unusual(), "reject malformed schema without partial changes");
            }
        });
        test("full finite numeric parser", [] {
            for (const auto bad : {L"", L"nan", L"NaN", L"inf", L"-infinity", L"1100px", L"1e9999", L"1,100", L" 1100", L"1100 ", L"0x44", L"1\n2"}) {
                Settings result = unusual();
                check(!parse_settings(changed(serialize_settings({}), L"speed", bad), result).empty(), "bad numeric value rejected");
                check(result == unusual(), "old config retained");
            }
        });
        test("validated ranges and hysteresis", [] {
            const std::array<std::pair<std::wstring_view, std::wstring_view>, 12> values{{
                {L"speed", L"99"}, {L"speed", L"5001"}, {L"precision", L"0.01"}, {L"precision", L"1.1"},
                {L"scroll", L"119"}, {L"scroll", L"4801"}, {L"left_exit", L"0.24"}, {L"left_enter", L"0.2"},
                {L"right_exit", L"-0.01"}, {L"right_enter", L"0.951"}, {L"ring_diameter", L"19"}, {L"ring_diameter", L"40.5"}}};
            for (const auto& [key, value] : values) {
                Settings output;
                check(!parse_settings(changed(serialize_settings({}), key, value), output).empty(), "range rejected");
            }
        });
        test("valid edge ranges", [] {
            Settings s;
            s.pointer = {100, .05, 120, 0, .01, .90, .95};
            s.ring_diameter = 20;
            check(validate_settings(s).empty(), "lower bounds");
            s.pointer.speed = 5000; s.pointer.precision = 1; s.pointer.scroll = 4800; s.ring_diameter = 96;
            check(validate_settings(s).empty(), "upper bounds");
        });
        test("Stage A exclusion validation reused", [] {
            for (const auto name : {L" game.exe", L"game.exe ", L"C:\\game.exe", L"game*.exe", L"game", L".exe", L"ga\tme.exe"}) {
                Settings output;
                check(!parse_settings(serialize_settings({}) + L"exclude=" + name + L"\n", output).empty(), "invalid basename");
            }
            Settings excessive;
            excessive.exclusions.resize(129, L"game.exe");
            check(!validate_settings(excessive).empty(), "count limit");
        });
        test("UI unchanged percentages retain exact configuration", [] {
            auto original = unusual();
            Settings result;
            check(parse_settings_fields(settings_fields(original), exclusion_text(original), original, result).empty(),
                "parse friendly form");
            check(result == original, "unchanged percentage no quantization");
        });
        test("UI changes and blank exclusion lines", [] {
            auto fields = settings_fields({});
            fields[0] = L"2200"; fields[1] = L"50"; fields[3] = L"10"; fields[4] = L"15"; fields[7] = L"48";
            Settings output;
            check(parse_settings_fields(fields, L"\r\nGame.exe\r\n\r\nTool.EXE\n", {}, output).empty(), "form accepted");
            check(output.pointer.speed == 2200 && output.pointer.precision == .5 && output.pointer.left_exit == .1
                && output.pointer.left_enter == .15 && output.ring_diameter == 48 && output.exclusions.size() == 2, "form units");
        });
        test("UI invalid form atomic rejection", [] {
            auto fields = settings_fields({});
            fields[0] = L"1200"; fields[6] = L"nan";
            Settings output = unusual();
            check(!parse_settings_fields(fields, L"Game.exe", {}, output).empty() && output == unusual(), "invalid number no partial commit");
            fields[6] = L"27";
            check(!parse_settings_fields(fields, L" Game.exe", {}, output).empty() && output == unusual(), "invalid exclusion no partial commit");
        });
        test("missing file is distinct and never created by load", [&] {
            Scratch scratch{root};
            const auto load = load_settings(scratch.file);
            check(load.state == LoadState::Missing && load.error.empty() && !std::filesystem::exists(scratch.file), "missing");
        });
        test("atomic create replace and UTF8 file round trip", [&] {
            Scratch scratch{root};
            auto baseline = load_settings(scratch.file);
            check(save_settings(scratch.file, unusual(), baseline).empty(), "initial save");
            auto loaded = load_settings(scratch.file);
            check(loaded.state == LoadState::Loaded && loaded.settings == unusual(), "UTF8 storage");
            check(save_settings(scratch.file, {}, baseline).empty(), "atomic replacement");
            check(load_settings(scratch.file).settings == Settings{}, "replacement loaded");
            check(!std::filesystem::exists(std::filesystem::path{scratch.file.native() + L".tmp"}), "no temp left");
        });
        test("corrupt unsupported and oversized files not accepted", [&] {
            Scratch scratch{root};
            for (const auto& bytes : {std::string{}, std::string{"version=2\n"}, std::string{"\xff\xfe"}, std::string(settings_byte_limit + 1, 'a')}) {
                scratch.write(bytes);
                auto load = load_settings(scratch.file);
                check(load.state == LoadState::Invalid && !load.error.empty(), "invalid distinguished from missing");
                check(!save_settings(scratch.file, {}, load).empty(), "invalid baseline never overwritten");
                check(std::filesystem::file_size(scratch.file) == bytes.size(), "file remains");
            }
        });
        test("optional UTF8 BOM and LF accepted", [&] {
            Scratch scratch{root};
            auto baseline = load_settings(scratch.file);
            check(save_settings(scratch.file, {}, baseline).empty(), "save fixture");
            auto bytes = baseline.bytes;
            std::erase(bytes, '\r');
            scratch.write(std::string{"\xef\xbb\xbf"} + bytes);
            const auto load = load_settings(scratch.file);
            check(load.state == LoadState::Loaded && load.settings == Settings{}, "BOM LF decode");
        });
        test("unreadable file distinct from missing or corrupt", [&] {
            Scratch scratch{root};
            scratch.write("not readable while exclusively locked");
            const auto handle = CreateFileW(scratch.file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            check(handle != INVALID_HANDLE_VALUE, "exclusive fixture lock");
            auto load = load_settings(scratch.file);
            CloseHandle(handle);
            check(load.state == LoadState::Unreadable && !load.error.empty(), "unreadable is not defaults or parse failure");
            check(!save_settings(scratch.file, {}, load).empty(), "unreadable baseline cannot be replaced");
        });
        test("external edit delete and new file conflict retained", [&] {
            Scratch scratch{root};
            auto baseline = load_settings(scratch.file);
            scratch.write("external");
            check(!save_settings(scratch.file, {}, baseline).empty(), "new file not clobbered");
            std::filesystem::remove(scratch.file);
            check(save_settings(scratch.file, {}, baseline).empty(), "create after conflict");
            scratch.write("external");
            check(!save_settings(scratch.file, {}, baseline).empty() && std::filesystem::file_size(scratch.file) == 8, "edit not clobbered");
            std::filesystem::remove(scratch.file);
            check(!save_settings(scratch.file, {}, baseline).empty() && !std::filesystem::exists(scratch.file), "delete not reversed");
        });
        test("temp collision no clobber", [&] {
            Scratch scratch{root};
            const auto temp = std::filesystem::path{scratch.file.native() + L".tmp"};
            { std::ofstream stream{temp}; stream << "untouched"; }
            auto baseline = load_settings(scratch.file);
            check(!save_settings(scratch.file, {}, baseline).empty(), "temp collision visible");
            check(std::filesystem::file_size(temp) == 9 && !std::filesystem::exists(scratch.file), "stale temp not truncated");
        });
        test("sharing failure retains persisted and runtime baseline", [&] {
            Scratch scratch{root};
            auto baseline = load_settings(scratch.file);
            check(save_settings(scratch.file, {}, baseline).empty(), "save fixture");
            const auto handle = CreateFileW(scratch.file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            check(handle != INVALID_HANDLE_VALUE, "lock fixture");
            const auto error = save_settings(scratch.file, unusual(), baseline);
            CloseHandle(handle);
            check(!error.empty() && baseline.settings == Settings{} && load_settings(scratch.file).settings == Settings{}, "failed replacement retains file and baseline");
            check(!std::filesystem::exists(std::filesystem::path{scratch.file.native() + L".tmp"}), "failed save temp cleaned");
        });
        test("invalid settings never persist", [&] {
            Scratch scratch{root};
            auto baseline = load_settings(scratch.file);
            auto value = unusual();
            value.pointer.speed = std::numeric_limits<double>::infinity();
            check(!save_settings(scratch.file, value, baseline).empty() && !std::filesystem::exists(scratch.file), "invalid save no file");
            value = {};
            value.exclusions = {std::wstring(1, static_cast<wchar_t>(0xd800)) + L".exe"};
            check(!save_settings(scratch.file, value, baseline).empty() && !std::filesystem::exists(scratch.file), "invalid UTF16 no file");
        });
        test("ring geometry centers negative and mixed DPI coordinates", [] {
            for (const unsigned dpi : {96u, 120u, 144u, 192u, 288u, 768u}) {
                for (const int diameter : {20, 40, 67, 96}) {
                    const auto geometry = ring_geometry({-1920, -1080}, dpi, diameter);
                    check(geometry && geometry->side % 2 == 1, "odd pixel grid");
                    check(geometry->left + geometry->side / 2 == -1920 && geometry->top + geometry->side / 2 == -1080, "hotspot centered exactly");
                    check(geometry->side >= diameter * static_cast<int>(dpi) / 96, "DIP scaling not clipped");
                }
            }
        });
        test("ring geometry rejects invalid and overflowing inputs", [] {
            check(!ring_geometry({}, 0, 40) && !ring_geometry({}, 800, 40) && !ring_geometry({}, 96, 19)
                && !ring_geometry({}, 96, 97) && !ring_geometry({INT_MIN, 0}, 96, 40)
                && !ring_geometry({INT_MAX, 0}, 96, 40), "bounded geometry");
        });
        test("ring pixels hollow premultiplied symmetric and contrasting", [] {
            for (const unsigned dpi : {96u, 144u, 192u}) {
                const auto geometry = *ring_geometry({}, dpi, 40);
                const auto pixels = ring_pixels(dpi, 40);
                const auto side = static_cast<std::size_t>(geometry.side);
                check(pixels.size() == side * side && pixels[(side / 2) * side + side / 2] == 0 && pixels.front() == 0, "clear center and corners");
                bool contour = false, body = false, antialias = false;
                for (std::size_t y = 0; y < side; ++y) {
                    for (std::size_t x = 0; x < side; ++x) {
                        const auto pixel = pixels[y * side + x];
                        const auto a = pixel >> 24;
                        // Channels carry independent colour now, so each one is checked against
                        // alpha on its own instead of assuming a single grey level.
                        check(((pixel >> 16) & 255) <= a && ((pixel >> 8) & 255) <= a && (pixel & 255) <= a, "BGRA premultiplication");
                        check(pixel == pixels[y * side + side - 1 - x] && pixel == pixels[(side - 1 - y) * side + x], "symmetric ring");
                        contour |= pixel == 0xff1c1e24;
                        body |= pixel == 0xfff6f3ec;
                        antialias |= a > 0 && a < 255;
                    }
                }
                check(contour && body && antialias, "charcoal contour, off-white body, and smooth edges present");
                // The stroke reads as a band, and the whole circle stays inside its nominal box.
                std::size_t hole = 0, stroke = 0, gap = 0;
                for (std::size_t x = side / 2; x < side; ++x) {
                    const bool inked = pixels[(side / 2) * side + x] != 0;
                    if (!inked && stroke == 0) { ++hole; } else if (inked) { ++stroke; } else { ++gap; }
                }
                check(hole > 0 && stroke > 0 && gap > 0, "clear hole, drawn stroke, margin before the edge");
                check(stroke * 100 >= (hole + stroke) * 30, "stroke is a substantial share of the drawn radius");
                check((hole + stroke) * 2 <= static_cast<std::size_t>(40 * static_cast<int>(dpi) / 96), "drawn circle sits inside its nominal diameter");
            }
            check(ring_pixels(0, 40).empty(), "invalid rendering geometry rejected");
        });
        test("literal legacy v1 default file preserves its explicitly saved size", [] {
            constexpr auto legacy = L"version=1\nspeed=1100\nprecision=0.25\nscroll=960\nleft_exit=0.20\nleft_enter=0.24\n"
                L"right_exit=0.23\nright_enter=0.27\nring=1\nring_diameter=40\n";
            Settings output = unusual();
            output.cursor_style = CursorStyle::Arrow;
            Settings expected;
            expected.ring_diameter = 40;
            expected.cursor_style = CursorStyle::Ring;
            check(parse_settings(legacy, output).empty() && output == expected, "old saved40DIP size must not become28");
        });
        test("v1 all doubles visibility diameter and Unicode exclusions migrate exactly", [] {
            const auto original = unusual();
            Settings output;
            check(parse_settings(version_one(original), output).empty() && output == original, "legacy exact raw fields");
            check(serialize_settings(output).starts_with(L"version=12\r\n"), "normal serializer emits v12");
            check(parse_settings_fields(settings_fields(output), exclusion_text(output), output, output).empty()
                && output == original, "unchanged migration apply retains all doubles");
        });
        test("legacy flags migrate to replacement while preserving every style and saved value", [&] {
            Scratch scratch{root};
            auto baseline = load_settings(scratch.file);
            for (const auto& choice : cursor_styles) {
                for (const bool replace : {false, true}) {
                    for (const bool visible : {false, true}) {
                        auto expected = unusual();
                        expected.cursor_style = choice.style;
                        Settings actual;
                        check(parse_settings(version_two(expected, replace, visible), actual).empty() && actual == expected, "v2 legacy flags validated but no longer determine visibility");
                        check(parse_settings(serialize_settings(expected), actual).empty() && actual == expected, "v8 style round trip");
                        check(parse_settings_fields(settings_fields(expected), exclusion_text(expected), expected, actual).empty()
                            && actual == expected, "unchanged editor preserves cursor options and exact doubles");
                        check(save_settings(scratch.file, expected, baseline).empty(), "save v8 cursor options");
                        const auto loaded = load_settings(scratch.file);
                        check(loaded.settings == expected && loaded.bytes.find("replace_cursor=") == std::string::npos
                              && loaded.bytes.find("\nring=") == std::string::npos, "legacy visibility flags are not persisted");
                    }
                }
            }
        });
        test("invalid cursor schema never partially changes destination", [] {
            const auto text = serialize_settings({});
            const auto missing_style = without(text, L"cursor_style");
            const auto legacy = version_two({}, false, true);
            auto missing_replace = legacy; missing_replace.erase(missing_replace.find(L"replace_cursor="), std::wstring_view{L"replace_cursor=0\r\n"}.size());
            for (const auto& bad : {changed(text, L"cursor_style", L"unknown"), changed(text, L"cursor_style", L"Ring"),
                changed(text, L"cursor_style", L" arrow"), changed(legacy, L"replace_cursor", L"true"),
                changed(legacy, L"replace_cursor", L"2"), changed(legacy, L"replace_cursor", L"-1"),
                changed(legacy, L"ring", L"true"), text + L"ring=1\n",
                changed(text, L"version", L"1"), text + L"cursor_style=dot\n", text + L"replace_cursor=1\n",
                missing_style, missing_replace, version_one({}) + L"replace_cursor=0\n", version_one({}) + L"cursor_style=ring\n"}) {
                auto output = unusual();
                check(!parse_settings(bad, output).empty() && output == unusual(), "strict schema atomic reject");
            }
            auto invalid = unusual(); invalid.cursor_style = static_cast<CursorStyle>(99);
            check(!validate_settings(invalid).empty(), "invalid internal enum rejected");
        });
        test("legacy storage read never rewrites and normal apply alone migrates", [&] {
            Scratch scratch{root};
            const auto original = unusual();
            const auto bytes = utf8(version_one(original));
            scratch.write(bytes);
            auto baseline = load_settings(scratch.file);
            check(baseline.state == LoadState::Loaded && baseline.settings == original && baseline.bytes == bytes, "legacy read");
            check(load_settings(scratch.file).bytes == bytes, "load leaves original byte representation unchanged");
            auto editor = Settings{};
            check(load_settings(scratch.file).bytes == bytes && editor.ring_diameter == 28 && editor.cursor_style == CursorStyle::Arrow,
                "editor-only defaults no write");
            check(save_settings(scratch.file, original, baseline).empty(), "normal Apply migrates");
            const auto saved = load_settings(scratch.file);
            check(saved.bytes.starts_with("version=12\r\n") && saved.settings == original, "migration preserves exact values and exclusions");
        });
        test("v1 external conflict retains migration source", [&] {
            Scratch scratch{root};
            scratch.write(utf8(version_one(unusual())));
            auto baseline = load_settings(scratch.file);
            const auto edited = utf8(version_one({}));
            scratch.write(edited);
            check(!save_settings(scratch.file, unusual(), baseline).empty(), "v1 conflict blocks migration");
            check(load_settings(scratch.file).bytes == edited, "external v1 source retained");
        });
        test("restore defaults stages legacy motion and modern pointer without altering saved size", [] {
            auto active = unusual();
            active.cursor_style = CursorStyle::Arrow;
            auto editor = active;
            editor = {};
            check(editor == Settings{} && editor.pointer == live::PointerSettings{} && editor.cursor_style == CursorStyle::Arrow
                && editor.ring_diameter == 28, "all defaults staged");
            check(active.ring_diameter == 67 && active.cursor_style == CursorStyle::Arrow && active.pointer == unusual().pointer, "active values untouched");
        });
        test("v1 v2 v3 preserve all explicit sizes and tuning with Smooth motion", [] {
            for (int size = 20; size <= 96; ++size) {
                auto expected = unusual();
                expected.ring_diameter = size;
                for (const auto& source : {version_one(expected), version_two(expected, false, false), version_three(expected)}) {
                    Settings result;
                    result.pointer.profile = live::MotionProfile::SpeedBands;
                    check(parse_settings(source, result).empty() && result == expected, "every legacy size preserved; motion resets to Smooth");
                }
            }
        });
        test("profile round trip storage and editor without touching legacy doubles", [&] {
            Scratch scratch{root};
            auto baseline = load_settings(scratch.file);
            for (const auto profile : {live::MotionProfile::Smooth, live::MotionProfile::SpeedBands}) {
                auto expected = unusual(); expected.pointer.profile = profile;
                Settings result;
                check(parse_settings(serialize_settings(expected), result).empty() && result == expected, "profile exact");
                check(parse_settings_fields(settings_fields(result), exclusion_text(result), result, result).empty()
                    && result == expected, "editor exact");
                check(save_settings(scratch.file, result, baseline).empty() && load_settings(scratch.file).settings == expected,
                    "profile persisted only on save");
                check(baseline.bytes.starts_with("version=12\r\n") && baseline.bytes.find("link_hint=") == std::string::npos,
                    "new schema omits removed hint");
            }
        });
        test("invalid profiles duplicates omissions and unsupported version keys rejected atomically", [] {
            const auto text = serialize_settings({});
            for (const auto& bad : {changed(text, L"motion_profile", L"Smooth"), changed(text, L"motion_profile", L"bands"),
                changed(text, L"motion_profile", L"speed_bands "), changed(text, L"motion_profile", L""),
                without(text, L"motion_profile"), text + L"motion_profile=smooth\n",
                text + L"link_hint=0\n", text + L"link_hint=1\n", text + L"link_hint=true\n",
                version_one({}) + L"motion_profile=smooth\n", version_two({}, false, true) + L"link_hint=0\n",
                version_three({}) + L"motion_profile=smooth\n", version_three({}) + L"link_hint=0\n"}) {
                auto result = unusual(); result.pointer.profile = live::MotionProfile::SpeedBands;
                const auto before = result;
                check(!parse_settings(bad, result).empty() && result == before, "strict atomic reject");
            }
            Settings invalid; invalid.pointer.profile = static_cast<live::MotionProfile>(99);
            check(!validate_settings(invalid).empty(), "internal invalid profile rejected");
        });
        test("v2 v3 loads do not migrate storage before Apply", [&] {
            Scratch scratch{root};
            for (const auto& source : {version_two(unusual(), true, true), version_three(unusual())}) {
                const auto bytes = utf8(source); scratch.write(bytes);
                auto baseline = load_settings(scratch.file);
                check(baseline.state == LoadState::Loaded && baseline.settings == unusual()
                    && load_settings(scratch.file).bytes == bytes, "legacy load no write");
                auto next = baseline.settings; next.pointer.profile = live::MotionProfile::SpeedBands;
                check(save_settings(scratch.file, next, baseline).empty(), "explicit migration");
                check(baseline.bytes.starts_with("version=12\r\n") && load_settings(scratch.file).settings == next, "v12 exact");
            }
        });
        test("literal v4 hint zero and one migrate identically without rewriting storage", [&] {
            constexpr auto legacy = L"version=4\r\nspeed=1234.56789\r\nprecision=0.3141592653589793\r\nscroll=765.4321\r\n"
                L"left_exit=0.123456789012345\r\nleft_enter=0.33333333333333\r\nright_exit=0.29\r\nright_enter=0.47\r\n"
                L"ring_diameter=67\r\ncursor_style=brackets\r\nmotion_profile=speed_bands\r\nlink_hint=0\r\n"
                L"exclude=Game.exe\r\nexclude=\u6e38\u620f.EXE\r\n";
            Scratch scratch{root};
            auto expected = unusual();
            expected.cursor_style = CursorStyle::Brackets;
            expected.pointer.profile = live::MotionProfile::SpeedBands;
            for (const auto hint : {L"0", L"1"}) {
                const auto source = changed(legacy, L"link_hint", hint);
                Settings parsed;
                check(parse_settings(source, parsed).empty() && parsed == expected, "legacy hint is ignored, not other saved fields");
                const auto bytes = std::string{"\xef\xbb\xbf"} + utf8(source);
                scratch.write(bytes);
                const auto written = std::filesystem::last_write_time(scratch.file);
                auto baseline = load_settings(scratch.file);
                check(baseline.state == LoadState::Loaded && baseline.settings == expected && baseline.bytes == bytes,
                    "v4 loads both hint values and keeps original UTF8 bytes");
                check(load_settings(scratch.file).bytes == bytes && std::filesystem::last_write_time(scratch.file) == written,
                    "loading does not rewrite the file or its write timestamp");
                check(!std::filesystem::exists(std::filesystem::path{scratch.file.native() + L".tmp"}), "loading creates no staging file");
                Settings editor;
                check(parse_settings_fields(settings_fields(expected), exclusion_text(expected), expected, editor).empty()
                    && editor == expected && load_settings(scratch.file).bytes == bytes, "editing retains exact values without saving");
                check(save_settings(scratch.file, editor, baseline).empty(), "only explicit save migrates");
                const auto saved = load_settings(scratch.file);
                check(saved.state == LoadState::Loaded && saved.settings == expected && saved.bytes.starts_with("version=12\r\n")
                    && saved.bytes.find("link_hint=") == std::string::npos, "v8 preserves values and drops obsolete field");
            }
        });
        test("v4 preserves every saved size style profile tuning and exclusion with either hint value", [] {
            for (int size = 20; size <= 96; ++size) {
                for (const auto& choice : cursor_styles) {
                    for (const auto profile : {live::MotionProfile::Smooth, live::MotionProfile::SpeedBands}) {
                        auto expected = unusual();
                        expected.ring_diameter = size;
                        expected.cursor_style = choice.style;
                        expected.pointer.profile = profile;
                        for (const bool hint : {false, true}) {
                            Settings actual;
                            check(parse_settings(version_four(expected, hint), actual).empty() && actual == expected, "complete v4 migration");
                        }
                    }
                }
            }
        });
        test("v4 through v8 reject every required field omission and duplicate atomically", [] {
            for (const auto& source : {version_four({}, false), version_five({}), version_six({}), version_seven({}), serialize_settings({})}) {
                auto lines = std::wstring_view{source};
                while (!lines.empty()) {
                    const auto end = lines.find(L"\r\n");
                    check(end != lines.npos, "complete fixture line");
                    const auto line = lines.substr(0, end);
                    const auto key = line.substr(0, line.find(L'='));
                    for (const auto& bad : {without(source, key), source + std::wstring{line} + L"\r\n"}) {
                        auto result = unusual();
                        check(!parse_settings(bad, result).empty() && result == unusual(), "required fields are strict and atomic");
                    }
                    lines.remove_prefix(end + 2);
                }
            }
        });
        test("v4 validates legacy hint syntax and rejects invalid fields without rewriting", [&] {
            Scratch scratch{root};
            const auto source = version_four({}, true);
            std::vector<std::wstring> invalid{
                without(source, L"link_hint"), source + L"link_hint=0\r\n", source + L"link_hint=1\r\n",
                changed(source, L"motion_profile", L"unknown"), changed(source, L"cursor_style", L"unknown"),
                changed(source, L"speed", L"nan"), source + L"ring=1\r\n", source + L"replace_cursor=1\r\n"};
            for (const auto hint : {L"", L"true", L"false", L"2", L"-1", L"1.0", L"01", L" 1", L"1 "}) {
                invalid.push_back(changed(source, L"link_hint", hint));
            }
            for (const auto& bad : invalid) {
                auto result = unusual();
                check(!parse_settings(bad, result).empty() && result == unusual(), "bad v4 must not partially change destination");
                const auto bytes = utf8(bad);
                scratch.write(bytes);
                const auto written = std::filesystem::last_write_time(scratch.file);
                auto baseline = load_settings(scratch.file);
                check(baseline.state == LoadState::Invalid && baseline.bytes == bytes && !baseline.error.empty(), "invalid v4 file reported");
                check(!save_settings(scratch.file, {}, baseline).empty() && load_settings(scratch.file).bytes == bytes
                    && std::filesystem::last_write_time(scratch.file) == written, "invalid migration source is never overwritten");
            }
        });
        test("band rates round trip text file and unchanged percentage editor exactly", [&] {
            Scratch scratch{root};
            auto baseline = load_settings(scratch.file);
            for (const auto rates : {std::array{.35, .35, 1.0}, std::array{.10, .60, .60},
                std::array{.01, .01, .01}, std::array{.123456789012345, .3141592653589793, .987654321098765}}) {
                auto expected = unusual(); expected.pointer.band_rates = rates;
                Settings actual;
                check(parse_settings(serialize_settings(expected), actual).empty() && actual == expected, "band ratios lost precision");
                check(parse_settings_fields(settings_fields(expected), exclusion_text(expected), expected, actual).empty()
                    && actual == expected, "unchanged band percentages changed exact ratios");
                check(save_settings(scratch.file, expected, baseline).empty()
                    && load_settings(scratch.file).settings == expected, "band rates were not persisted");
            }
        });
        test("equal percentage editor fields merge bands and invalid rates are atomic", [] {
            auto original = unusual();
            auto fields = settings_fields(original);
            fields[8] = fields[9] = L"35"; fields[10] = L"100";
            Settings actual;
            check(parse_settings_fields(fields, exclusion_text(original), original, actual).empty()
                && actual.pointer.band_rates == std::array{.35, .35, 1.0}, "merged band editor values rejected");
            for (std::size_t field = 8; field < fields.size(); ++field) {
                for (const auto bad : {L"0", L"101", L"nan", L"", L"35%", L" 35"}) {
                    auto invalid = fields; invalid[field] = bad;
                    actual = original;
                    check(!parse_settings_fields(invalid, exclusion_text(original), original, actual).empty()
                        && actual == original, "invalid band editor partially applied");
                }
            }
            fields[8] = L"36"; actual = original;
            check(!parse_settings_fields(fields, exclusion_text(original), original, actual).empty()
                && actual == original, "descending rates accepted");
            check(Settings{}.pointer.band_rates == std::array{.10, .35, 1.0}, "restored band defaults drifted");
        });
        test("legacy versions retain other preferences and gain default band rates without writing", [&] {
            Scratch scratch{root};
            const auto expected = unusual();
            for (const auto& source : {version_one(expected), version_two(expected, false, false),
                version_three(expected), version_four(expected, true), version_five(expected)}) {
                const auto bytes = utf8(source); scratch.write(bytes);
                const auto written = std::filesystem::last_write_time(scratch.file);
                const auto baseline = load_settings(scratch.file);
                check(baseline.state == LoadState::Loaded && baseline.settings == expected
                    && baseline.settings.pointer.band_rates == std::array{.10, .35, 1.0}, "legacy rates or preferences changed");
                check(baseline.bytes == bytes && std::filesystem::last_write_time(scratch.file) == written,
                    "legacy load wrote migration");
                for (const auto key : {L"band_low", L"band_middle", L"band_high"}) {
                    Settings output = expected;
                    check(!parse_settings(source + key + std::wstring{L"=0.5\r\n"}, output).empty()
                        && output == expected, "new band field accepted in legacy version");
                }
            }
        });
        test("automatic switching round trips on and off without changing pointer settings", [&] {
            Scratch scratch{root}; auto baseline = load_settings(scratch.file);
            for (const bool automatic : {false, true}) {
                auto expected = unusual(); expected.automatic_game_switching = automatic;
                expected.pointer.band_rates = {.35, .35, 1.0};
                Settings actual;
                check(parse_settings(serialize_settings(expected), actual).empty() && actual == expected, "Auto text value changed");
                check(parse_settings_fields(settings_fields(expected), exclusion_text(expected), expected, actual).empty()
                    && actual == expected, "Auto editor value lost");
                check(save_settings(scratch.file, expected, baseline).empty()
                    && load_settings(scratch.file).settings == expected, "Auto setting not persisted");
            }
            check(Settings{}.automatic_game_switching, "Auto default should remain enabled");
        });
        test("automatic switching legacy defaults and boolean validation are strict", [&] {
            Scratch scratch{root};
            auto expected = unusual(); expected.pointer.band_rates = {.35, .35, .8};
            const auto source = version_six(expected); const auto bytes = utf8(source); scratch.write(bytes);
            const auto written = std::filesystem::last_write_time(scratch.file);
            const auto baseline = load_settings(scratch.file);
            check(baseline.state == LoadState::Loaded && baseline.settings == expected && baseline.settings.automatic_game_switching
                && baseline.bytes == bytes && std::filesystem::last_write_time(scratch.file) == written, "v6 migration changed saved state");
            const auto text = serialize_settings(expected);
            for (const auto value : {L"", L"true", L"false", L"01", L"2", L"-1", L"1.0", L" 1"}) {
                Settings output = expected;
                check(!parse_settings(changed(text, L"automatic_game_switching", value), output).empty()
                    && output == expected, "invalid Auto value partially loaded");
            }
            Settings output = expected;
            check(!parse_settings(source + L"automatic_game_switching=1\r\n", output).empty()
                && output == expected, "new Auto key accepted in an old schema");
        });
        test("keyboard input mode round trips, defaults FaceButtons pre-v8, and rejects invalid values atomically", [&] {
            Scratch scratch{root}; auto baseline = load_settings(scratch.file);
            check(Settings{}.keyboard_input_mode == KeyboardInputMode::FaceButtons, "default mode is FaceButtons");
            for (const auto mode : {KeyboardInputMode::FaceButtons, KeyboardInputMode::OneHanded}) {
                auto expected = unusual(); expected.keyboard_input_mode = mode;
                Settings actual;
                check(parse_settings(serialize_settings(expected), actual).empty() && actual == expected, "mode text round trip");
                check(parse_settings_fields(settings_fields(expected), exclusion_text(expected), expected, actual).empty()
                    && actual == expected, "mode editor value lost");
                check(save_settings(scratch.file, expected, baseline).empty()
                    && load_settings(scratch.file).settings == expected, "mode not persisted");
            }
            for (const auto& source : {version_one({}), version_two({}, false, false), version_three({}),
                version_four({}, false), version_five({}), version_six({}), version_seven({})}) {
                Settings output;
                check(parse_settings(source, output).empty() && output.keyboard_input_mode == KeyboardInputMode::FaceButtons,
                    "pre-v8 schema defaults to FaceButtons without an explicit key");
            }
            const auto text = serialize_settings({});
            for (const auto value : {L"", L"FaceButtons", L"one-handed", L"2", L" one_handed", L"one_handed "}) {
                Settings output = unusual();
                check(!parse_settings(changed(text, L"keyboard_input", value), output).empty()
                    && output == unusual(), "invalid keyboard_input value partially loaded");
            }
            Settings output = unusual();
            check(!parse_settings(version_seven({}) + L"keyboard_input=one_handed\r\n", output).empty()
                && output == unusual(), "new keyboard_input key accepted in an old schema");
        });
        test("native keyboard defaults off across v1-v8 without rewriting and v9 is strict", [&] {
            check(!Settings{}.keyboard_in_native_mode, "native keyboard must default off");
            Scratch scratch{root};
            for (const auto& source : {version_one({}), version_two({}, false, false), version_three({}),
                version_four({}, false), version_five({}), version_six({}), version_seven({}), version_eight({})}) {
                const auto bytes = utf8(source); scratch.write(bytes);
                const auto written = std::filesystem::last_write_time(scratch.file);
                const auto loaded = load_settings(scratch.file);
                check(loaded.state == LoadState::Loaded && !loaded.settings.keyboard_in_native_mode
                    && loaded.bytes == bytes && std::filesystem::last_write_time(scratch.file) == written,
                    "legacy loading must not enable native typing or rewrite file");
                Settings output = unusual();
                check(!parse_settings(source + L"keyboard_in_native_mode=1\r\n", output).empty()
                    && output == unusual(), "old schema must reject the new authorization key");
            }
            auto settings = unusual(); settings.keyboard_in_native_mode = true;
            Settings actual;
            const auto text = serialize_settings(settings);
            check(parse_settings(text, actual).empty() && actual == settings, "v9 true round trip");
            check(parse_settings_fields(settings_fields(settings), exclusion_text(settings), settings, actual).empty()
                && actual == settings, "editor must preserve explicit native option");
            for (const auto& invalid : {without(text, L"keyboard_in_native_mode"), text + L"keyboard_in_native_mode=1\r\n",
                changed(text, L"keyboard_in_native_mode", L"true"), changed(text, L"keyboard_in_native_mode", L"2"),
                changed(text, L"keyboard_in_native_mode", L"01")}) {
                actual = unusual();
                check(!parse_settings(invalid, actual).empty() && actual == unusual(), "v9 native bool rejects atomically");
            }
        });
        test("navigation filter schema10 is opt-in and all older settings load without rewriting", [&] {
            check(!Settings{}.experimental_navigation_filter, "filter default must be false");
            Scratch scratch{root};
            for (const auto& source : {version_one({}), version_two({}, false, false), version_three({}),
                version_four({}, false), version_five({}), version_six({}), version_seven({}), version_eight({}), version_nine({})}) {
                const auto bytes = utf8(source); scratch.write(bytes);
                const auto written = std::filesystem::last_write_time(scratch.file);
                const auto baseline = load_settings(scratch.file);
                check(baseline.state == LoadState::Loaded && !baseline.settings.experimental_navigation_filter
                    && baseline.bytes == bytes && std::filesystem::last_write_time(scratch.file) == written,
                    "legacy load must not enable hooks or rewrite user preference");
                Settings output = unusual();
                check(!parse_settings(source + L"experimental_navigation_filter=1\r\n", output).empty()
                    && output == unusual(), "legacy schema cannot smuggle filter authorization");
            }
            for (const bool enabled : {false, true}) {
                auto expected = unusual(); expected.experimental_navigation_filter = enabled;
                Settings actual;
                const auto source = serialize_settings(expected);
                check(parse_settings(source, actual).empty() && actual == expected, "filter bool exact roundtrip");
                check(parse_settings_fields(settings_fields(expected), exclusion_text(expected), expected, actual).empty()
                    && actual == expected, "ordinary settings editing preserves opt-in");
                for (const auto& bad : {without(source, L"experimental_navigation_filter"),
                    source + L"experimental_navigation_filter=0\r\n",
                    changed(source, L"experimental_navigation_filter", L"true"),
                    changed(source, L"experimental_navigation_filter", L"01"),
                    changed(source, L"experimental_navigation_filter", L"2"),
                    changed(source, L"version", L"010")}) {
                    actual = unusual();
                    check(!parse_settings(bad, actual).empty() && actual == unusual(), "invalid filter setting rejected atomically");
                }
            }
        });
        test("voice defaults off and v1-v10 migration never writes or loses preferences", [&] {
            check(!Settings{}.voice_enabled && Settings{}.voice_model_path.empty(), "safe voice defaults");
            Scratch scratch{root};
            const auto expected = unusual();
            for (const auto& source : {version_one(expected), version_two(expected, false, false), version_three(expected),
                version_four(expected, false), version_five(expected), version_six(expected), version_seven(expected),
                version_eight(expected), version_nine(expected), version_ten(expected)}) {
                const auto bytes = utf8(source); scratch.write(bytes);
                const auto written = std::filesystem::last_write_time(scratch.file);
                const auto loaded = load_settings(scratch.file);
                check(loaded.state == LoadState::Loaded && loaded.settings == expected && loaded.bytes == bytes
                    && std::filesystem::last_write_time(scratch.file) == written, "legacy voice migration changed or wrote settings");
                Settings output = unusual();
                check(!parse_settings(source + L"voice_enabled=1\r\n", output).empty() && output == unusual(),
                    "legacy schema cannot authorize voice");
            }
        });
        test("v1-v11 enables suggestions without writes and explicit Save alone migrates", [&] {
            Scratch scratch{root};
            const auto expected = unusual();
            for (const auto& source : {version_one(expected), version_two(expected, false, false), version_three(expected),
                version_four(expected, false), version_five(expected), version_six(expected), version_seven(expected),
                version_eight(expected), version_nine(expected), version_ten(expected), version_eleven(expected)}) {
                const auto bytes = utf8(source); scratch.write(bytes);
                const auto written = std::filesystem::last_write_time(scratch.file);
                auto loaded = load_settings(scratch.file);
                check(loaded.state == LoadState::Loaded && loaded.settings == expected && loaded.settings.word_suggestions
                    && load_settings(scratch.file).bytes == bytes && std::filesystem::last_write_time(scratch.file) == written,
                    "legacy prediction default must not alter bytes, timestamp, or previous preferences");
                auto output = unusual();
                check(!parse_settings(source + L"word_suggestions=0\r\n", output).empty() && output == unusual(),
                    "old schemas reject new keys transactionally");
                auto opted_out = loaded.settings; opted_out.word_suggestions = false;
                check(save_settings(scratch.file, opted_out, loaded).empty(), "explicit Save migrates with opt-out");
                check(loaded.bytes.starts_with("version=12\r\n") && load_settings(scratch.file).settings == opted_out,
                    "schema12 retains the opt-out and all other preferences");
            }
        });
        test("word suggestions strict bool roundtrip and atomic invalid rejection", [] {
            for (bool enabled : {false, true}) {
                auto expected = unusual(); expected.word_suggestions = enabled;
                const auto source = serialize_settings(expected);
                Settings actual;
                check(parse_settings(source, actual).empty() && actual == expected, "exact suggestion option roundtrip");
                check(parse_settings_fields(settings_fields(expected), exclusion_text(expected), expected, actual).empty()
                    && actual == expected, "unchanged editor retains opt-out");
                for (const auto& bad : {without(source, L"word_suggestions"), source + L"word_suggestions=0\r\n",
                    changed(source, L"word_suggestions", L"true"), changed(source, L"word_suggestions", L"01"),
                    changed(source, L"word_suggestions", L"2"), changed(source, L"word_suggestions", L"-1"),
                    changed(source, L"word_suggestions", L""), changed(source, L"version", L"012")}) {
                    actual = unusual();
                    check(!parse_settings(bad, actual).empty() && actual == unusual(), "invalid prediction option is atomic");
                }
            }
        });
        test("voice preferences round trip Unicode equals empty and long paths without numeric drift", [&] {
            Scratch scratch{root}; auto baseline = load_settings(scratch.file);
            for (const auto& path : {std::wstring{}, std::wstring{L"C:\\Models\\\u97f3\u58f0=tiny.en.bin"},
                    std::wstring(voice_model_path_limit, L'x')}) {
                auto expected = unusual(); expected.voice_enabled = true; expected.voice_model_path = path;
                Settings actual;
                check(parse_settings(serialize_settings(expected), actual).empty() && actual == expected, "voice exact roundtrip");
                check(parse_settings_fields(settings_fields(expected), exclusion_text(expected), expected, actual).empty()
                    && actual == expected, "ordinary field edit lost voice preference");
                check(save_settings(scratch.file, expected, baseline).empty() && load_settings(scratch.file).settings == expected,
                    "voice preferences not persisted exactly");
            }
        });
        test("voice schema rejects invalid authorization missing duplicate keys and unsafe paths atomically", [] {
            const auto text = serialize_settings({});
            for (const auto& invalid : {without(text, L"voice_enabled"), without(text, L"voice_model_path"),
                text + L"voice_enabled=0\r\n", text + L"voice_model_path=x\r\n",
                changed(text, L"voice_enabled", L"true"), changed(text, L"voice_enabled", L"01"),
                changed(text, L"voice_enabled", L"-1"), changed(text, L"voice_enabled", L"2"),
                changed(text, L"voice_model_path", std::wstring(voice_model_path_limit + 1, L'x')),
                changed(text, L"voice_model_path", L"a\tb"), changed(text, L"voice_model_path", L"a\rb"),
                changed(text, L"voice_model_path", std::wstring(1, 0xD800))}) {
                auto output = unusual();
                check(!parse_settings(invalid, output).empty() && output == unusual(), "invalid voice settings partially loaded");
            }
            for (const wchar_t control : {wchar_t{0}, wchar_t{10}, wchar_t{13}, wchar_t{0x7F}, wchar_t{0x85}}) {
                auto invalid = unusual(); invalid.voice_model_path = std::wstring{L"x"} + control + L"y";
                check(!validate_settings(invalid).empty(), "control character accepted in model path");
            }
        });
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << count << " cases: " << error.what() << '\n';
        return 1;
    }
    std::cout << count << " settings/storage/ring cases passed.\n";
    return 0;
}
