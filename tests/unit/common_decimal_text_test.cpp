// common/decimal_text.hpp: exact decimal TEXT <-> common::Decimal (Money / Price) and Quantity.
// The Decimal class itself is in common_decimal_test.cpp. No floating point is involved anywhere
// here, so every expectation is an integer count of millionths or a string, and the round trip
// is checked over the whole int64 range.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "trading_engine/common/decimal.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/types.hpp"

namespace common = trading_engine::common;
using common::Decimal;
using common::DecimalStatus;

namespace {

struct Parsed {
    DecimalStatus status;
    std::int64_t  micros;
};

Parsed price(std::string_view text) {
    Decimal value = Decimal::from_micros(-12345);   // a sentinel: it must survive a failed parse unchanged
    const DecimalStatus status = common::parse_decimal(text, value);
    if (status != DecimalStatus::Ok) {
        EXPECT_EQ(value.micros(), -12345) << "a failed parse must leave the output alone: '" << text << "'";
    }
    return {status, value.micros()};
}

Decimal micros(std::int64_t value) { return Decimal::from_micros(value); }

}  // namespace

// --- the types and the scale -----------------------------------------------------------------

TEST(CommonMoneyTypes, MoneyAndPriceAreExactDecimalsAndQuantityIsAnInt64) {
    static_assert(std::is_same_v<common::Price, Decimal>);
    static_assert(std::is_same_v<common::Money, Decimal>);
    static_assert(std::is_same_v<common::Quantity, std::int64_t>);
    static_assert(!std::is_floating_point_v<common::Price>);
    static_assert(!std::is_floating_point_v<common::Money>);
    static_assert(!std::is_floating_point_v<common::Quantity>);
    static_assert(!std::is_convertible_v<double, common::Price>, "no implicit conversion from a double");
    static_assert(!std::is_convertible_v<std::int64_t, common::Price>, "no implicit conversion from a bare integer");
    EXPECT_EQ(Decimal::kScale, 1'000'000);
    EXPECT_EQ(common::kDecimalPlaces, 6) << "the text helpers assume six places";
    // A price times a whole number of shares is exactly a Money: no rescaling, no rounding.
    EXPECT_EQ(common::Price::from_units(150) * common::Quantity{3}, common::Money::from_units(450));
}

// --- parsing ---------------------------------------------------------------------------------

