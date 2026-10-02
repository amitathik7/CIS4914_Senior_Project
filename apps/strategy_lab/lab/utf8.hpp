#pragma once

// UTF-8 helpers. The tool only ever writes valid UTF-8 (JSON requires it), so every
// piece of text that comes from outside is checked on the way in.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace trading_engine::lab {

// Well-formed UTF-8 per Unicode table 3-7: no overlong forms, no surrogates, nothing
// above U+10FFFF, no stray continuation bytes. NUL is valid UTF-8 and is accepted here.
[[nodiscard]] inline bool is_valid_utf8(std::string_view text) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
    const auto continuation = [&](std::size_t i) { return i < text.size() && (byte(i) & 0xC0U) == 0x80U; };

    std::size_t i = 0;
    while (i < text.size()) {
        const std::uint8_t lead = byte(i);
        if (lead < 0x80U) {
            ++i;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            if (!continuation(i + 1)) return false;
            i += 2;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            if (!continuation(i + 1) || !continuation(i + 2)) return false;
            const std::uint8_t second = byte(i + 1);
            if (lead == 0xE0U && second < 0xA0U) return false;   // overlong
            if (lead == 0xEDU && second > 0x9FU) return false;   // surrogate
            i += 3;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            if (!continuation(i + 1) || !continuation(i + 2) || !continuation(i + 3)) return false;
            const std::uint8_t second = byte(i + 1);
            if (lead == 0xF0U && second < 0x90U) return false;   // overlong
            if (lead == 0xF4U && second > 0x8FU) return false;   // above U+10FFFF
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

// `text` with every byte that is not part of a well-formed sequence replaced by '?'. For
// the one place text of unknown origin must still be written as JSON: an error message.
[[nodiscard]] inline std::string to_valid_utf8(std::string_view text) {
    if (is_valid_utf8(text)) {
        return std::string{text};
    }
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t length = 0;
        for (std::size_t n = 1; n <= 4 && i + n <= text.size(); ++n) {
            if (is_valid_utf8(text.substr(i, n))) {
                length = n;
                break;
            }
        }
        if (length == 0) {
            out += '?';
            ++i;
        } else {
            out.append(text.substr(i, length));
            i += length;
        }
    }
    return out;
}

// At most `max_bytes` of `text`, never cutting a multi-byte character, with "..." appended
// when something was dropped. For quoting an offending value in a message.
[[nodiscard]] inline std::string utf8_excerpt(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string{text};
    }
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<std::uint8_t>(text[end]) & 0xC0U) == 0x80U) {
        --end;   // text[end] is a continuation byte: step back to a character boundary
    }
    return std::string{text.substr(0, end)} + "...";
}

}  // namespace trading_engine::lab
