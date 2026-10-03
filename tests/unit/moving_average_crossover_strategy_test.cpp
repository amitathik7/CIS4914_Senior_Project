// MovingAverageCrossoverStrategy driven directly, without an engine: its
// configuration, warm-up and baseline, crossovers, equal averages, window
// eviction, symbols and the signals it emits.
//
// Every expected value was worked out by hand (the arithmetic is in the
// comments) and cross-checked with an independent exact-fraction script; none
// is read back from the implementation. What it ignores, how it restarts and a
// long run against an exact reference are in
// moving_average_crossover_robustness_test.cpp; the real StrategyEngine in
// moving_average_crossover_engine_test.cpp.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "moving_average_crossover_fixture.hpp"
#include "trading_engine/common/errors.hpp"

namespace {

using namespace sma_test;
using domain::SignalSide;
using strategy::MovingAverageCrossoverConfig;
using strategy::MovingAverageCrossoverStrategy;

struct Case {
    const char* name;
    std::size_t short_window;
    std::size_t long_window;
    Closes      closes;
    Labels      expected;
};

void expect_labels(const std::vector<Case>& cases) {
    for (const Case& scenario : cases) {
        SCOPED_TRACE(scenario.name);
        Harness h{make_config(scenario.short_window, scenario.long_window)};
        h.feed_closes(scenario.closes);
        EXPECT_EQ(h.labels(), scenario.expected);
    }
}

// What constructing the strategy from `cfg` throws: its message if a
// common::ConfigError, "<accepted>" if nothing, "<other>" for anything else.
std::string config_error(MovingAverageCrossoverConfig cfg) {
    try {
        const MovingAverageCrossoverStrategy accepted{std::move(cfg)};
        (void)accepted;
    } catch (const common::ConfigError& e) {
        return e.what();
    } catch (...) {
        return "<other>";
    }
    return "<accepted>";
}

bool mentions(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

// --- Configuration -------------------------------------------------------------

TEST(SmaCrossoverConfig, DefaultsAreFiveTwentyOneShareAndTheSmaCrossoverId) {
    const MovingAverageCrossoverConfig defaults;
    EXPECT_EQ(defaults.strategy_id, "sma_crossover");
    EXPECT_EQ(defaults.short_window, 5u);
    EXPECT_EQ(defaults.long_window, 20u);
    EXPECT_EQ(defaults.requested_quantity, 1);
    EXPECT_TRUE(defaults.symbols.empty()) << "the allowlist has no default";

    // Behaviourally, with 5/20 windows: 20 closes falling 120, 119, ..., 101 and
    // then a 200. At bar 20 short = (105+104+103+102+101)/5 = 103 is below long =
    // (101+...+120)/20 = 110.5: the silent baseline. Bar 21 (200) leaves short =
    // (104+103+102+101+200)/5 = 122 and long = (101+...+119+200)/20 = 2290/20 =
    // 114.5: short is above, so Buy.
    MovingAverageCrossoverConfig only_symbols;
    only_symbols.symbols = {"AAPL"};
    Harness h{only_symbols};
    EXPECT_EQ(h.sma.id(), "sma_crossover");
    Closes closes;
    for (int i = 0; i < 20; ++i) {
        closes.push_back(common::Price::from_units(120 - i));
    }
    closes.push_back(common::Price::from_units(200));
    h.feed_closes(closes);
    EXPECT_EQ(h.labels(), (Labels{"buy@21"}));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_EQ(h.signals[0].requested_quantity, std::optional<common::Quantity>{1});
    EXPECT_EQ(h.signals[0].metadata.at("short_window"), "5");
    EXPECT_EQ(h.signals[0].metadata.at("long_window"), "20");
    EXPECT_EQ(h.signals[0].metadata.at("short_sma"), "122");
    EXPECT_EQ(h.signals[0].metadata.at("long_sma"), "114.5");
}

TEST(SmaCrossoverConfig, AcceptsValidConfigurations) {
    EXPECT_EQ(config_error(make_config(1, 2)), "<accepted>");   // the tightest pair
    EXPECT_EQ(config_error(make_config(50, 200, {"AAPL", "MSFT", "BRK.B"})), "<accepted>");
    for (const common::Quantity whole : {common::Quantity{1}, common::Quantity{2}, common::Quantity{100},
                                         common::Quantity{1'000'000},
                                         std::numeric_limits<common::Quantity>::max()}) {
        MovingAverageCrossoverConfig cfg = make_config(2, 3);
        cfg.requested_quantity           = whole;
        EXPECT_EQ(config_error(cfg), "<accepted>") << whole;
    }
}

TEST(SmaCrossoverConfig, RejectsWindowsThatAreNotZeroLessThanShortLessThanLong) {
    EXPECT_THROW(MovingAverageCrossoverStrategy{make_config(0, 3)}, common::ConfigError);

    struct Bad { std::size_t short_window; std::size_t long_window; const char* field; };
    for (const Bad& bad : {Bad{0, 3, "short_window"}, Bad{0, 0, "short_window"},
                           Bad{3, 3, "long_window"}, Bad{4, 3, "long_window"},
                           Bad{1, 1, "long_window"}}) {
        const std::string message = config_error(make_config(bad.short_window, bad.long_window));
        EXPECT_TRUE(mentions(message, bad.field))
            << bad.short_window << "/" << bad.long_window << ": " << message;
    }
}

TEST(SmaCrossoverConfig, RejectsAnEmptyStrategyId) {
    MovingAverageCrossoverConfig cfg = make_config(2, 3);
    cfg.strategy_id.clear();
    EXPECT_TRUE(mentions(config_error(cfg), "strategy_id"));
}

TEST(SmaCrossoverConfig, RejectsAMissingEmptyOrRepeatedSymbol) {
    EXPECT_TRUE(mentions(config_error(make_config(2, 3, {})), "symbols"));
    EXPECT_TRUE(mentions(config_error(make_config(2, 3, {"AAPL", ""})), "empty entry"));
    EXPECT_TRUE(mentions(config_error(make_config(2, 3, {""})), "empty entry"));
    const std::string repeated = config_error(make_config(2, 3, {"AAPL", "MSFT", "AAPL"}));
    EXPECT_TRUE(mentions(repeated, "AAPL")) << repeated;
    EXPECT_TRUE(mentions(repeated, "more than once")) << repeated;
}

TEST(SmaCrossoverConfig, RejectsQuantitiesThatAreNotPositive) {
    // Quantity is an exact integer count of shares, so a fraction cannot even be written; only
    // the sign needs checking. The message shows the offending value.
    for (const common::Quantity bad : {common::Quantity{0}, common::Quantity{-1}, common::Quantity{-1'000},
                                       std::numeric_limits<common::Quantity>::min()}) {
        MovingAverageCrossoverConfig cfg = make_config(2, 3);
        cfg.requested_quantity           = bad;
        const std::string message        = config_error(cfg);
        EXPECT_TRUE(mentions(message, "requested_quantity")) << bad << ": " << message;
        EXPECT_TRUE(mentions(message, std::to_string(bad))) << message;
    }
}

// --- Warm-up and the baseline ----------------------------------------------------

TEST(SmaCrossoverWarmUp, EmitsNothingUntilTheLongWindowIsFull) {
    Closes nineteen_falling;
    for (int i = 0; i < 19; ++i) {
        nineteen_falling.push_back(common::Price::from_units(120 - i));
    }
    expect_labels({
        {"two bars of a 2/3 pair", 2, 3, units({3, 2}), {}},
        {"a violent move inside the warm-up", 2, 3, units({1, 100}), {}},
        {"nineteen bars of a 5/20 pair", 5, 20, nineteen_falling, {}},
    });
}

TEST(SmaCrossoverWarmUp, TheFirstWarmedUpComparisonIsABaselineNotASignal) {
    expect_labels({
        // Bar 3: short (2+3)/2 = 2.5 above long (1+2+3)/3 = 2. Recorded, silent.
        {"baseline above", 2, 3, units({1, 2, 3}), {}},
        {"baseline above, then more rise", 2, 3, units({1, 2, 3, 4, 5}), {}},
        // Bar 3: short (2+1)/2 = 1.5 below long (3+2+1)/3 = 2.
        {"baseline below", 2, 3, units({3, 2, 1}), {}},
    });
}

// --- Crossovers -----------------------------------------------------------------

TEST(SmaCrossoverCrossovers, AnUpwardCrossingBuysAndADownwardOneSells) {
    expect_labels({
        // Bar 3 below (1.5 vs 2: baseline), bar 4 below (1.5 vs 1.667), bar 5 above (2.5 vs 2).
        {"up", 2, 3, units({3, 2, 1, 2, 3}), {"buy@5"}},
        // Bar 3 above (2.5 vs 2: baseline), bar 4 above (2.5 vs 2.333), bar 5 below (1.5 vs 2).
        {"down", 2, 3, units({1, 2, 3, 2, 1}), {"sell@5"}},
    });
}

TEST(SmaCrossoverCrossovers, NeverRepeatsASignalWhileTheRelationshipHolds) {
    expect_labels({
        {"keeps rising after the buy", 2, 3, units({3, 2, 1, 2, 3, 4, 5, 6, 7}), {"buy@5"}},
        {"keeps falling after the sell", 2, 3, {1_px, 2_px, 3_px, 2_px, 1_px, 0.5_px, 0.25_px, 0.125_px},
         {"sell@5"}},
    });
}

TEST(SmaCrossoverCrossovers, TheSpecFixtureGivesABuyAtBarFiveAndASellAtBarEight) {
    // bar close  short  long    short-long
    //   1   3      .      .     warming up
    //   2   2      .      .     warming up
    //   3   1     1.5    2.0    -0.5    below: the baseline, silent
    //   4   2     1.5    1.667  -0.167  below
    //   5   3     2.5    2.0    +0.5    above   BUY
    //   6   4     3.5    3.0    +0.5    above
    //   7   3     3.5    3.333  +0.167  above
    //   8   2     2.5    3.0    -0.5    below   SELL
    //   9   1     1.5    2.0    -0.5    below
    Harness h{make_config(2, 3)};
    std::vector<std::size_t> emitted_after_each_bar;
    long long minute = 0;
    for (const common::Price close : kFixture) {
        h.feed(bar("AAPL", close, ++minute));
        emitted_after_each_bar.push_back(h.signals.size());
    }
    EXPECT_EQ(emitted_after_each_bar, (std::vector<std::size_t>{0, 0, 0, 0, 1, 1, 1, 2, 2}));
    ASSERT_EQ(h.signals.size(), 2u);

    EXPECT_EQ(h.signals[0].side, SignalSide::Buy);
    EXPECT_EQ(h.signals[0].metadata.at("short_sma"), "2.5");
    EXPECT_EQ(h.signals[0].metadata.at("long_sma"), "2");
    EXPECT_EQ(h.signals[1].side, SignalSide::Sell);
    EXPECT_EQ(h.signals[1].metadata.at("short_sma"), "2.5");
    EXPECT_EQ(h.signals[1].metadata.at("long_sma"), "3");
}

// --- Equal averages ----------------------------------------------------------------

TEST(SmaCrossoverEquality, EqualAveragesKeepTheLastNonzeroRelationship) {
    // With a 1/2 pair the relationship is the sign of the last price change:
    // short = close, long = (previous + close) / 2.
    expect_labels({
        {"below, equal, below", 1, 2, units({5, 4, 4, 4, 3}), {}},
        {"above, equal, above", 1, 2, units({4, 5, 5, 5, 6}), {}},
        {"below, equal, above is a crossing", 1, 2, units({5, 4, 4, 6}), {"buy@4"}},
        {"above, equal, below is a crossing", 1, 2, units({4, 5, 5, 3}), {"sell@4"}},
    });
}

TEST(SmaCrossoverEquality, InitiallyEqualAveragesNeverSignalByThemselves) {
    expect_labels({
        // Equal at bars 2 and 3; bar 4 (6 vs 5) is the first nonzero: baseline, silent;
        // bar 5 (4 vs 5) is below: Sell.
        {"equal, then above, then below", 1, 2, units({5, 5, 5, 6, 4}), {"sell@5"}},
        // Equal at bar 2; bar 3 (4 vs 5) is the baseline; bar 4 (6 vs 4) is above: Buy.
        {"equal, then below, then above", 1, 2, units({5, 5, 4, 6}), {"buy@4"}},
        // 2/3 pair: bars 3 and 4 equal (all 5s), bar 5 above (5.5 vs 5.333) is the
        // baseline, bars 6 and 7 above, bar 8 below (5.5 vs 6): the only signal.
        {"flat start on a 2/3 pair", 2, 3, units({5, 5, 5, 5, 6, 7, 6, 5, 4}), {"sell@8"}},
    });
}

TEST(SmaCrossoverEquality, AveragesThatAreEqualInDecimalAreExactlyEqual) {
    // 2/3 pair. At bar 3 the oldest close is the mean of the other two, so short and
    // long are equal in decimal arithmetic:
    //   (0.3, 0.4, 0.2): short (0.4+0.2)/2 = 0.3 = long (0.3+0.4+0.2)/3
    //   (0.7, 0.5, 0.9): short (0.5+0.9)/2 = 0.7 = long (0.7+0.5+0.9)/3
    // With doubles these are not bit-equal (one rounding step above, one below), which is
    // why a floating-point version needs an equality tolerance. Prices are integer micros
    // here and the averages are compared by cross-multiplication, so the tie is exact and
    // no tolerance exists: bar 3 sets no baseline, bar 4 becomes it, and nothing fires.
    expect_labels({
        {"a tie that doubles read one step above", 2, 3, {0.3_px, 0.4_px, 0.2_px, 0.1_px}, {}},
        {"a tie that doubles read one step below", 2, 3, {0.7_px, 0.5_px, 0.9_px, 0.5_px}, {}},
    });
}

TEST(SmaCrossoverEquality, AOneMicroMoveIsAnExactCrossingNotMaskedByAnyTolerance) {
    // A 1/2 pair on 100, 99.999999, 100: one micro (1e-6) each way, a gap of 5e-9 of the
    // price. The comparison is exact, so even that is a real crossing (a floating-point
    // version would need a tolerance far below this to see it).
    expect_labels({{"a one-micro move", 1, 2, {100_px, 99.999999_px, 100_px}, {"buy@3"}}});
}

// --- Window eviction ------------------------------------------------------------

TEST(SmaCrossoverWindows, ClosesOlderThanTheLongWindowStopCounting) {
    // 2/3 pair on 100, 1, 1, 1, 2, 3. Bar 3: long (100+1+1)/3 = 34 is far above
    // short 1: baseline below. Bar 4: the 100 has left, long (1+1+1)/3 = 1 = short:
    // equal. Bar 5: short (1+2)/2 = 1.5 above long (1+1+2)/3: Buy. A 100 that
    // stayed in the window would keep long above short and hide the Buy.
    Harness h{make_config(2, 3)};
    h.feed_closes(units({100, 1, 1, 1, 2, 3}));
    EXPECT_EQ(h.labels(), (Labels{"buy@5"}));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_EQ(number(h.signals[0], "short_sma"), 1.5);
    EXPECT_EQ(number(h.signals[0], "long_sma"), 4.0 / 3.0)
        << "the text reads back as exactly the double";
}

TEST(SmaCrossoverWindows, TheRingStaysCorrectAcrossManyWraps) {
    // 3/5 pair: five 10s (all equal), then five 5s, then a 20. Bar 6 (short 8.33,
    // long 9) is the baseline, below; bars 7-9 stay below; bar 10 is five 5s: equal;
    // bar 11 has short (5+5+20)/3 = 10 above long (5+5+5+5+20)/5 = 8: Buy. The
    // history has wrapped twice by then, so every eviction index was used.
    expect_labels({{"wraps twice", 3, 5, units({10, 10, 10, 10, 10, 5, 5, 5, 5, 5, 20}), {"buy@11"}}});
}

// --- Symbols -----------------------------------------------------------------------

TEST(SmaCrossoverSymbols, KeepIndependentStateWhetherOrNotTheirBarsInterleave) {
    // MSFT closes at 10 - AAPL's close. An average is linear, so every
    // relationship flips: where AAPL buys, MSFT sells.
    //   AAPL: Buy@5 (2.5 vs 2), Sell@8 (2.5 vs 3)
    //   MSFT: Sell@5 (7.5 vs 8), Buy@8 (7.5 vs 7)
    Closes msft;
    for (const common::Price close : kFixture) {
        msft.push_back(common::Price::from_units(10) - close);
    }

    for (const bool alternate : {true, false}) {
        SCOPED_TRACE(alternate ? "alternating bars" : "all of AAPL, then all of MSFT");
        Harness h{make_config(2, 3, {"AAPL", "MSFT"})};
        if (alternate) {
            for (std::size_t i = 0; i < kFixture.size(); ++i) {
                const auto minute = static_cast<long long>(i) + 1;
                h.feed(bar("AAPL", kFixture[i], minute));
                h.feed(bar("MSFT", msft[i], minute));   // the same time: not a duplicate
            }
        } else {
            h.feed_closes(kFixture, "AAPL");
            h.feed_closes(msft, "MSFT");   // older than AAPL's last bar: fine, per symbol
        }
        EXPECT_EQ(h.labels("AAPL"), (Labels{"buy@5", "sell@8"}));
        EXPECT_EQ(h.labels("MSFT"), (Labels{"sell@5", "buy@8"}));

        ASSERT_EQ(h.signals.size(), 4u);
        for (const domain::TradeSignal& signal : h.signals) {
            if (signal.symbol != "MSFT") {
                continue;
            }
            const bool sell = signal.side == SignalSide::Sell;
            EXPECT_EQ(number(signal, "short_sma"), 7.5);
            EXPECT_EQ(number(signal, "long_sma"), sell ? 8.0 : 7.0);
        }
    }
}

TEST(SmaCrossoverSymbols, IgnoresSymbolsOutsideTheAllowlistAndMatchesExactly) {
    Harness h{make_config(2, 3)};
    h.feed_closes(kFixture, "TSLA");
    h.feed_closes(kFixture, "aapl");   // matching is case-sensitive
    EXPECT_TRUE(h.signals.empty());

    h.feed_closes(kFixture, "AAPL");   // and left no trace: the same minutes are still fresh
    EXPECT_EQ(h.labels(), (Labels{"buy@5", "sell@8"}));
}

// --- The signals it emits -----------------------------------------------------------

TEST(SmaCrossoverSignals, CarryTheContractedFields) {
    MovingAverageCrossoverConfig cfg = make_config(2, 3);
    cfg.strategy_id                  = "sma_2_3";
    cfg.requested_quantity           = 25;
    Harness h{cfg};
    h.feed_closes(kFixture);
    ASSERT_EQ(h.signals.size(), 2u);
    EXPECT_EQ(h.sma.id(), "sma_2_3");

    for (const domain::TradeSignal& signal : h.signals) {
        EXPECT_EQ(signal.symbol, "AAPL");
        EXPECT_EQ(signal.requested_quantity, std::optional<common::Quantity>{25});
        EXPECT_EQ(signal.order_type, std::optional<domain::OrderType>{domain::OrderType::Market});
        EXPECT_FALSE(signal.target_exposure.has_value());
        EXPECT_FALSE(signal.limit_price.has_value());
        EXPECT_FALSE(signal.confidence.has_value());
        // The engine owns these three and stamps them (see ISignalSink).
        EXPECT_FALSE(signal.id.valid());
        EXPECT_TRUE(signal.strategy_id.empty());
        EXPECT_EQ(signal.created_at, common::Timestamp{});
    }

    using Metadata = std::map<std::string, std::string>;
    EXPECT_EQ(h.signals[0].side, SignalSide::Buy);
    EXPECT_EQ(h.signals[0].metadata,
              (Metadata{{"trigger", "short_crossed_above_long"},
                        {"short_window", "2"},
                        {"long_window", "3"},
                        {"short_sma", "2.5"},
                        {"long_sma", "2"}}));
    EXPECT_EQ(h.signals[1].side, SignalSide::Sell);
    EXPECT_EQ(h.signals[1].metadata,
              (Metadata{{"trigger", "short_crossed_below_long"},
                        {"short_window", "2"},
                        {"long_window", "3"},
                        {"short_sma", "2.5"},
                        {"long_sma", "3"}}));
}
