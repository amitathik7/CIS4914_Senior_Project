#pragma once

// Byte-exact output. The tool's stdout is a JSON document that must not be altered on the
// way out: on Windows the C runtime's text mode would turn every "\n" into "\r\n".

#include <cstdio>
#include <string_view>

namespace trading_engine::lab {

// Switches `stream` to binary mode where the platform distinguishes (Windows); no-op elsewhere.
void set_binary_mode(std::FILE* stream) noexcept;

// Writes every byte and flushes. False if the stream refused any of it (a closed pipe, a
// full disk): the caller must not report success.
[[nodiscard]] bool write_all(std::FILE* stream, std::string_view bytes) noexcept;

}  // namespace trading_engine::lab
