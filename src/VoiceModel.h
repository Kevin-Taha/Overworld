#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace overworld {
constexpr std::size_t voice_model_bytes = 77704715;
// The initial feature accepts only the licensed, pinned tiny.en model. Verification
// and parser input use the same owned bytes, never a hash-then-reopen path.
[[nodiscard]] bool verified_voice_model(std::span<std::uint8_t> bytes, std::wstring& error);
[[nodiscard]] bool load_voice_model(const std::filesystem::path& path,
    std::vector<std::uint8_t>& bytes, std::wstring& error);
} // namespace overworld
