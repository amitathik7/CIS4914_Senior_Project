// The Strategy Lab's building blocks that have a standard to be checked against: UTF-8
// validity, RFC 3339 time, SHA-256 and the JSON writer. Expected values are the
// standards' own published vectors and hand-computed epoch seconds, not output from this code.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <locale>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lab/errors.hpp"
#include "lab/io.hpp"
#include "lab/json_writer.hpp"
#include "lab/sha256.hpp"
#include "lab/timestamp.hpp"
#include "lab/utf8.hpp"

namespace {

using namespace trading_engine::lab;
namespace common = trading_engine::common;

// ---- UTF-8 ------------------------------------------------------------------------------

TEST(LabUtf8, AcceptsWellFormedTextOfEveryLength) {
    EXPECT_TRUE(is_valid_utf8(""));
    EXPECT_TRUE(is_valid_utf8("plain ASCII"));
    EXPECT_TRUE(is_valid_utf8("\xC3\xA9"));                   // U+00E9, two bytes
    EXPECT_TRUE(is_valid_utf8("\xE2\x82\xAC"));               // U+20AC, three bytes
    EXPECT_TRUE(is_valid_utf8("\xF0\x9F\x98\x80"));           // U+1F600, four bytes
    EXPECT_TRUE(is_valid_utf8("\xF4\x8F\xBF\xBF"));           // U+10FFFF, the last code point
    EXPECT_TRUE(is_valid_utf8("\xED\x9F\xBF"));               // U+D7FF, just below the surrogates
}

TEST(LabUtf8, RejectsEveryMalformedForm) {
    EXPECT_FALSE(is_valid_utf8("\x80"));                      // stray continuation byte
    EXPECT_FALSE(is_valid_utf8("\xC0\x80"));                  // overlong NUL
    EXPECT_FALSE(is_valid_utf8("\xC1\xBF"));                  // overlong
    EXPECT_FALSE(is_valid_utf8("\xE0\x80\x80"));              // overlong three-byte
    EXPECT_FALSE(is_valid_utf8("\xED\xA0\x80"));              // a surrogate, U+D800
    EXPECT_FALSE(is_valid_utf8("\xF0\x80\x80\x80"));          // overlong four-byte
    EXPECT_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));          // above U+10FFFF
    EXPECT_FALSE(is_valid_utf8("\xF5\x80\x80\x80"));          // a lead byte that never occurs
    EXPECT_FALSE(is_valid_utf8("\xFF"));
    EXPECT_FALSE(is_valid_utf8("\xE2\x82"));                  // truncated
    EXPECT_FALSE(is_valid_utf8("ab\xC3"));                    // truncated at the end
}

TEST(LabUtf8, ExcerptNeverCutsACharacterAndSanitizingReplacesOnlyTheBadBytes) {
    EXPECT_EQ(utf8_excerpt("short", 40), "short");
    EXPECT_EQ(utf8_excerpt("abcdef", 3), "abc...");
    // "a" + U+00E9 (two bytes) + "b": cutting at 2 bytes would split the e-acute.
    EXPECT_EQ(utf8_excerpt("a\xC3\xA9" "b", 2), "a...");
    EXPECT_TRUE(is_valid_utf8(utf8_excerpt("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", 5)));

    EXPECT_EQ(to_valid_utf8("fine \xC3\xA9"), "fine \xC3\xA9");
    EXPECT_EQ(to_valid_utf8("a\xFF" "b"), "a?b");
    EXPECT_EQ(to_valid_utf8("\xE2\x82"), "??");
    EXPECT_TRUE(is_valid_utf8(to_valid_utf8("\x80\xC0\xED\xA0\x80 end")));
}

// ---- timestamps ---------------------------------------------------------------------------

std::int64_t seconds_of(const char* text) {
    std::string why;
    const auto parsed = parse_utc_timestamp(text, &why);
    EXPECT_TRUE(parsed.has_value()) << text << ": " << why;
    return parsed.has_value()
               ? std::chrono::duration_cast<std::chrono::seconds>(parsed->time_since_epoch()).count()
               : 0;
}

