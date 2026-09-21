#include "VoiceModel.h"

#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <fstream>

namespace overworld {
bool verified_voice_model(std::span<std::uint8_t> bytes, std::wstring& error)
{
    if (bytes.size() != voice_model_bytes) {
        error = L"Use the verified tiny.en model supplied with this build";
        return false;
    }
    constexpr std::array<std::uint8_t, 32> expected{
        0x92, 0x1e, 0x4c, 0xf8, 0x68, 0x6f, 0xdd, 0x99, 0x3d, 0xcd, 0x08, 0x1a, 0x5d, 0xa5, 0xb6, 0xc3,
        0x65, 0xbf, 0xde, 0x11, 0x62, 0xe7, 0x2b, 0x08, 0xd7, 0x5a, 0xc7, 0x52, 0x89, 0x92, 0x0b, 0x1f};
    std::array<std::uint8_t, 32> actual{};
    const NTSTATUS result = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(),
        static_cast<ULONG>(bytes.size()), actual.data(), static_cast<ULONG>(actual.size()));
    if (result < 0) { error = L"Local voice model SHA256 verification failed"; return false; }
    if (actual != expected) { error = L"Model is not the verified tiny.en release; recognition was not started"; return false; }
    error.clear();
    return true;
}

bool load_voice_model(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, std::wstring& error)
{
    bytes.clear();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != static_cast<std::streamoff>(voice_model_bytes)) {
        error = L"Verified tiny.en model missing or incorrect size";
        return false;
    }
    bytes.resize(voice_model_bytes);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(voice_model_bytes)) {
        bytes.clear();
        error = L"Local voice model could not be read completely";
        return false;
    }
    if (!verified_voice_model(bytes, error)) { bytes.clear(); return false; }
    return true;
}
} // namespace overworld
