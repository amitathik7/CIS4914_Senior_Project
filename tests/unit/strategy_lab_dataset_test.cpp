// CSV VALIDATION: what the dataset loader accepts and refuses. These tests are about the
// input boundary only. How the strategies treat a malformed MarketEvent that reaches them is
// a different question, answered with in-memory events in strategy_lab_replay_test.cpp.
//
// The loader validates and never repairs: every refusal names a line and a column, and a
// bad price is never turned into a good one.

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "strategy_lab_test_support.hpp"
#include "lab/sha256.hpp"

namespace {

using namespace lab_test;
using lab::Dataset;
using lab::ErrorCode;
using lab::LabError;

const std::string kHeader = "symbol,exchange_time,type,price\n";
const std::string kT0     = "2026-01-05T14:30:00Z";
const std::string kT1     = "2026-01-05T14:31:00Z";
const std::string kT2     = "2026-01-05T14:32:00Z";

std::string bar(const std::string& symbol, const std::string& time, const std::string& price) {
    return symbol + "," + time + ",bar," + price + "\n";
}

Dataset ok(const std::string& text) { return lab::parse_dataset(text, "test.csv"); }

LabError refused(const std::string& text, const lab::DatasetLimits& limits = {}) {
    return expect_lab_error([&] { (void)lab::parse_dataset(text, "test.csv", limits); });
}

bool has_problem(const LabError& error, std::size_t line, const std::string& column, const std::string& needle) {
    return std::any_of(error.problems().begin(), error.problems().end(), [&](const lab::Problem& p) {
        return p.line == line && p.column == column && p.message.find(needle) != std::string::npos;
    });
}

// A file whose single data row is `row` (without its newline): the problem, if any.
LabError refused_row(const std::string& row) { return refused(kHeader + row + "\n"); }

// ---- accepted input -------------------------------------------------------------------------------

TEST(LabDataset, ParsesRowsIntoMarketEventsInFileOrderWithProvenance) {
    const std::string text = kHeader + bar("AAPL", kT0, "101.5") + bar("MSFT", kT0, "300") +
                             bar("AAPL", "2026-01-05T14:31:00.5Z", "102");
    const Dataset dataset = ok(text);

    ASSERT_EQ(dataset.rows.size(), 3u);
    EXPECT_EQ(dataset.rows[0].line, 2u);
    EXPECT_EQ(dataset.rows[2].line, 4u);

    const domain::MarketEvent& first = dataset.rows[0].event;
    EXPECT_EQ(first.symbol, "AAPL");
    EXPECT_EQ(first.type, domain::MarketEventType::Bar);
    EXPECT_EQ(first.price, std::optional<double>{101.5});
    EXPECT_EQ(first.exchange_time, *lab::parse_utc_timestamp(kT0));
    EXPECT_EQ(first.ingest_time, first.exchange_time) << "the lab replays: ingest time is the exchange time";
    EXPECT_FALSE(first.open.has_value());
    EXPECT_FALSE(first.volume.has_value());
    EXPECT_EQ(dataset.rows[2].event.exchange_time, *lab::parse_utc_timestamp("2026-01-05T14:31:00.5Z"));

    // sequence counts rows per symbol, in file order
    EXPECT_EQ(dataset.rows[0].event.sequence, 1u);
    EXPECT_EQ(dataset.rows[1].event.sequence, 1u);
    EXPECT_EQ(dataset.rows[2].event.sequence, 2u);

    EXPECT_EQ(dataset.info.name, "test.csv");
    EXPECT_EQ(dataset.info.bytes, text.size());
    EXPECT_EQ(dataset.info.rows, 3u);
    EXPECT_EQ(dataset.info.sha256, lab::sha256_hex(text));
    ASSERT_EQ(dataset.info.symbols.size(), 2u);
    EXPECT_EQ(dataset.info.symbols[0].symbol, "AAPL") << "sorted by symbol";
    EXPECT_EQ(dataset.info.symbols[0].bar_rows, 2u);
    EXPECT_EQ(dataset.info.symbols[1].symbol, "MSFT");
    EXPECT_EQ(dataset.info.first_exchange_time, *lab::parse_utc_timestamp(kT0));
    EXPECT_EQ(dataset.info.last_exchange_time, *lab::parse_utc_timestamp("2026-01-05T14:31:00.5Z"));
}

TEST(LabDataset, ReadsAFileUsingItsNameNotItsPathAndParsesOptionalColumns) {
    const Dataset dataset = lab::load_dataset(fixture_path("ohlcv_example.csv"));
    EXPECT_EQ(dataset.info.name, "ohlcv_example.csv") << "never a directory path";
    ASSERT_EQ(dataset.rows.size(), 3u);
    const domain::MarketEvent& event = dataset.rows[0].event;
    EXPECT_EQ(event.price, std::optional<double>{101.25});
    EXPECT_EQ(event.open, std::optional<double>{100.5});
    EXPECT_EQ(event.high, std::optional<double>{102.0});
    EXPECT_EQ(event.low, std::optional<double>{100.25});
    EXPECT_EQ(event.volume, std::optional<double>{15000.0});
}

TEST(LabDataset, AcceptsCrlfABomNoFinalNewlineAndAnyColumnOrder) {
    const std::string lf = kHeader + bar("AAPL", kT0, "1") + bar("AAPL", kT1, "2");
    std::string crlf;
    for (const char c : lf) {
        crlf += (c == '\n') ? std::string{"\r\n"} : std::string{c};
    }
    EXPECT_EQ(ok(crlf).rows.size(), 2u);
    EXPECT_EQ(ok("\xEF\xBB\xBF" + lf).rows.size(), 2u) << "UTF-8 byte-order mark";
    EXPECT_EQ(ok(lf.substr(0, lf.size() - 1)).rows.size(), 2u) << "no newline after the last row";

    const Dataset permuted = ok("price,type,symbol,exchange_time\n1.5,bar,AAPL," + kT0 + "\n");
    ASSERT_EQ(permuted.rows.size(), 1u);
    EXPECT_EQ(permuted.rows[0].event.symbol, "AAPL");
    EXPECT_EQ(permuted.rows[0].event.price, std::optional<double>{1.5});

    // Empty optional cells mean absent.
    const Dataset sparse = ok("symbol,exchange_time,type,price,open,high,low,volume\nAAPL," + kT0 + ",bar,5,,,,\n");
    EXPECT_FALSE(sparse.rows[0].event.open.has_value());
    EXPECT_FALSE(sparse.rows[0].event.volume.has_value());
}

TEST(LabDataset, SymbolsKeepTheirCaseAndMayContainPunctuation) {
    const Dataset dataset = ok(kHeader + bar("aapl", kT0, "1") + bar("BRK.B", kT0, "2") + bar("A\\B", kT0, "3"));
    EXPECT_EQ(dataset.rows[0].event.symbol, "aapl") << "case is preserved: 'aapl' is not 'AAPL'";
    EXPECT_EQ(dataset.rows[1].event.symbol, "BRK.B");
    EXPECT_EQ(dataset.rows[2].event.symbol, "A\\B");
}

TEST(LabDataset, AcceptsPricesAtTheEdgesOfTheDoubleRange) {
    const Dataset dataset = ok(kHeader + bar("AAPL", kT0, "5e-324") + bar("AAPL", kT1, "1.7976931348623157e308") +
                               bar("AAPL", kT2, "0.30000000000000004"));
    EXPECT_EQ(dataset.rows[0].event.price, std::optional<double>{5e-324});
    EXPECT_EQ(dataset.rows[1].event.price, std::optional<double>{1.7976931348623157e308});
    EXPECT_EQ(dataset.rows[2].event.price, std::optional<double>{0.30000000000000004});
}

TEST(LabDataset, TradeRowsAreAcceptedSoTheyCanBeShownBeingIgnored) {
    const Dataset dataset = ok(kHeader + bar("AAPL", kT0, "10") + "AAPL," + kT0 + ",trade,10.01\n");
    ASSERT_EQ(dataset.rows.size(), 2u);
    EXPECT_EQ(dataset.rows[1].event.type, domain::MarketEventType::Trade);
    EXPECT_EQ(dataset.info.symbols[0].trade_rows, 1u);
    EXPECT_EQ(dataset.info.symbols[0].bar_rows, 1u);
}

// ---- the header -----------------------------------------------------------------------------------

TEST(LabDatasetHeader, RequiredColumnsMustAllBePresent) {
    for (const char* name : {"symbol", "exchange_time", "type", "price"}) {
        std::string header;
        for (const char* column : {"symbol", "exchange_time", "type", "price"}) {
            if (std::string{column} != name) {
                header += (header.empty() ? "" : ",") + std::string{column};
            }
        }
        const LabError error = refused(header + "\n");
        EXPECT_EQ(error.code(), ErrorCode::DatasetError);
        EXPECT_TRUE(has_problem(error, 1, name, "required column is missing")) << name;
    }
}

TEST(LabDatasetHeader, UnknownAndRepeatedColumnsAreRefused) {
    EXPECT_TRUE(has_problem(refused("symbol,exchange_time,type,price,colour\n"), 1, "colour", "unknown column"));
    EXPECT_TRUE(has_problem(refused("symbol,exchange_time,type,price,symbol\n"), 1, "symbol", "more than once"));
    EXPECT_TRUE(has_problem(refused("Symbol,exchange_time,type,price\n"), 1, "Symbol", "unknown column"))
        << "column names are case-sensitive";
}

TEST(LabDatasetHeader, AnEmptyFileOrAHeaderWithNoRowsIsAnError) {
    EXPECT_TRUE(has_problem(refused(""), 0, "", "empty"));
    EXPECT_TRUE(has_problem(refused(kHeader), 1, "", "no data rows"));
    EXPECT_EQ(refused("\n").code(), ErrorCode::DatasetError) << "a blank first line is not a header";
}

// ---- row structure ------------------------------------------------------------------------------

TEST(LabDatasetRows, FieldCountBlankLinesAndQuotesAreRefusedWithTheirLine) {
    EXPECT_TRUE(has_problem(refused(kHeader + "AAPL," + kT0 + ",bar\n"), 2, "", "has 3 fields; the header has 4"));
    EXPECT_TRUE(has_problem(refused(kHeader + "AAPL," + kT0 + ",bar,1,extra\n"), 2, "", "has 5 fields"));
    EXPECT_TRUE(has_problem(refused(kHeader + bar("AAPL", kT0, "1") + "\n" + bar("AAPL", kT1, "2")), 3, "", "blank line"));
    EXPECT_TRUE(has_problem(refused(kHeader + bar("AAPL", kT0, "1") + "\n"), 3, "", "blank line"))
        << "a second newline at the end is a blank line; one final newline is not";
    EXPECT_TRUE(has_problem(refused(kHeader + "\"AAPL\"," + kT0 + ",bar,1\n"), 2, "", "quote"));
    EXPECT_TRUE(has_problem(refused(kHeader + "AAPL," + kT0 + ",bar,\"1,5\"\n"), 2, "", "quote"));
}

TEST(LabDatasetRows, SymbolsMustBePrintableAsciiOfBoundedLength) {
    for (const std::string symbol : {std::string{}, std::string{"A B"}, std::string(33, 'X'), std::string{"\t"},
                                     std::string{"caf\xC3\xA9"}}) {
        EXPECT_TRUE(has_problem(refused_row(symbol + "," + kT0 + ",bar,1"), 2, "symbol", "symbol")) << symbol;
    }
    EXPECT_NO_THROW((void)ok(kHeader + bar(std::string(32, 'X'), kT0, "1")));
}

TEST(LabDatasetRows, TimestampsMustBeRfc3339UtcWithAZ) {
    for (const char* time : {"2026-01-05T14:30:00", "2026-01-05T14:30:00+00:00", "2026-01-05 14:30:00Z",
                             "2026-02-30T14:30:00Z", "2026-01-05T25:30:00Z", "05/01/2026", ""}) {
        const LabError error = refused_row(std::string{"AAPL,"} + time + ",bar,1");
        EXPECT_TRUE(has_problem(error, 2, "exchange_time", "exchange_time")) << time;
    }
}

TEST(LabDatasetRows, OnlyBarAndTradeTypesAreSupported) {
    for (const char* type : {"BAR", "quote", "status", "unknown", "", "bars"}) {
        const LabError error = refused_row("AAPL," + kT0 + "," + type + ",1");
        EXPECT_TRUE(has_problem(error, 2, "type", "not supported")) << "'" << type << "'";
    }
}

// ---- prices: never repaired ---------------------------------------------------------------------

TEST(LabDatasetPrices, EveryNonFinitePositivePriceIsRefusedNotCorrected) {
    struct Case { const char* text; const char* reason; };
    const std::vector<Case> cases{
        {"", "required"},          {"0", "greater than 0"},   {"-1", "greater than 0"},
        {"-0", "greater than 0"},  {"nan", "finite"},         {"NaN", "finite"},
        {"inf", "finite"},         {"-inf", "finite"},        {"infinity", "finite"},
        {"1e999", "range"},        {"-1e999", "range"},       {"abc", "not a number"},
        {"1.5x", "not a number"},  {" 1", "not a number"},    {"1 ", "not a number"},
        {"+1", "not a number"},    {"0x10", "not a number"},  {"1e", "not a number"},
        {"1e-400", "range"},       {".", "not a number"},
    };
    for (const Case& each : cases) {
        const LabError error = refused_row("AAPL," + kT0 + ",bar," + each.text);
        EXPECT_EQ(error.problems().size(), 1u) << each.text;
        EXPECT_TRUE(has_problem(error, 2, "price", each.reason)) << "'" << each.text << "' -> " <<
            (error.problems().empty() ? "no problem" : error.problems()[0].message);
    }
}

TEST(LabDatasetPrices, TheSameRulesApplyToTradeRows) {
    EXPECT_TRUE(has_problem(refused_row("AAPL," + kT0 + ",trade,0"), 2, "price", "greater than 0"));
    EXPECT_TRUE(has_problem(refused_row("AAPL," + kT0 + ",trade,nan"), 2, "price", "finite"));
}

TEST(LabDatasetPrices, OpenHighLowAndVolumeAreCheckedAgainstTheClose) {
    const std::string h = "symbol,exchange_time,type,price,open,high,low,volume\n";
    const auto row = [&](const std::string& tail) { return refused(h + "AAPL," + kT0 + ",bar," + tail + "\n"); };

    EXPECT_TRUE(has_problem(row("100,99,101,100.5,10"), 2, "low", "above the close"));
    EXPECT_TRUE(has_problem(row("100,99,99.5,98,10"), 2, "high", "below the close"));
    EXPECT_TRUE(has_problem(row("100,99,101,99.5,10"), 2, "low", "above the open"));
    EXPECT_TRUE(has_problem(row("100,102,101,99,10"), 2, "high", "below the open"));
    EXPECT_TRUE(has_problem(row("100,0,101,99,10"), 2, "open", "greater than 0"));
    EXPECT_TRUE(has_problem(row("100,,nan,99,10"), 2, "high", "finite"));
    EXPECT_TRUE(has_problem(row("100,,,,-1"), 2, "volume", "at least 0"));
    EXPECT_TRUE(has_problem(row("100,,,,nan"), 2, "volume", "finite"));
    EXPECT_NO_THROW((void)ok(h + "AAPL," + kT0 + ",bar,100,100,100,100,0\n")) << "a flat bar with zero volume is valid";

    const LabError trade = refused(h + "AAPL," + kT0 + ",trade,100,99,,,\n");
    EXPECT_TRUE(has_problem(trade, 2, "open", "only allowed on bar rows"));
}

// ---- ordering: per symbol, across the file, and ties ---------------------------------------------

TEST(LabDatasetOrdering, ADuplicateBarForOneSymbolIsAnErrorNeverDropped) {
    const LabError error = refused(kHeader + bar("AAPL", kT0, "1") + bar("AAPL", kT0, "2"));
    EXPECT_TRUE(has_problem(error, 3, "exchange_time", "not later than the previous AAPL bar at line 2"));
}

TEST(LabDatasetOrdering, AnOlderBarForOneSymbolIsAnError) {
    const LabError error = refused(kHeader + bar("AAPL", kT1, "1") + bar("AAPL", kT0, "2"));
    EXPECT_TRUE(has_problem(error, 3, "exchange_time", "not later than the previous AAPL bar at line 2"));
}

TEST(LabDatasetOrdering, TimeMayNotGoBackwardsAcrossTheFileEvenBetweenSymbols) {
    const LabError error = refused(kHeader + bar("AAPL", kT1, "1") + bar("MSFT", kT0, "2"));
    EXPECT_TRUE(has_problem(error, 3, "exchange_time", "earlier than line 2"));
    EXPECT_TRUE(has_problem(error, 3, "exchange_time", "never sorts")) << "the message says nothing is sorted or repaired";
}

TEST(LabDatasetOrdering, EqualTimestampsAcrossSymbolsAreValidAndKeepFileOrder) {
    // The lab-only tie policy: rows of different symbols with one exchange_time replay in the
    // order the file lists them. Both orders are valid and are NOT normalized.
    const Dataset aapl_first = ok(kHeader + bar("AAPL", kT0, "1") + bar("MSFT", kT0, "2"));
    const Dataset msft_first = ok(kHeader + bar("MSFT", kT0, "2") + bar("AAPL", kT0, "1"));
    ASSERT_EQ(aapl_first.rows.size(), 2u);
    EXPECT_EQ(aapl_first.rows[0].event.symbol, "AAPL");
    EXPECT_EQ(aapl_first.rows[1].event.symbol, "MSFT");
    EXPECT_EQ(msft_first.rows[0].event.symbol, "MSFT") << "not alphabetical, not sorted: file order";
    EXPECT_EQ(msft_first.rows[1].event.symbol, "AAPL");
    EXPECT_EQ(msft_first.rows[0].event.exchange_time, msft_first.rows[1].event.exchange_time);
}

TEST(LabDatasetOrdering, ATradeMayShareATimestampWithABarOfTheSameSymbolButNotPrecedeItsPreviousRow) {
    EXPECT_NO_THROW((void)ok(kHeader + bar("AAPL", kT0, "1") + "AAPL," + kT0 + ",trade,1.1\n"));
    EXPECT_NO_THROW((void)ok(kHeader + "AAPL," + kT0 + ",trade,1.1\n" + bar("AAPL", kT0, "1")))
        << "a bar may share the time of an earlier trade";
    const LabError error = refused(kHeader + bar("AAPL", kT1, "1") + "AAPL," + kT0 + ",trade,1.1\n");
    EXPECT_TRUE(has_problem(error, 3, "exchange_time", "earlier than the previous AAPL row at line 2"));
}

TEST(LabDatasetOrdering, EachSymbolIsCheckedIndependently) {
    // AAPL's bars strictly increase and so do MSFT's, though they interleave in time.
    EXPECT_NO_THROW((void)ok(kHeader + bar("AAPL", kT0, "1") + bar("MSFT", kT1, "2") + bar("AAPL", kT2, "3") +
                             bar("MSFT", kT2, "4")));
    // ... and a duplicate for MSFT is caught even though AAPL's rows lie between.
    const LabError error = refused(kHeader + bar("MSFT", kT1, "2") + bar("AAPL", kT1, "1") + bar("MSFT", kT1, "3"));
    EXPECT_TRUE(has_problem(error, 4, "exchange_time", "previous MSFT bar at line 2"));
}

// ---- reporting several problems, limits, encodings -----------------------------------------------

TEST(LabDatasetErrors, SeveralRowProblemsAreCollectedWithTheirLinesAndInvalidRowsAreNotKept) {
    const std::string text = kHeader + bar("AAPL", kT0, "1") + bar("AAPL", kT1, "nan") + bar("", kT2, "3") +
                             bar("AAPL", "bad", "4") + bar("AAPL", kT2, "5");
    const LabError error = refused(text);
    EXPECT_EQ(error.code(), ErrorCode::DatasetError);
    EXPECT_EQ(error.total_problems(), 3u);
    EXPECT_TRUE(has_problem(error, 3, "price", "finite"));
    EXPECT_TRUE(has_problem(error, 4, "symbol", "symbol"));
    EXPECT_TRUE(has_problem(error, 5, "exchange_time", "exchange_time"));
    EXPECT_NE(std::string{error.what()}.find("3 problems"), std::string::npos) << error.what();
}

TEST(LabDatasetErrors, TheProblemListIsCappedButTheTotalIsCounted) {
    std::string text = kHeader;
    for (int i = 0; i < 120; ++i) {
        text += bar("AAPL", kT0, "nan");
    }
    const LabError error = refused(text);
    EXPECT_EQ(error.problems().size(), 50u);
    EXPECT_EQ(error.total_problems(), 120u);
}

TEST(LabDatasetLimits, TooManyRowsIsALimitErrorNotATruncation) {
    lab::DatasetLimits limits;
    limits.max_rows = 2;
    EXPECT_NO_THROW((void)lab::parse_dataset(kHeader + bar("A", kT0, "1") + bar("A", kT1, "2"), "t.csv", limits));
    const LabError error = refused(kHeader + bar("A", kT0, "1") + bar("A", kT1, "2") + bar("A", kT2, "3"), limits);
    EXPECT_EQ(error.code(), ErrorCode::LimitExceeded);
    EXPECT_EQ(lab::exit_status(error.code()), 3);
    EXPECT_NE(std::string{error.what()}.find("never truncates"), std::string::npos);
}

TEST(LabDatasetLimits, AnOverlongLineIsRefused) {
    lab::DatasetLimits limits;
    limits.max_line_bytes = 40;
    EXPECT_TRUE(has_problem(refused(kHeader + bar(std::string(32, 'X'), kT0, "1"), limits), 2, "", "longer than 40 bytes"));
}

TEST(LabDatasetEncoding, InvalidUtf8AndControlCharactersAreRefusedPerLine) {
    EXPECT_TRUE(has_problem(refused(kHeader + "AAPL," + kT0 + ",bar,1\xFF\n"), 2, "", "not valid UTF-8"));
    EXPECT_TRUE(has_problem(refused(kHeader + std::string("AAP\0L,", 6) + kT0 + ",bar,1\n"), 2, "symbol", "symbol"));
    EXPECT_EQ(refused(std::string("symbol,exchange_time,type,pri\xFF" "ce\n")).code(), ErrorCode::DatasetError);
}

TEST(LabDatasetFiles, AMissingFileOrADirectoryIsADatasetError) {
    const LabError missing = expect_lab_error([] { (void)lab::load_dataset(fixture_path("no_such_file.csv")); });
    EXPECT_EQ(missing.code(), ErrorCode::DatasetError);
    EXPECT_EQ(lab::exit_status(missing.code()), 3);

    const LabError directory = expect_lab_error([] { (void)lab::load_dataset(fixture_path("")); });
    EXPECT_EQ(directory.code(), ErrorCode::DatasetError);
}

TEST(LabDatasetFiles, AFileOverTheSizeLimitIsALimitError) {
    lab::DatasetLimits limits;
    limits.max_file_bytes = 10;
    const LabError error =
        expect_lab_error([&] { (void)lab::load_dataset(fixture_path("sma_crossover.csv"), limits); });
    EXPECT_EQ(error.code(), ErrorCode::LimitExceeded);
}

TEST(LabDatasetFiles, EveryCheckedInFixtureLoadsCleanly) {
    for (const char* name : {"sma_crossover.csv", "sma_oscillation.csv", "mr_rearm.csv", "mr_suppression.csv",
                             "constant_price.csv", "warmup_boundary.csv", "two_symbols_interleaved.csv",
                             "tie_aapl_first.csv", "tie_msft_first.csv", "bars_and_trades.csv", "ohlcv_example.csv",
                             "precision_edge.csv", "escape_symbol.csv"}) {
        EXPECT_NO_THROW((void)lab::load_dataset(fixture_path(name))) << name;
    }
}

}  // namespace
