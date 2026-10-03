#pragma once

// -----------------------------------------------------------------------------
//  Exact price literals for tests.
//
//  Price / Money are common::Decimal, an exact int64 count of millionths, so a test must
//  not write a price as a double and convert it (Decimal::from_double rounds). Instead:
//
//      101.25_px        a Price, read digit by digit from the literal's own text
//                       (101'250'000 millionths); a literal with more than 6 decimals, or
//                       one that does not fit int64, does not compile
//      units({3, 2})    a vector of whole-currency-unit Prices (3.000000, 2.000000)
//      px("101.25")     the same as a runtime call; throws std::invalid_argument
//      micros(1)        a Price of that many millionths: micros(1) is the smallest positive
//                       price (0.000001); micros(-5) is minus five millionths
//      kMaxPrice        the largest Price (INT64_MAX millionths); kMinPrice the smallest
//
//  Spell a `_px` literal WITHOUT digit separators ("1000.5_px", not "1'000.5_px").
// -----------------------------------------------------------------------------

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/decimal.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::test_support {

[[nodiscard]] constexpr common::Price micros(std::int64_t millionths) noexcept {
    return common::Price::from_micros(millionths);
}

inline constexpr common::Price kMaxPrice = micros(std::numeric_limits<std::int64_t>::max());
inline constexpr common::Price kMinPrice = micros(std::numeric_limits<std::int64_t>::min());

[[nodiscard]] inline common::Price px(std::string_view text) {
    common::Price value{};
    if (common::parse_decimal(text, value) != common::DecimalStatus::Ok) {
        throw std::invalid_argument("not an exact price: '" + std::string{text} + "'");
    }
    return value;
}

// Whole currency units, each as a Price: units({3, 2}) is {3.000000, 2.000000}.
[[nodiscard]] inline std::vector<common::Price> units(std::initializer_list<std::int64_t> whole) {
    std::vector<common::Price> out;
    out.reserve(whole.size());
    for (const std::int64_t each : whole) {
        out.push_back(common::Price::from_units(each));
    }
    return out;
}

inline namespace literals {

// The raw-literal form receives the literal's source text, so no double is ever involved.
[[nodiscard]] constexpr common::Price operator""_px(const char* text) {
    common::Price value{};
    if (common::parse_decimal(std::string_view{text}, value) != common::DecimalStatus::Ok) {
        throw std::invalid_argument("not an exact price literal");   // a compile error in a constant expression
    }
    return value;
}

}  // namespace literals

}  // namespace trading_engine::test_support
