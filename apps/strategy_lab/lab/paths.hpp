#pragma once

// Paths and UTF-8 text. The tool treats every command-line value and every file as
// UTF-8 (the JSON it writes must be valid UTF-8), so paths cross between std::string
// and std::filesystem::path through char8_t explicitly. (std::filesystem::u8path is
// deprecated in C++20 and a path built from a narrow string would use the platform's
// code page.)

#include <filesystem>
#include <string>
#include <string_view>

namespace trading_engine::lab {

[[nodiscard]] inline std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
}

}  // namespace trading_engine::lab
