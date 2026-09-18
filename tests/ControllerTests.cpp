#include "ControllerDevices.h"
#include "ControllerMapping.h"

#include <Windows.h>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace gamepad;
namespace {
void check(bool value, const char* message)
{
    if (!value) { throw std::runtime_error(message); }
}
constexpr std::string_view id = "03000000341200007856000000000000";
const std::string ordinary = std::string(id) + ",Synthetic,a:b0,b:b1,x:b2,y:b3,back:b4,start:b5,"
    "leftshoulder:b6,rightshoulder:b7,leftstick:b8,rightstick:b9,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Windows,";
ControllerMapping mapping(std::string_view line = ordinary)
{
    ControllerMapping value;
    check(parse_controller_mapping(line, value).empty(), "valid SDL-format mapping");
    return value;
}
void parsing()
{
    const auto normal = mapping();
    check(directinput_mapping_guid(0x1234, 0x5678) == id, "SDL Windows VID/PID GUID byte order");
    check(directinput_mapping_guid(0, 0, "Example") == "05000000" "4578616d706c65" "0000000000",
        "legacy GUID uses bounded name bytes, not an instance or arbitrary VID");
    check(controller_name_crc("123456789") == 0xbb3d, "standard reflected CRC16 for name-qualified records");
    check(mapping_fits(normal, {4, 12, 1}) && !mapping_fits(normal, {3, 12, 1})
        && !mapping_fits(normal, {4, 11, 1}) && !mapping_fits(normal, {4, 12, 0}), "mapping cannot exceed actual control counts");
    const auto base = std::string(id) + ",Synthetic,a:b0,leftx:a0,lefty:a1,";
    for (const auto tail : {"a:b2,", "lefttrigger:a8,", "righttrigger:b128,", "dpup:h4.1,",
        "dpup:h0.0,", "dpup:h0.16,", "platform:Linux,", "lefttrigger:+b2,", "lefttrigger:a1x,",
        "lefttrigger:a-1,", "unknown:b1,", "lefttrigger:a1~~,", ",", "+leftx:b1,", "crc:zzzz,"}) {
        auto untouched = normal;
        check(!parse_controller_mapping(base + tail, untouched).empty(), "invalid or ambiguous mappings are rejected");
        check(untouched.axes[2].index == normal.axes[2].index && untouched.buttons[13].index == 3,
            "parsing is transactional");
    }
    auto value = normal;
    check(!parse_controller_mapping(std::string(5000, 'a'), value).empty(), "mapping text is bounded");
    const auto matching = base + "platform:Windows,";
    const auto different = std::string(id) + ",Other,a:b1,leftx:a0,lefty:a1,platform:Windows,";
    check(find_controller_mapping(matching + "\n" + different, id) == different, "last generic mapping wins");
    const auto qualified = base + "crc:1234,platform:Windows,";
    check(find_controller_mapping(matching + "\n" + qualified, id, 0x2222) == matching,
        "name-qualified record cannot apply to a different controller name");
    check(find_controller_mapping(qualified + "\n" + matching, id, 0x1234) == qualified,
        "matching name qualification takes precedence over a generic record");
    const std::string embedded = "03003dbb341200007856000000000000,Synthetic,a:b1,leftx:a0,lefty:a1,platform:Windows,";
    check(find_controller_mapping(embedded, id, 0xbb3d) == embedded, "embedded SDL GUID CRC matches normalized Windows GUID");
    check(find_controller_mapping(embedded, id, 0x1234).empty(), "embedded CRC cannot match a different product name");
    const auto no_comma = base + "crc:1234";
    check(find_controller_mapping(no_comma + "\r\n", id, 0x1234) == no_comma,
        "CRLF mapping without trailing comma retains CRC qualification");
    auto parsed_crc = normal;
    check(!parse_controller_mapping(embedded + "crc:1234,", parsed_crc).empty(), "contradictory CRC metadata is rejected");
    check(!parse_controller_mapping(base + "crc:1234,crc:5678,", parsed_crc).empty(), "duplicate CRC cannot redirect an override");
    check(find_controller_mapping(matching, "00000000000000000000000000000000").empty(), "unknown device stays unmapped");
}
void normalization()
{
    const auto map = mapping();
    RawController raw;
    check(neutral(mapped_controller(map, raw)), "centered mapped controller is neutral");
    constexpr std::array<std::uint16_t, 10> bits{0x1000, 0x2000, 0x4000, 0x8000, 0x20, 0x10, 0x100, 0x200, 0x40, 0x80};
    for (unsigned i = 0; i < bits.size(); ++i) {
        raw = {}; raw.buttons[i] = true;
        check(mapped_controller(map, raw).buttons == bits[i], "logical button actions preserve physical-position convention");
    }
    raw = {}; raw.buttons[10] = raw.buttons[11] = true;
    check(mapped_controller(map, raw).left_trigger == 255 && mapped_controller(map, raw).right_trigger == 255,
        "independent digital triggers, including simultaneous hold");
    raw = {}; raw.axes = {-32768, -32768, 32767, 32767};
    const auto extreme = mapped_controller(map, raw);
    check(extreme.left_x == -32768 && extreme.left_y == 32767 && extreme.right_x == 32767 && extreme.right_y == -32767,
        "stick ranges and Y inversion avoid signed overflow");
    raw = {}; raw.hats[0] = 3;
    check(mapped_controller(map, raw).buttons == 9, "POV diagonals become two desktop D-pad directions");
    const auto base = std::string(id) + ",Synthetic,a:b0,leftx:a0,lefty:a1,";
    const auto analog = mapping(base + "lefttrigger:a4,righttrigger:-a5,");
    raw = {}; raw.axes[4] = -32768;
    check(mapped_controller(analog, raw).left_trigger == 0 && mapped_controller(analog, raw).right_trigger == 0,
        "full-range trigger rests at negative endpoint; negative half trigger rests at zero");
    raw.axes[4] = 32767; raw.axes[5] = -32768;
    check(mapped_controller(analog, raw).left_trigger == 255 && mapped_controller(analog, raw).right_trigger == 255,
        "full and half trigger endpoints");
    raw.axes[5] = 32767;
    check(mapped_controller(analog, raw).right_trigger == 0, "opposite half-axis never activates trigger");
    const auto inverted = mapping(base + "lefttrigger:+a4~,righttrigger:-a5~,");
    raw = {}; raw.axes[4] = 32767; raw.axes[5] = -32768;
    check(mapped_controller(inverted, raw).left_trigger == 0 && mapped_controller(inverted, raw).right_trigger == 0,
        "inversion swaps half-axis endpoints, not axis sign before half selection");
    raw = {};
    check(mapped_controller(inverted, raw).left_trigger == 255 && mapped_controller(inverted, raw).right_trigger == 255,
        "inverted half-axis zero endpoint");
    const auto hat = mapping(base + "x:h0.3,");
    raw = {}; raw.hats[0] = 1;
    check((mapped_controller(hat, raw).buttons & 0x4000) != 0, "composite hat mask uses any matching bit");
    const auto digital = mapping(std::string(id) + ",Synthetic,a:b0,+leftx:h0.2,-leftx:h0.8,+lefty:h0.4,-lefty:h0.1,");
    raw = {}; raw.hats[0] = 3;
    check(mapped_controller(digital, raw).left_x == 32767 && mapped_controller(digital, raw).left_y == 32767,
        "configurable half outputs permit D-pad-only controllers without guessing their layout");
    raw.hats[0] = 0;
    check(neutral(mapped_controller(digital, raw)), "released digital sticks are neutral");
    const auto opposed = mapping(std::string(id) + ",Synthetic,a:b0,+leftx:b1,-leftx:b2,lefty:a0,");
    raw = {}; raw.buttons[1] = raw.buttons[2] = true;
    const auto conflict = mapped_controller(opposed, raw);
    check(conflict.connection == Connection::Error && conflict.error == ERROR_INVALID_DATA,
        "opposing held directions cannot masquerade as neutral and authorize a release-to-move");
    raw.buttons[2] = false;
    check(mapped_controller(opposed, raw).left_x == 32767, "single direction decodes but requires fresh neutral after the error");
}
class Fake final : public ControllerBackend {
public:
    ControllerDevices devices{{ControllerDeviceId{1}, ControllerDeviceId{2}}, 2, 0};
    unsigned discoveries = 0;
    std::array<unsigned, slot_count> opens{}, samples{}, closes{};
    std::array<bool, slot_count> opened{};
    std::array<ControllerDeviceId, slot_count> identities{};
    DWORD open_error = 0, sample_error = 0;
    bool* revoke_discovery = nullptr;
    bool* revoke_open = nullptr;
    ControllerDevices discover() override { ++discoveries; if (revoke_discovery) { *revoke_discovery = false; } return devices; }
    std::uint32_t open(unsigned slot, const ControllerDeviceId& value) override
    {
        ++opens[slot]; identities[slot] = value; opened[slot] = !open_error;
        if (revoke_open) { *revoke_open = false; }
        return open_error;
    }
    Controller sample(unsigned slot) override
    {
        check(opened[slot], "only opened device can be sampled");
        ++samples[slot];
        Controller value;
        value.connection = sample_error ? Connection::Error : Connection::Connected;
        value.error = sample_error;
        value.left_x = 1234;
        return value;
    }
    void close(unsigned slot) override { ++closes[slot]; opened[slot] = false; }
    void describe(unsigned, Controller&) const override {}
};
void lifecycle()
{
    Fake fake;
    ControllerDevicesReader reader(fake);
    const auto first = reader.read(0, 1000);
    check(first.connection == Connection::Connected && fake.identities[0][0] == 1, "first available device assigned");
    check(fake.samples[1] == 0 && fake.samples[2] == 0 && fake.samples[3] == 0, "one-slot read never samples other slots");
    check(reader.read(1, 1000).connection == Connection::Connected && fake.identities[1][0] == 2, "second independent device");
    check(reader.read(2, 1000).connection == Connection::Disconnected && fake.discoveries == 1, "enumeration throttled and devices not duplicated");
    check(reader.read(0, 1016).connection_epoch == first.connection_epoch, "stable identity keeps epoch");
    fake.sample_error = ERROR_DEVICE_NOT_CONNECTED;
    const auto failed = reader.read(0, 1032);
    check(failed.connection == Connection::Error && !failed.left_x && !fake.opened[0], "failed read discards controls and closes device");
    fake.sample_error = 0;
    check(reader.read(0, 1048).connection == Connection::Error && fake.opens[0] == 1, "failed read is not immediately reacquired");
    const auto recovered = reader.read(0, 2048);
    check(recovered.connection == Connection::Connected && recovered.connection_epoch > failed.connection_epoch, "reacquire needs fresh neutral");
    fake.devices.ids[0] = {3};
    const auto replaced = reader.read(0, 3048);
    check(replaced.connection == Connection::Connected && replaced.connection_epoch > recovered.connection_epoch
        && fake.identities[0][0] == 3, "replacement between UI snapshots changes epoch");
    check(fake.opened[1], "unrelated assigned controller survives enumeration reordering");
    reader.suspend(0);
    check(!fake.opened[0] && fake.opened[1], "XInput preempts only its own slot");
    const auto returned = reader.read(0, 3064);
    check(returned.connection_epoch > replaced.connection_epoch, "source return cannot reuse old neutral");
    fake.devices.error = ERROR_ACCESS_DENIED;
    check(reader.read(0, 4048).error == ERROR_ACCESS_DENIED && !fake.opened[1], "enumeration failure is explicit and closes uncertain devices");
    fake.devices.error = 0; fake.devices.count = 33;
    check(reader.read(0, 5048).error == ERROR_MORE_DATA, "oversized backend list is bounded");
    fake.devices.count = 2; fake.devices.ids[1] = fake.devices.ids[0];
    check(reader.read(0, 6048).error == ERROR_DUP_NAME, "duplicate native identity is never assigned twice");
    fake.devices.count = 1; fake.open_error = ERROR_NOT_SUPPORTED;
    check(reader.read(0, 7048).error == ERROR_NOT_SUPPORTED, "unmapped controller is not success-shaped");
    fake.open_error = 0;
    const auto ready = reader.read(0, 8048);
    check(ready.connection == Connection::Connected, "configured controller can recover");
    check(reader.read(0, 8047).error == ERROR_INVALID_TIME && !fake.opened[0], "clock rollback clears native ownership");
    check(reader.read(0, 8064).connection_epoch > ready.connection_epoch, "rollback recovery advances epoch");
    check(reader.read(slot_count, 8080).error == ERROR_INVALID_PARAMETER, "invalid slot rejected");
    ControllerEpoch epoch;
    const auto hid = epoch.update(ready, ControllerApi::DirectInput);
    Controller xbox; xbox.connection = Connection::Connected; xbox.buttons = 0x1000;
    const auto switched = epoch.update(xbox, ControllerApi::XInput);
    check(switched.connection_epoch > hid.connection_epoch && switched.buttons == xbox.buttons,
        "source change cannot inherit held-control authorization or alter XInput mapping");
    check(epoch.update(xbox, ControllerApi::XInput).connection_epoch == switched.connection_epoch, "unchanged XInput epoch stable");
}
void database(const char* path)
{
    std::ifstream file(path);
    check(bool(file), "pinned mapping database available");
    unsigned windows = 0, supported = 0;
    std::map<std::wstring, unsigned> rejected;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#' || line.find("platform:Windows,") == line.npos || !controller_guid(line.substr(0, 32))) { continue; }
        ++windows;
        ControllerMapping parsed;
        const auto error = parse_controller_mapping(line, parsed);
        if (error.empty()) { ++supported; } else { ++rejected[error]; }
    }
    std::cout << "Bundled Windows mappings: " << supported << "/" << windows << " have supported pointer layouts.\n";
    for (const auto& [reason, count] : rejected) { std::wcout << count << L": " << reason << L'\n'; }
    check(windows > 500 && supported * 100 / windows >= 85, "broad database coverage, not a single-device profile");
}
void read_scope()
{
    bool allowed = true;
    const ControllerReadScope scope{&allowed, [](void* context) { return *static_cast<bool*>(context); }};
    Fake fake;
    fake.revoke_discovery = &allowed;
    ControllerDevicesReader reader(fake);
    check(reader.read(0, 1000, scope).error == ERROR_CANCELLED && fake.opens[0] == 0 && fake.samples[0] == 0,
        "expired original authorization after slow enumeration cannot open or sample a controller");
    allowed = true; fake.revoke_discovery = nullptr; fake.revoke_open = &allowed;
    check(reader.read(0, 1016, scope).error == ERROR_CANCELLED && fake.samples[0] == 0 && !fake.opened[0],
        "authorization lost during acquisition cannot begin a native sample");
    check(reader.read(1, 1032, scope).error == ERROR_CANCELLED && fake.opens[1] == 0, "terminal revocation prevents other slot work");
}
}
int main(int argc, char** argv)
{
    try { check(argc == 2, "mapping database argument required"); parsing(); normalization(); lifecycle(); read_scope(); database(argv[1]); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS generic mapping and device lifecycle; no native input.\n";
}
