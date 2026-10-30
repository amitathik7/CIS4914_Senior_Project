// RFC 3339 text <-> Timestamp, used by the event wire format and the CLI's CSV reader.

#include <gtest/gtest.h>

#include <chrono>

#include "trading_engine/common/timestamp_text.hpp"

namespace {

using trading_engine::common::format_timestamp;
using trading_engine::common::parse_timestamp;
using trading_engine::common::Timestamp;
using namespace std::chrono;

TEST(TimestampText, FormatsWithNineFractionalDigitsAndZ) {
    const Timestamp t = Timestamp{sys_days{2026y / January / 5}} + 14h + 30min + 1s + 7ns;
    EXPECT_EQ(format_timestamp(t), "2026-01-05T14:30:01.000000007Z");
    EXPECT_EQ(format_timestamp(Timestamp{}), "1970-01-01T00:00:00.000000000Z");
}

TEST(TimestampText, RoundTripsExactly) {
    for (const Timestamp t : {Timestamp{}, Timestamp{nanoseconds{1}},
                              Timestamp{sys_days{2024y / February / 29}} + 23h + 59min + 59s +
                                  999'999'999ns,
                              Timestamp{sys_days{1969y / December / 31}} + 12h}) {
        const auto back = parse_timestamp(format_timestamp(t));
        ASSERT_TRUE(back.has_value()) << format_timestamp(t);
        EXPECT_EQ(*back, t);
    }
}

TEST(TimestampText, AcceptsShortOrMissingFractions) {
    const Timestamp base = Timestamp{sys_days{2026y / January / 5}} + 14h + 30min;
    EXPECT_EQ(parse_timestamp("2026-01-05T14:30:00Z"), base);
    EXPECT_EQ(parse_timestamp("2026-01-05T14:30:00.5Z"), base + 500ms);
    EXPECT_EQ(parse_timestamp("2026-01-05T14:30:00.000001Z"), base + 1us);
}

TEST(TimestampText, RefusesAnythingAmbiguousOrMalformed) {
    for (const char* bad : {"", "2026-01-05", "2026-01-05 14:30:00Z", "2026-01-05T14:30:00",
                            "2026-01-05T14:30:00+00:00", "2026-01-05t14:30:00z",
                            "2026-02-30T00:00:00Z", "2026-13-01T00:00:00Z",
                            "2026-01-05T24:00:00Z", "2026-01-05T14:30:60Z",
                            "2026-01-05T14:30:00.Z", "2026-01-05T14:30:00.1234567890Z",
                            "2026-01-05T14:30:00ZZ", "20x6-01-05T14:30:00Z"}) {
        EXPECT_FALSE(parse_timestamp(bad).has_value()) << bad;
    }
}

}  // namespace