TEST(CommonDecimalParse, ReadsPlainDecimalsExactly) {
    EXPECT_EQ(price("150.02").micros, 150'020'000);
    EXPECT_EQ(price("3").micros, 3'000'000);
    EXPECT_EQ(price("0.000001").micros, 1) << "one millionth";
    EXPECT_EQ(price("0.1").micros, 100'000);
    EXPECT_EQ(price("123456789.123456").micros, 123'456'789'123'456) << "a double would round this";
    EXPECT_EQ(price(".5").micros, 500'000) << "no integer digits";
    EXPECT_EQ(price("7.").micros, 7'000'000) << "no fraction digits";
    EXPECT_EQ(price("0").micros, 0);
    EXPECT_EQ(price("0.000000").micros, 0);
    EXPECT_EQ(price("007.5").micros, 7'500'000) << "leading zeros";
    EXPECT_EQ(price("0.1").micros, Decimal::from_double(0.1).micros()) << "agrees with the rounding conversion where that is exact";
}

TEST(CommonDecimalParse, ExtraTrailingZerosBeyondTheScaleChangeNothing) {
    EXPECT_EQ(price("1.2500000").micros, 1'250'000);
    EXPECT_EQ(price("1.250000000000").micros, 1'250'000);
    EXPECT_EQ(price("0.0000000").micros, 0);
}

TEST(CommonDecimalParse, VeryLongRunsOfZerosAreHarmlessAndNeverOverflowOrRound) {
    // Leading zeros do not count towards the magnitude, so a long run of them is just a small number...
    EXPECT_EQ(price(std::string(1000, '0') + "1.5").micros, 1'500'000);
    EXPECT_EQ(price(std::string(25, '0') + "9223372036854.775807").status, DecimalStatus::Ok);
    // ... and a long run of trailing fractional zeros changes nothing, but one non-zero digit after it is refused.
    EXPECT_EQ(price("1." + std::string(1000, '0')).micros, 1'000'000);
    EXPECT_EQ(price("9223372036854.775807" + std::string(500, '0')).micros, std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(price("1." + std::string(1000, '0') + "1").status, DecimalStatus::TooManyDecimals);
    // A long run of NINES (a real magnitude) overflows cleanly rather than wrapping.
    EXPECT_EQ(price(std::string(30, '9')).status, DecimalStatus::OutOfRange);
    EXPECT_EQ(price("." + std::string(30, '9')).status, DecimalStatus::TooManyDecimals);
}

TEST(CommonDecimalParse, RefusesWhatCannotBeHeldExactlyInsteadOfRounding) {
    EXPECT_EQ(price("0.0000001").status, DecimalStatus::TooManyDecimals);
    EXPECT_EQ(price("1.0000001").status, DecimalStatus::TooManyDecimals);
    EXPECT_EQ(price("1.2500001").status, DecimalStatus::TooManyDecimals) << "a non-zero digit after zeros";
    EXPECT_EQ(price("123456789.1234567").status, DecimalStatus::TooManyDecimals);
}

TEST(CommonDecimalParse, TheInt64BoundaryIsExact) {
    EXPECT_EQ(price("9223372036854.775807").status, DecimalStatus::Ok);
    EXPECT_EQ(price("9223372036854.775807").micros, std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(price("9223372036854.775808").status, DecimalStatus::OutOfRange) << "INT64_MAX + 1 millionth";
    EXPECT_EQ(price("9223372036855").status, DecimalStatus::OutOfRange);
    EXPECT_EQ(price("99999999999999999999").status, DecimalStatus::OutOfRange);
    EXPECT_EQ(price("9223372036854.775807000").status, DecimalStatus::Ok) << "trailing zeros past the scale are free";

    // Quantity (whole shares) takes whole numbers up to INT64_MAX.
    common::Quantity quantity = 0;
    EXPECT_EQ(common::parse_quantity("9223372036854775807", quantity), DecimalStatus::Ok);
    EXPECT_EQ(quantity, std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(common::parse_quantity("9223372036854775808", quantity), DecimalStatus::OutOfRange);
    EXPECT_EQ(common::parse_quantity("1500.0", quantity), DecimalStatus::Ok);
    EXPECT_EQ(quantity, 1500);
    EXPECT_EQ(common::parse_quantity("1500.5", quantity), DecimalStatus::TooManyDecimals) << "a share is whole";
    EXPECT_EQ(quantity, 1500) << "unchanged after the failure";
}

TEST(CommonDecimalParse, RefusesEverythingThatIsNotAPlainDecimal) {
    const std::vector<std::string> not_decimals{
        "nan", "NaN", "inf", "-inf", "infinity", "1e3", "1E3", "1e-7", "1e999", "0x10", "abc", "1.5x",
        " 1", "1 ", "+1", "-1", "-0", "--1", ".", "..", "1.2.3", "1,5", "1_000", "1'000", "\t1"};
    for (const std::string& text : not_decimals) {
        EXPECT_EQ(price(text).status, DecimalStatus::NotADecimal) << "'" << text << "'";
    }
    EXPECT_EQ(price("").status, DecimalStatus::Empty);
}

TEST(CommonDecimalParse, ABadDecimalCountIsRefused) {
    std::int64_t value = 7;
    EXPECT_EQ(common::parse_scaled("1", -1, value), DecimalStatus::NotADecimal);
    EXPECT_EQ(common::parse_scaled("1", 19, value), DecimalStatus::NotADecimal);
    EXPECT_EQ(value, 7);
    EXPECT_EQ(common::parse_scaled("1.5", 18, value), DecimalStatus::Ok);
    EXPECT_EQ(value, 1'500'000'000'000'000'000LL);
    EXPECT_EQ(common::parse_scaled("9.5", 18, value), DecimalStatus::OutOfRange) << "9.5e18 does not fit int64";
}

TEST(CommonDecimalParse, IsUsableInAConstantExpression) {
    constexpr auto parse = [](std::string_view text) {
        Decimal value{};
        return common::parse_decimal(text, value) == DecimalStatus::Ok ? value.micros() : std::int64_t{-1};
    };
    static_assert(parse("150.02") == 150'020'000);
    static_assert(parse("0.0000001") == -1);
    static_assert(parse("9223372036854.775808") == -1);
    SUCCEED();
}

// --- formatting ------------------------------------------------------------------------------

TEST(CommonDecimalFormat, WritesTheShortestExactText) {
    EXPECT_EQ(common::format_decimal(micros(150'020'000)), "150.02");
    EXPECT_EQ(common::format_decimal(micros(3'000'000)), "3");
    EXPECT_EQ(common::format_decimal(micros(1)), "0.000001");
    EXPECT_EQ(common::format_decimal(micros(10)), "0.00001");
    EXPECT_EQ(common::format_decimal(micros(100'000)), "0.1");
    EXPECT_EQ(common::format_decimal(Decimal{}), "0");
    EXPECT_EQ(common::format_decimal(micros(-1'500'000)), "-1.5");
    EXPECT_EQ(common::format_decimal(micros(-1)), "-0.000001");
    EXPECT_EQ(common::format_decimal(common::Money::from_units(100'000)), "100000");
    EXPECT_EQ(common::format_quantity(42), "42");
    EXPECT_EQ(common::format_quantity(-7), "-7");
    EXPECT_EQ(common::format_scaled(42, 0), "42");
    EXPECT_EQ(common::format_scaled(5, 3), "0.005");
}

TEST(CommonDecimalFormat, EveryInt64ExtremeKeepsEveryDigit) {
    EXPECT_EQ(common::format_decimal(micros(std::numeric_limits<std::int64_t>::max())), "9223372036854.775807");
    EXPECT_EQ(common::format_decimal(micros(std::numeric_limits<std::int64_t>::min())), "-9223372036854.775808")
        << "INT64_MIN has no positive twin: the magnitude is negated in unsigned arithmetic";
    EXPECT_EQ(common::format_quantity(std::numeric_limits<std::int64_t>::max()), "9223372036854775807");
    EXPECT_EQ(common::format_quantity(std::numeric_limits<std::int64_t>::min()), "-9223372036854775808");
}

TEST(CommonDecimalFormat, ParsingTheFormattedTextGivesBackTheSameDecimal) {
    // A spread across the range plus the edges, from a fixed xorshift (the sequence never changes).
    std::vector<std::int64_t> samples{0, 1, 9, 10, 99, 100, 999'999, 1'000'000, 1'000'001, 150'020'000,
                                      std::numeric_limits<std::int64_t>::max(),
                                      std::numeric_limits<std::int64_t>::max() - 1};
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 2000; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        samples.push_back(static_cast<std::int64_t>(state >> 1));              // non-negative, full width
        samples.push_back(static_cast<std::int64_t>(state >> (1 + i % 60)));   // every magnitude
    }
    for (const std::int64_t value : samples) {
        const std::string text = common::format_decimal(micros(value));
        Decimal back{};
        ASSERT_EQ(common::parse_decimal(text, back), DecimalStatus::Ok) << text;
        EXPECT_EQ(back.micros(), value) << text;
    }
}

TEST(CommonDecimalFormat, FormattedPricesNeverUseAnExponentOrALocaleSeparator) {
    for (const std::int64_t value : {std::int64_t{1}, std::int64_t{123}, std::int64_t{1'234'567'890'123'456},
                                     std::numeric_limits<std::int64_t>::max()}) {
        const std::string text = common::format_decimal(micros(value));
        EXPECT_EQ(text.find_first_not_of("0123456789."), std::string::npos) << text;
        EXPECT_LE(std::count(text.begin(), text.end(), '.'), 1) << text;
    }
}
