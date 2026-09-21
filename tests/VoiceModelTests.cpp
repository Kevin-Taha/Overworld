#include "VoiceModel.h"

#include <array>
#include <cstring>
#include <iostream>

int main()
{
    std::wstring error;
    std::array<std::uint8_t, 64> malicious{};
    constexpr std::int32_t dimensions[]{0x67676d6c, 51864, 1500, 384, 6, 4, 448, 384, 6, 4, -1, 1};
    std::memcpy(malicious.data(), dimensions, sizeof(dimensions));
    if (overworld::verified_voice_model(malicious, error) || error.empty()) {
        std::cerr << "FAIL malformed model dimensions reached acceptance\n"; return 1;
    }
    std::vector<std::uint8_t> unsupported(overworld::voice_model_bytes, 0);
    std::memcpy(unsupported.data(), dimensions, sizeof(dimensions));
    if (overworld::verified_voice_model(unsupported, error) || error.empty()) {
        std::cerr << "FAIL size-matching unsupported model accepted\n"; return 1;
    }
    unsupported.clear();
    if (overworld::load_voice_model({}, unsupported, error) || !unsupported.empty() || error.empty()) {
        std::cerr << "FAIL missing model accepted\n"; return 1;
    }
    std::cout << "PASS malformed/size-matching unsupported/missing model rejected before parser; no recognizer or capture linked\n";
}
