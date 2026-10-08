#pragma once

// Locale-independent number parsing for CSV cells and command-line parameters.

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

namespace trading_engine::lab {

// Parses ALL of `text` as a C-locale decimal or scientific double ("101.25", "1e-7",
// ".5"): no sign prefix other than '-', no spaces, no hexadecimal. "nan" and "inf"
// DO parse (std::from_chars accepts them); whether a non-finite value is acceptable is the
// caller's rule. On failure returns false and sets `why` to "is empty", "is not a number"
// or "is outside the range of a double" (a value that overflows or underflows past
// every double is never turned into infinity or zero).
[[nodiscard]] inline bool parse_double_text(std::string_view text, double& out, std::string& why) {
    if (text.empty()) {
        why = "is empty";
        return false;
    }
    const char* first = text.data();
    const char* last  = first + text.size();
    const auto result = std::from_chars(first, last, out, std::chars_format::general);
    if (result.ec == std::errc::result_out_of_range) {
        why = "is outside the range of a double";
        return false;
    }
    if (result.ec != std::errc{} || result.ptr != last) {
        why = "is not a number";
        return false;
    }
    return true;
}

}  // namespace trading_engine::lab