TEST(LabTimestamp, ParsesKnownInstantsToTheirEpochSeconds) {
    // Epoch seconds worked out by hand: 2026-01-01 is 1767225600 (2025-01-01 is 1735689600 plus
    // 365 days); 2026-01-05T14:30 adds 4 days and 14.5 hours. 2000-02-29T12:00 is 946684800 + 59
    // days + 12 hours.
    EXPECT_EQ(seconds_of("1970-01-01T00:00:00Z"), 0);
    EXPECT_EQ(seconds_of("2026-01-05T14:30:00Z"), 1767623400);
    EXPECT_EQ(seconds_of("2000-02-29T12:00:00Z"), 951825600);
    EXPECT_EQ(seconds_of("2024-02-29T00:00:00Z"), 1709164800);   // a leap day exists in 2024
}

TEST(LabTimestamp, FractionalSecondsScaleToNanoseconds) {
    const auto nanos = [](const char* text) {
        return parse_utc_timestamp(text)->time_since_epoch().count() % 1'000'000'000;
    };
    EXPECT_EQ(nanos("2026-01-05T14:30:00.5Z"), 500'000'000);
    EXPECT_EQ(nanos("2026-01-05T14:30:00.123456789Z"), 123'456'789);
    EXPECT_EQ(nanos("2026-01-05T14:30:00.000000001Z"), 1);
    EXPECT_EQ(nanos("2026-01-05T14:30:00.12Z"), 120'000'000);
}

TEST(LabTimestamp, EveryMalformedOrAmbiguousFormIsRejectedWithAReason) {
    const std::vector<const char*> bad{
        "",
        "2026-01-05",
        "2026-01-05T14:30:00",             // no zone
        "2026-01-05t14:30:00Z",            // lower-case t
        "2026-01-05T14:30:00z",            // lower-case z
        "2026-01-05T14:30:00+00:00",       // an offset is never accepted, not even a zero one
        "2026-01-05T14:30:00-05:00",
        "2026-01-05 14:30:00Z",
        "2026-01-05T14:30:00.Z",           // a point with no digits
        "2026-01-05T14:30:00.1234567890Z", // ten fractional digits
        "2026-01-05T14:30:00Zjunk",
        "2026-01-05T14:30:00ZZ",
        "2026-13-05T14:30:00Z",            // month 13
        "2026-00-05T14:30:00Z",
        "2026-01-00T14:30:00Z",
        "2026-01-32T14:30:00Z",
        "2026-04-31T14:30:00Z",            // April has 30 days
        "2026-02-29T00:00:00Z",            // 2026 is not a leap year
        "1900-02-29T00:00:00Z",            // nor is 1900
        "2026-01-05T24:00:00Z",
        "2026-01-05T14:60:00Z",
        "2026-01-05T14:30:60Z",            // no leap seconds
        "1969-12-31T23:59:59Z",            // before the epoch
        "2262-01-01T00:00:00Z",            // after what a signed 64-bit nanosecond count holds
        "20260105T143000Z",
        "2026-1-5T14:30:00Z",
        "2026-01-05T14:30:0aZ",
    };
    for (const char* text : bad) {
        std::string why;
        EXPECT_FALSE(parse_utc_timestamp(text, &why).has_value()) << "'" << text << "' was accepted";
        EXPECT_FALSE(why.empty()) << "'" << text << "' gave no reason";
    }
    EXPECT_TRUE(parse_utc_timestamp("2000-02-29T00:00:00Z").has_value()) << "2000 is a leap year";
    EXPECT_TRUE(parse_utc_timestamp("2261-12-31T23:59:59.999999999Z").has_value());
    EXPECT_TRUE(parse_utc_timestamp("1970-01-01T00:00:00Z").has_value());
}

