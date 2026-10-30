#include "trading_engine/common/timestamp_text.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace trading_engine::common {

namespace {

// Reads exactly `width` ASCII digits at text[pos..]; false on anything else.
bool read_digits(std::string_view text, std::size_t pos, std::size_t width, int& out) noexcept {
    if (pos + width > text.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

}  // namespace

std::string format_timestamp(Timestamp t) {
    using namespace std::chrono;
    const auto day        = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss<nanoseconds> time{t - day};

    char buffer[48];
    const int written = std::snprintf(
        buffer, sizeof buffer, "%04d-%02u-%02uT%02d:%02d:%02d.%09lldZ",
        static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
        static_cast<unsigned>(ymd.day()), static_cast<int>(time.hours().count()),
        static_cast<int>(time.minutes().count()), static_cast<int>(time.seconds().count()),
        static_cast<long long>(time.subseconds().count()));
    return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0U);
}

std::optional<Timestamp> parse_timestamp(std::string_view text) noexcept {
    using namespace std::chrono;
    // YYYY-MM-DDTHH:MM:SS is 19 characters, then [.fraction] and 'Z'.
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (text.size() < 20 || !read_digits(text, 0, 4, y) || text[4] != '-' ||
        !read_digits(text, 5, 2, mo) || text[7] != '-' || !read_digits(text, 8, 2, d) ||
        text[10] != 'T' || !read_digits(text, 11, 2, h) || text[13] != ':' ||
        !read_digits(text, 14, 2, mi) || text[16] != ':' || !read_digits(text, 17, 2, s)) {
        return std::nullopt;
    }

    std::int64_t nanos = 0;
    std::size_t pos = 19;
    if (text[pos] == '.') {
        ++pos;
        std::size_t digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (++digits > 9) {
                return std::nullopt;
            }
            nanos = nanos * 10 + (text[pos] - '0');
            ++pos;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        for (std::size_t i = digits; i < 9; ++i) {
            nanos *= 10;
        }
    }
    if (pos + 1 != text.size() || text[pos] != 'Z') {
        return std::nullopt;
    }

    const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)},
                             day{static_cast<unsigned>(d)}};
    if (!ymd.ok() || h > 23 || mi > 59 || s > 59) {
        return std::nullopt;
    }
    return Timestamp{sys_days{ymd}} + hours{h} + minutes{mi} + seconds{s} + nanoseconds{nanos};
}

}  // namespace trading_engine::common
