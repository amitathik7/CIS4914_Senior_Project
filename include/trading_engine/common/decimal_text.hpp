#pragma once

// -----------------------------------------------------------------------------
//  Exact decimal TEXT <-> common::Decimal (Money / Price) and Quantity.
//
//  Responsibility: convert between the project's fixed-point money types and their decimal
//  text WITHOUT ever passing through floating point, so a value read from a file or written
//  to a log or a JSON document is exactly the integer that is stored. Only the standard
//  library and decimal.hpp are used. Everything here is a pure function.
//
//  Text syntax accepted by parse_scaled() / parse_decimal(): plain decimal only, in the C
//  locale ("101.25", "3", ".5", "7."). No sign, no spaces, no exponent ("1e3"), no
//  hexadecimal, no "nan"/"inf". There is NO rounding: a value with more fractional
//  digits than the scale holds is accepted only if every extra digit is '0'
//  ("1.2500000" is 1.25); otherwise it is TooManyDecimals. A value that does not fit
//  int64 is OutOfRange. Nothing is clamped or repaired.
//
//  (Decimal::from_double() rounds and is for inputs that arrive as doubles; nothing here
//  uses it. A price read from text is never a double on the way.)
// -----------------------------------------------------------------------------

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "trading_engine/common/decimal.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::common {

// Digits after the point in a Money / Price's decimal text (Decimal is whole millionths).
inline constexpr int kDecimalPlaces = 6;
static_assert(Decimal::kScale == 1'000'000, "kDecimalPlaces below must match Decimal::kScale");

enum class DecimalStatus : std::uint8_t {
    Ok,
    Empty,             // no text
    NotADecimal,       // anything but digits and at most one point (and at least one digit)
    TooManyDecimals,   // a non-zero digit beyond the scale
    OutOfRange,        // does not fit int64 at this scale
};

// Parses ALL of `text` as a non-negative decimal into `out`, scaled by 10^decimals
// (decimals in [0, 18]). On failure `out` is left unchanged. Negative values are not
// parsed here: every field this serves (price, volume) is non-negative in text.
[[nodiscard]] constexpr DecimalStatus parse_scaled(std::string_view text, int decimals,
                                                   std::int64_t& out) noexcept {
    if (text.empty()) {
        return DecimalStatus::Empty;
    }
    if (decimals < 0 || decimals > 18) {
        return DecimalStatus::NotADecimal;
    }
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    std::int64_t value          = 0;
    bool         seen_point     = false;
    bool         any_digit      = false;
    int          fraction_digits = 0;
    for (const char c : text) {
        if (c == '.') {
            if (seen_point) {
                return DecimalStatus::NotADecimal;
            }
            seen_point = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return DecimalStatus::NotADecimal;
        }
        any_digit = true;
        const int digit = c - '0';
        if (seen_point) {
            ++fraction_digits;
            if (fraction_digits > decimals) {
                if (digit != 0) {
                    return DecimalStatus::TooManyDecimals;
                }
                continue;   // an extra zero changes nothing
            }
        }
        if (value > (kMax - digit) / 10) {
            return DecimalStatus::OutOfRange;
        }
        value = value * 10 + digit;
    }
    if (!any_digit) {
        return DecimalStatus::NotADecimal;
    }
    // Pad the missing fractional digits: "1.5" at 6 decimals is 1'500'000.
    for (int i = (seen_point ? (fraction_digits < decimals ? fraction_digits : decimals) : 0); i < decimals; ++i) {
        if (value > kMax / 10) {
            return DecimalStatus::OutOfRange;
        }
        value *= 10;
    }
    out = value;
    return DecimalStatus::Ok;
}

// The exact shortest decimal text of `scaled` / 10^decimals: no trailing zeros, no
// trailing point, a leading '-' for negatives. 150'020'000 at 6 decimals is "150.02",
// 3'000'000 is "3", -1'500'000 is "-1.5", 1 is "0.000001". Works for every int64.
[[nodiscard]] inline std::string format_scaled(std::int64_t scaled, int decimals) {
    const bool negative = scaled < 0;
    // Negate in unsigned arithmetic: -INT64_MIN does not fit in int64.
    std::uint64_t magnitude = negative ? (~static_cast<std::uint64_t>(scaled) + 1U)
                                       : static_cast<std::uint64_t>(scaled);
    std::string digits = std::to_string(magnitude);
    std::string out;
    if (decimals <= 0) {
        out = digits;
    } else {
        if (digits.size() <= static_cast<std::size_t>(decimals)) {
            digits.insert(0, static_cast<std::size_t>(decimals) + 1 - digits.size(), '0');
        }
        const std::size_t point = digits.size() - static_cast<std::size_t>(decimals);
        std::string fraction    = digits.substr(point);
        while (!fraction.empty() && fraction.back() == '0') {
            fraction.pop_back();
        }
        out = digits.substr(0, point);
        if (!fraction.empty()) {
            out += '.';
            out += fraction;
        }
    }
    return negative ? "-" + out : out;
}

// Text -> Money / Price (non-negative, six decimals). On failure `out` is left unchanged.
[[nodiscard]] constexpr DecimalStatus parse_decimal(std::string_view text, Decimal& out) noexcept {
    std::int64_t micros = 0;
    const DecimalStatus status = parse_scaled(text, kDecimalPlaces, micros);
    if (status == DecimalStatus::Ok) {
        out = Decimal::from_micros(micros);
    }
    return status;
}

// Text -> a whole number of shares (no point needed; "1500.0" is accepted, "1500.5" is not).
[[nodiscard]] constexpr DecimalStatus parse_quantity(std::string_view text, Quantity& out) noexcept {
    return parse_scaled(text, 0, out);
}

// The shortest exact text of a Money / Price: 150.02, 3, 0.000001, -1.5 (never "150.020000").
[[nodiscard]] inline std::string format_decimal(Decimal value) {
    return format_scaled(value.micros(), kDecimalPlaces);
}

[[nodiscard]] inline std::string format_quantity(Quantity quantity) { return std::to_string(quantity); }

}  // namespace trading_engine::common
