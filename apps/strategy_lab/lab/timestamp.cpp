#include "lab/timestamp.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>

namespace trading_engine::lab {

namespace {

constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
constexpr int kMinYear = 1970;
constexpr int kMaxYear = 2261;

constexpr bool is_leap(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

constexpr unsigned days_in_month(int year, unsigned month) noexcept {
    constexpr std::array<unsigned, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap(year) ? 29U : days[month - 1];
}

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm).
constexpr std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept {
    const std::int64_t y = static_cast<std::int64_t>(year) - (month <= 2 ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153U * (month > 2 ? month - 3 : month + 9) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct Civil {
    int      year;
    unsigned month;
    unsigned day;
};

constexpr Civil civil_from_days(std::int64_t days) noexcept {
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const unsigned mp = (5U * doy + 2U) / 153U;
    const unsigned day = doy - (153U * mp + 2U) / 5U + 1U;
    const unsigned month = mp < 10 ? mp + 3U : mp - 9U;
    return Civil{static_cast<int>(y + (month <= 2 ? 1 : 0)), month, day};
}

// Exactly `count` ASCII digits at text[pos..]; false if any is not a digit.
bool read_digits(std::string_view text, std::size_t pos, std::size_t count, unsigned& out) noexcept {
    unsigned value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10U + static_cast<unsigned>(c - '0');
    }
    out = value;
    return true;
}

std::optional<common::Timestamp> fail(std::string* why, std::string message) {
    if (why != nullptr) {
        *why = std::move(message);
    }
    return std::nullopt;
}

void append_padded(std::string& out, std::uint64_t value, std::size_t width) {
    std::string digits = std::to_string(value);
    out.append(digits.size() < width ? width - digits.size() : 0, '0');
    out += digits;
}

}  // namespace

std::optional<common::Timestamp> parse_utc_timestamp(std::string_view text, std::string* why) {
    // Built only on failure: a valid timestamp costs no allocation.
    const auto hint = [] {
        return std::string{" (expected YYYY-MM-DDTHH:MM:SS[.fffffffff]Z, UTC, upper-case T and Z)"};
    };

    if (text.size() < 20) {
        return fail(why, "not a UTC timestamp" + hint());
    }
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const bool shape_ok = read_digits(text, 0, 4, year) && text[4] == '-' && read_digits(text, 5, 2, month) &&
                          text[7] == '-' && read_digits(text, 8, 2, day) && text[10] == 'T' &&
                          read_digits(text, 11, 2, hour) && text[13] == ':' &&
                          read_digits(text, 14, 2, minute) && text[16] == ':' &&
                          read_digits(text, 17, 2, second);
    if (!shape_ok) {
        return fail(why, "not a UTC timestamp" + hint());
    }

    std::int64_t fraction_nanos = 0;
    std::size_t pos = 19;
    if (text[pos] == '.') {
        ++pos;
        std::size_t digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits == 9) {
                return fail(why, "more than nine fractional digits" + hint());
            }
            fraction_nanos = fraction_nanos * 10 + (text[pos] - '0');
            ++digits;
            ++pos;
        }
        if (digits == 0) {
            return fail(why, "'.' with no fractional digits" + hint());
        }
        for (std::size_t i = digits; i < 9; ++i) {
            fraction_nanos *= 10;
        }
    }
    if (pos >= text.size() || text[pos] != 'Z' || pos + 1 != text.size()) {
        return fail(why, "must end with 'Z' (UTC); offsets such as +00:00 are not accepted" + hint());
    }

    if (static_cast<int>(year) < kMinYear || static_cast<int>(year) > kMaxYear) {
        return fail(why, "year " + std::to_string(year) + " is outside " + std::to_string(kMinYear) + ".." +
                             std::to_string(kMaxYear));
    }
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(static_cast<int>(year), month)) {
        return fail(why, "not a real calendar date");
    }
    if (hour > 23 || minute > 59 || second > 59) {
        return fail(why, "time of day out of range (hours 00-23, minutes and seconds 00-59; no leap seconds)");
    }

    const std::int64_t days = days_from_civil(static_cast<int>(year), month, day);
    const std::int64_t seconds =
        days * 86400 + static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 +
        static_cast<std::int64_t>(second);
    return common::Timestamp{common::Duration{seconds * kNanosPerSecond + fraction_nanos}};
}

std::string format_utc_timestamp(common::Timestamp time) {
    const std::int64_t total = time.time_since_epoch().count();
    std::int64_t seconds = total / kNanosPerSecond;
    std::int64_t nanos   = total % kNanosPerSecond;
    if (nanos < 0) {   // floor, so a time before 1970 still prints a valid fraction
        nanos += kNanosPerSecond;
        --seconds;
    }
    std::int64_t days = seconds / 86400;
    std::int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    const Civil civil = civil_from_days(days);

    std::string out;
    out.reserve(30);
    append_padded(out, static_cast<std::uint64_t>(civil.year), 4);
    out += '-';
    append_padded(out, civil.month, 2);
    out += '-';
    append_padded(out, civil.day, 2);
    out += 'T';
    append_padded(out, static_cast<std::uint64_t>(rest / 3600), 2);
    out += ':';
    append_padded(out, static_cast<std::uint64_t>((rest % 3600) / 60), 2);
    out += ':';
    append_padded(out, static_cast<std::uint64_t>(rest % 60), 2);
    out += '.';
    append_padded(out, static_cast<std::uint64_t>(nanos), 9);
    out += 'Z';
    return out;
}

}  // namespace trading_engine::lab