TEST(LabTimestamp, FormattingRoundTripsAndAlwaysPrintsNineDigits) {
    EXPECT_EQ(format_utc_timestamp(common::Timestamp{}), "1970-01-01T00:00:00.000000000Z");
    EXPECT_EQ(format_utc_timestamp(*parse_utc_timestamp("2026-01-05T14:30:00Z")), "2026-01-05T14:30:00.000000000Z");
    EXPECT_EQ(format_utc_timestamp(*parse_utc_timestamp("2000-02-29T23:59:59.5Z")), "2000-02-29T23:59:59.500000000Z");
    // Before 1970 a floor division keeps the fraction valid.
    EXPECT_EQ(format_utc_timestamp(common::Timestamp{common::Duration{-1}}), "1969-12-31T23:59:59.999999999Z");

    // Round trip over a spread of instants, including every month boundary of a leap year.
    for (std::int64_t step = 0; step < 4000; ++step) {
        const std::int64_t nanos = step * 7'919'000'000'013LL;   // an odd stride, to vary every field
        const common::Timestamp time{common::Duration{nanos}};
        const std::string text = format_utc_timestamp(time);
        const auto parsed      = parse_utc_timestamp(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(*parsed, time) << text;
    }
}

// ---- SHA-256 (FIPS 180-4 / NIST example vectors) ---------------------------------------------

TEST(LabSha256, MatchesThePublishedVectors) {
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha256_hex(std::string(1'000'000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(LabSha256, PaddingBoundariesAroundTheBlockSizeAreDistinctAndStable) {
    // 55, 56 and 63/64/65 bytes straddle the point where the length no longer fits the last block.
    std::vector<std::string> digests;
    for (const std::size_t size : {0U, 1U, 55U, 56U, 57U, 63U, 64U, 65U, 119U, 120U, 128U}) {
        const std::string text(size, 'x');
        const std::string digest = sha256_hex(text);
        EXPECT_EQ(digest.size(), 64u);
        EXPECT_EQ(digest, sha256_hex(text)) << "deterministic";
        digests.push_back(digest);
    }
    std::sort(digests.begin(), digests.end());
    EXPECT_EQ(std::adjacent_find(digests.begin(), digests.end()), digests.end()) << "no two sizes collide";
    // 56 'a' bytes: a published two-block padding example.
    EXPECT_EQ(sha256_hex(std::string(56, 'a')), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

// ---- JSON writer ---------------------------------------------------------------------------

TEST(LabJsonWriter, WritesNestedStructureWithoutWhitespaceInCompactMode) {
    JsonWriter w;
    w.begin_object();
    w.field("a", "x").field("b", true);
    w.key("list").begin_array().integer(-5).unsigned_integer(7).number(0.5).null().end_array();
    w.key("empty_object").begin_object().end_object();
    w.key("empty_array").begin_array().end_array();
    w.end_object();
    EXPECT_EQ(w.str(), R"({"a":"x","b":true,"list":[-5,7,0.5,null],"empty_object":{},"empty_array":[]})");
}

TEST(LabJsonWriter, PrettyModeIndentsTwoSpacesAndNeverEmitsCarriageReturns) {
    JsonWriter w{true};
    w.begin_object();
    w.field("a", 1);
    w.key("list").begin_array().string("p").string("q").end_array();
    w.key("empty").begin_array().end_array();
    w.end_object();
    EXPECT_EQ(w.str(),
              "{\n"
              "  \"a\": 1,\n"
              "  \"list\": [\n"
              "    \"p\",\n"
              "    \"q\"\n"
              "  ],\n"
              "  \"empty\": []\n"
              "}");
    EXPECT_EQ(w.str().find('\r'), std::string::npos);
}

TEST(LabJsonWriter, EscapesQuotesBackslashesControlCharactersAndLineSeparators) {
    JsonWriter w;
    w.begin_array();
    w.string("say \"hi\" \\ done");
    w.string("tab\there\nnew\rline\b\f");
    w.string(std::string("nul\0byte", 8));
    w.string("\x01\x1f\x7f");
    w.string("\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");   // e-acute, euro, emoji: written as is
    w.string("\xE2\x80\xA8\xE2\x80\xA9");                 // U+2028, U+2029: escaped for JavaScript's sake
    w.end_array();
    EXPECT_EQ(w.str(),
              "[\"say \\\"hi\\\" \\\\ done\","
              "\"tab\\there\\nnew\\rline\\b\\f\","
              "\"nul\\u0000byte\","
              "\"\\u0001\\u001f\\u007f\","
              "\"\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\","
              "\"\\u2028\\u2029\"]");
}

TEST(LabJsonWriter, RefusesStringsThatAreNotValidUtf8) {
    JsonWriter w;
    w.begin_array();
    EXPECT_THROW(w.string("\xFF"), std::invalid_argument);
    EXPECT_THROW(w.string("\xC3"), std::invalid_argument);
}

TEST(LabJsonWriter, DoublesAreTheShortestTextThatReadsBackToTheSameBits) {
    const std::vector<double> values{
        0.0, -0.0, 1.0, -1.0, 0.1, 0.2, 0.1 + 0.2, 1.0 / 3.0, 100.0, 123456789.12345679, 1e-7, 1e21, 1e22,
        2.5e-310 /* subnormal */, std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::min(),
        std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(), 9007199254740993.0,
        0.30000000000000004, 5e-324};
    for (const double value : values) {
        JsonWriter w;
        w.number(value);
        const std::string& text = w.str();
        double back             = 0.0;
        const auto result       = std::from_chars(text.data(), text.data() + text.size(), back);
        ASSERT_EQ(result.ec, std::errc{}) << text;
        ASSERT_EQ(result.ptr, text.data() + text.size()) << text;
        EXPECT_EQ(std::signbit(back), std::signbit(value)) << text;
        EXPECT_EQ(back, value) << text << " must read back to the same double";
        // Valid JSON number syntax: digits, an optional fraction and exponent; never "inf", "nan" or a bare '.'.
        EXPECT_EQ(text.find_first_not_of("0123456789+-.e"), std::string::npos) << text;
        EXPECT_EQ(text.find(','), std::string::npos) << text;
    }
    JsonWriter w;
    w.number(0.1);
    EXPECT_EQ(w.str(), "0.1") << "shortest, not 0.10000000000000001";
}

TEST(LabJsonWriter, NonFiniteNumbersCannotBeWritten) {
    for (const double value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
        JsonWriter w;
        EXPECT_THROW(w.number(value), std::domain_error);
        EXPECT_FALSE(w.complete()) << "a refused number leaves no half-written value behind";
    }
}

TEST(LabJsonWriter, StructuralMisuseThrowsInsteadOfProducingBrokenJson) {
    {
        JsonWriter w;
        w.begin_object();
        EXPECT_THROW(w.string("value without a key"), std::logic_error);
    }
    {
        JsonWriter w;
        w.begin_array();
        EXPECT_THROW(w.key("k"), std::logic_error);
    }
    {
        JsonWriter w;
        w.begin_object();
        EXPECT_THROW(w.end_array(), std::logic_error);
    }
    {
        JsonWriter w;
        w.begin_object().key("dangling");
        EXPECT_THROW(w.end_object(), std::logic_error);
    }
    {
        JsonWriter w;
        w.integer(1);
        EXPECT_THROW(w.integer(2), std::logic_error) << "one root value only";
    }
    {
        JsonWriter w;
        w.begin_object();
        EXPECT_FALSE(w.complete());
        EXPECT_THROW((void)w.str(), std::logic_error);
        EXPECT_THROW((void)w.take(), std::logic_error);
    }
}

// A locale that would corrupt any number written through iostreams or printf-style
// formatting: a comma as the decimal point and a dot as the thousands separator.
struct CommaDecimal final : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};

class LocaleScope {
public:
    LocaleScope()
        : cpp_{std::locale::global(std::locale(std::locale::classic(), new CommaDecimal))} {
        // Also try the C locale of a real comma-decimal language, best effort.
        if (const char* current = std::setlocale(LC_ALL, nullptr)) {
            saved_c_ = current;
        }
        for (const char* name : {"de_DE.UTF-8", "German_Germany.1252", "fr_FR.UTF-8", "French_France.1252"}) {
            if (std::setlocale(LC_ALL, name) != nullptr) {
                c_locale_changed = true;
                break;
            }
        }
    }
    ~LocaleScope() {
        std::locale::global(cpp_);
        std::setlocale(LC_ALL, saved_c_.c_str());
    }
    bool c_locale_changed{false};

private:
    std::locale cpp_;
    std::string saved_c_{"C"};
};

TEST(LabJsonWriter, NumbersAreIdenticalUnderAHostileGlobalLocale) {
    const auto render = [] {
        JsonWriter w;
        w.begin_array();
        for (const double value : {1234567.5, 0.1, -2.5e-10, 1e22, 100.0}) {
            w.number(value);
        }
        w.unsigned_integer(18446744073709551615ULL).integer(-9223372036854775807LL - 1).integer(1234567);
        w.end_array();
        return w.take();
    };
    const std::string baseline = render();
    EXPECT_EQ(baseline,
              "[1234567.5,0.1,-2.5e-10,1e+22,100,18446744073709551615,-9223372036854775808,1234567]");

    std::string under_locale;
    {
        LocaleScope scope;
        // Sanity: the hostile locale really is in force for anything that consults it.
        EXPECT_EQ(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point(), ',');
        under_locale = render();
    }
    EXPECT_EQ(under_locale, baseline) << "number text must not depend on the locale";
}

// ---- errors and I/O -------------------------------------------------------------------------

TEST(LabErrors, EveryCodeMapsToTheDocumentedExitStatus) {
    EXPECT_EQ(exit_status(ErrorCode::UsageError), 2);
    EXPECT_EQ(exit_status(ErrorCode::InvalidParameter), 2);
    EXPECT_EQ(exit_status(ErrorCode::UnavailableStrategy), 2);
    EXPECT_EQ(exit_status(ErrorCode::ConfigError), 2);
    EXPECT_EQ(exit_status(ErrorCode::DatasetError), 3);
    EXPECT_EQ(exit_status(ErrorCode::LimitExceeded), 3);
    EXPECT_EQ(exit_status(ErrorCode::InternalError), 4);
    EXPECT_EQ(kExitOk, 0);
    EXPECT_EQ(code_name(ErrorCode::DatasetError), "dataset_error");
    EXPECT_EQ(code_name(ErrorCode::UnavailableStrategy), "unavailable_strategy");
}

TEST(LabErrors, TheProblemCountNeverFallsBelowTheListedProblems) {
    const LabError few{ErrorCode::DatasetError, "m", {Problem{1, "c", "", "x"}, Problem{2, "c", "", "y"}}, 0};
    EXPECT_EQ(few.total_problems(), 2u);
    const LabError capped{ErrorCode::DatasetError, "m", {Problem{1, "c", "", "x"}}, 90};
    EXPECT_EQ(capped.total_problems(), 90u);
}

// fopen is "unsafe" to MSVC's CRT and fopen_s does not exist elsewhere.
std::FILE* open_file(const std::string& path, const char* mode) {
#if defined(_MSC_VER)
    std::FILE* file = nullptr;
    return fopen_s(&file, path.c_str(), mode) == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

TEST(LabIo, WriteAllReportsFailureInsteadOfPretendingToSucceed) {
    const std::string path =
        (std::filesystem::temp_directory_path() / "strategy_lab_write_all_test.bin").string();

    std::FILE* sink = open_file(path, "wb");
    ASSERT_NE(sink, nullptr);
    EXPECT_TRUE(write_all(sink, "hello"));
    std::fclose(sink);

    std::FILE* check = open_file(path, "rb");
    ASSERT_NE(check, nullptr);
    char buffer[8] = {};
    EXPECT_EQ(std::fread(buffer, 1, 5, check), 5u);
    EXPECT_EQ(std::string(buffer, 5), "hello");
    std::fclose(check);

    // A stream opened read-only refuses writes: write_all must say so, not pretend.
    std::FILE* read_only = open_file(path, "rb");
    ASSERT_NE(read_only, nullptr);
    EXPECT_FALSE(write_all(read_only, "x"));
    std::fclose(read_only);
    std::filesystem::remove(path);
}
}  // namespace
