// MeanReversionStrategy driven directly, without an engine: its configuration,
// warm-up, the statistics it computes, the latch rules (entry, suppression, rearm,
// direct transitions, constant windows), window eviction, symbols and the signals
// it emits.
//
// Every expected value was worked out by hand (the arithmetic is in the comments)
// and cross-checked with an independent exact-fraction script; none is read back
// from the implementation. What it ignores, numerical edge cases, how it restarts
// and a long run against an exact integer reference are in
// mean_reversion_robustness_test.cpp; the real StrategyEngine in
// mean_reversion_engine_test.cpp.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "mean_reversion_fixture.hpp"
#include "trading_engine/common/errors.hpp"

namespace {

using namespace mr_test;
using domain::SignalSide;
using strategy::MeanReversionConfig;
using strategy::MeanReversionStrategy;

struct Case {
    const char* name;
    std::size_t lookback;
    double      entry;
    double      rearm;
    Closes      closes;
    Labels      expected;
};

void expect_labels(const std::vector<Case>& cases) {
    for (const Case& scenario : cases) {
        SCOPED_TRACE(scenario.name);
        Harness h{make_config(scenario.lookback, scenario.entry, scenario.rearm)};
        h.feed_closes(scenario.closes);
        EXPECT_EQ(h.labels(), scenario.expected);
    }
}

// What constructing the strategy from `cfg` throws: its message if a
// common::ConfigError, "<accepted>" if nothing, "<other>" for anything else.
std::string config_error(MeanReversionConfig cfg) {
    try {
        const MeanReversionStrategy accepted{std::move(cfg)};
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

TEST(MeanReversionConfigTest, DefaultsAreTwentyBarsTwoAndAHalfOneShareAndTheMeanReversionId) {
    const MeanReversionConfig defaults;
    EXPECT_EQ(defaults.strategy_id, "mean_reversion");
    EXPECT_EQ(defaults.lookback, 20u);
    EXPECT_EQ(defaults.entry_threshold, 2.0);
    EXPECT_EQ(defaults.rearm_threshold, 0.5);
    EXPECT_EQ(defaults.requested_quantity, 1);
    EXPECT_TRUE(defaults.symbols.empty()) << "the allowlist has no default";

    // Behaviourally: nineteen closes of 100, then 110. The first full window is
    // 19 x 100 and a 110: mean (1900 + 110) / 20 = 100.5, variance (19 x 0.5^2 +
    // 9.5^2) / 20 = 95 / 20 = 4.75, z = 9.5 / sqrt(4.75) = 4.3588989 >= 2: Sell.
    MeanReversionConfig only_symbols;
    only_symbols.symbols = {"AAPL"};
    Harness h{only_symbols};
    EXPECT_EQ(h.mr.id(), "mean_reversion");
    Closes closes(19, 100_px);
    closes.push_back(110_px);
    h.feed_closes(closes);
    EXPECT_EQ(h.labels(), (Labels{"sell@20"}));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_EQ(h.signals[0].requested_quantity, std::optional<common::Quantity>{1});
    EXPECT_EQ(h.signals[0].metadata.at("lookback"), "20");
    EXPECT_EQ(h.signals[0].metadata.at("entry_threshold"), "2");
    EXPECT_EQ(h.signals[0].metadata.at("rearm_threshold"), "0.5");
    EXPECT_NEAR(number(h.signals[0], "mean"), 100.5, 1e-12);
    EXPECT_NEAR(number(h.signals[0], "z_score"), 4.358898943540674, 1e-9);
}

TEST(MeanReversionConfigTest, AcceptsValidConfigurations) {
    EXPECT_EQ(config_error(make_config(2, 1.0, 0.5)), "<accepted>");   // the smallest window
    EXPECT_EQ(config_error(make_config(3, 1.5, 0.0)), "<accepted>");   // rearm 0 is allowed
    EXPECT_EQ(config_error(make_config(250, 3.0, 1.0, {"AAPL", "MSFT", "BRK.B"})), "<accepted>");
    EXPECT_EQ(config_error(make_config(5, 1.0e300, 0.5)), "<accepted>") << "large but finite";
    for (const common::Quantity whole : {common::Quantity{1}, common::Quantity{2}, common::Quantity{100},
                                         common::Quantity{1'000'000},
                                         std::numeric_limits<common::Quantity>::max()}) {
        MeanReversionConfig cfg = make_config();
        cfg.requested_quantity  = whole;
        EXPECT_EQ(config_error(cfg), "<accepted>") << whole;
    }
}

TEST(MeanReversionConfigTest, RejectsALookbackBelowTwoAndNamesTheValue) {
    for (const std::size_t bad : {std::size_t{0}, std::size_t{1}}) {
        const std::string message = config_error(make_config(bad));
        EXPECT_TRUE(mentions(message, "MeanReversionStrategy: lookback")) << message;
        EXPECT_TRUE(mentions(message, "(got " + std::to_string(bad) + ")")) << message;
    }
}

TEST(MeanReversionConfigTest, RejectsThresholdsThatAreNotFiniteOrNotZeroLessThanRearmLessThanEntry) {
    for (const double bad : {kNaN, kInf, -kInf}) {
        const std::string entry = config_error(make_config(4, bad, 0.5));
        EXPECT_TRUE(mentions(entry, "entry_threshold")) << bad << ": " << entry;
        const std::string rearm = config_error(make_config(4, 5.0, bad));
        EXPECT_TRUE(mentions(rearm, "rearm_threshold")) << bad << ": " << rearm;
    }
    EXPECT_TRUE(mentions(config_error(make_config(4, 1.5, -0.1)), "rearm_threshold"));

    struct Bad { double entry; double rearm; };
    for (const Bad& bad : {Bad{0.5, 0.5}, Bad{1.5, 1.5}, Bad{1.5, 2.0}, Bad{0.0, 0.0}, Bad{-1.0, 0.0}}) {
        const std::string message = config_error(make_config(4, bad.entry, bad.rearm));
        EXPECT_TRUE(mentions(message, "rearm_threshold must be less than entry_threshold"))
            << bad.rearm << " vs " << bad.entry << ": " << message;
    }
    // The message shows both values.
    const std::string message = config_error(make_config(4, 1.5, 2.0));
    EXPECT_TRUE(mentions(message, "rearm_threshold 2")) << message;
    EXPECT_TRUE(mentions(message, "entry_threshold 1.5")) << message;
}

TEST(MeanReversionConfigTest, RejectsAnEmptyStrategyId) {
    MeanReversionConfig cfg = make_config();
    cfg.strategy_id.clear();
    EXPECT_TRUE(mentions(config_error(cfg), "strategy_id"));
}

TEST(MeanReversionConfigTest, RejectsAMissingEmptyOrRepeatedSymbol) {
    EXPECT_TRUE(mentions(config_error(make_config(4, 1.5, 0.5, {})), "symbols"));
    EXPECT_TRUE(mentions(config_error(make_config(4, 1.5, 0.5, {"AAPL", ""})), "empty entry"));
    EXPECT_TRUE(mentions(config_error(make_config(4, 1.5, 0.5, {""})), "empty entry"));
    const std::string repeated = config_error(make_config(4, 1.5, 0.5, {"AAPL", "MSFT", "AAPL"}));
    EXPECT_TRUE(mentions(repeated, "AAPL")) << repeated;
    EXPECT_TRUE(mentions(repeated, "more than once")) << repeated;
}

TEST(MeanReversionConfigTest, RejectsQuantitiesThatAreNotPositive) {
    // Quantity is an exact integer count of shares, so a fraction cannot even be written; only
    // the sign needs checking. The message shows the offending value.
    for (const common::Quantity bad : {common::Quantity{0}, common::Quantity{-1}, common::Quantity{-1'000},
                                       std::numeric_limits<common::Quantity>::min()}) {
        MeanReversionConfig cfg = make_config();
        cfg.requested_quantity  = bad;
        const std::string message = config_error(cfg);
        EXPECT_TRUE(mentions(message, "requested_quantity")) << bad << ": " << message;
        EXPECT_TRUE(mentions(message, std::to_string(bad))) << message;
    }
}

// --- Warm-up -------------------------------------------------------------------

TEST(MeanReversionWarmUp, EmitsNothingUntilTheWindowIsFull) {
    expect_labels({
        {"three bars of a lookback-4 window", 4, 1.5, 0.5, units({10, 10, 10}), {}},
        {"a violent move inside the warm-up", 4, 1.5, 0.5, units({1, 1000, 1}), {}},
        {"a single bar", 2, 1.0, 0.5, units({10}), {}},
    });
}

TEST(MeanReversionWarmUp, TheFirstFullWindowCanSignalWithNoSilentBaseline) {
    // Unlike the SMA strategy, nothing is swallowed as a baseline: bar 4 of the
    // fixture, the first full window, is already a Buy (z = -1.7320508 <= -1.5).
    // Lookback 2 is the extreme case: the second bar is the first full window.
    expect_labels({
        {"the fixture's fourth bar", 4, 1.5, 0.5, units({10, 10, 10, 6}), {"buy@4"}},
        {"lookback 2, up", 2, 1.0, 0.5, units({10, 12}), {"sell@2"}},
        {"lookback 2, down", 2, 1.0, 0.5, units({12, 10}), {"buy@2"}},
    });
}

TEST(MeanReversionWarmUp, ALookbackOfTwoCapsTheZScoreAtOneSoTheDefaultThresholdNeverFires) {
    // With N = 2 the two closes sit exactly one deviation either side of their
    // mean, so |z| = 1 whatever they are: (10, 1000) is z = +1, never 2.
    expect_labels({
        {"entry 2.0 with lookback 2", 2, 2.0, 0.5, units({10, 1000, 1, 1000, 1}), {}},
        {"entry 1.0 with lookback 2", 2, 1.0, 0.5, units({10, 1000, 1}), {"sell@2", "buy@3"}},
    });
}

// --- The fixture from the task --------------------------------------------------

TEST(MeanReversionFixture, GivesBuySellAndTheRearmExactlyAsSpecified) {
    // lookback 4, entry 1.5, rearm 0.5, closes 10 10 10 6 9 2 30. By hand:
    //  bar 4 [10 10 10 6]  mean 9      variance (1+1+1+9)/4 = 3        z = -3/sqrt(3)       = -1.7320508  Buy
    //  bar 5 [10 10  6 9]  mean 8.75   variance 2.6875                 z = 0.25/1.6393596   =  0.1524986  rearm (<= 0.5)
    //  bar 6 [10  6  9 2]  mean 6.75   variance 9.6875                 z = -4.75/3.1124749  = -1.5261167  Buy
    //  bar 7 [ 6  9  2 30] mean 11.75  variance 117.1875               z = 18.25/10.825317  =  1.6858628  Sell
    Harness h{make_config()};
    h.feed_closes(kFixture);

    EXPECT_EQ(h.labels(), (Labels{"buy@4", "buy@6", "sell@7"}));
    ASSERT_EQ(h.signals.size(), 3u);
    EXPECT_EQ(h.signals[0].side, SignalSide::Buy);
    EXPECT_EQ(h.signals[1].side, SignalSide::Buy);
    EXPECT_EQ(h.signals[2].side, SignalSide::Sell);

    // The statistics on each signalling bar: mean, population variance (as the
    // square of the reported deviation) and z, each worked out above.
    // The close is an exact Price and is reported as its exact decimal text.
    struct Expected { const char* close; double mean; double variance; double z; };
    const Expected expected[] = {
        {"6", 9.0, 3.0, -1.7320508075688772},
        {"2", 6.75, 9.6875, -1.5261167249147478},
        {"30", 11.75, 117.1875, 1.6858627860337072},
    };
    for (std::size_t i = 0; i < 3; ++i) {
        SCOPED_TRACE("signal " + std::to_string(i));
        EXPECT_EQ(h.signals[i].metadata.at("close"), expected[i].close);
        EXPECT_NEAR(number(h.signals[i], "mean"), expected[i].mean, 1e-12);
        const double deviation = number(h.signals[i], "standard_deviation");
        EXPECT_NEAR(deviation * deviation, expected[i].variance, 1e-9);
        EXPECT_NEAR(number(h.signals[i], "z_score"), expected[i].z, 1e-9);
    }
}

TEST(MeanReversionFixture, UsesThePopulationDeviationAndIncludesTheCurrentClose) {
    // Bar 4 of the fixture separates the conventions. Population (divide by N) gives
    // z = -1.7320508; the sample deviation (N-1) would give -1.5, and leaving the
    // current 6 out of the window would give a mean of 10 and a deviation of 0.
    Harness h{make_config()};
    h.feed_closes(units({10, 10, 10, 6}));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_NEAR(number(h.signals[0], "mean"), 9.0, 1e-12);
    EXPECT_NEAR(number(h.signals[0], "z_score"), -std::sqrt(3.0), 1e-12);
    EXPECT_NE(number(h.signals[0], "z_score"), -1.5);
}

TEST(MeanReversionFixture, AMissedRearmChangesTheOutcome) {
    // Bar 5's z is +0.1525. With the default 0.5 that rearms, and bar 6 buys again.
    // With rearm 0.1 it does not (0.1525 > 0.1, and it is not an extreme either): the
    // latch stays LowerExtreme, bar 6's Buy is suppressed, and bar 7 is a Sell.
    Harness rearmed{make_config(4, 1.5, 0.5)};
    rearmed.feed_closes(kFixture);
    EXPECT_EQ(rearmed.labels(), (Labels{"buy@4", "buy@6", "sell@7"}));

    Harness held{make_config(4, 1.5, 0.1)};
    held.feed_closes(kFixture);
    EXPECT_EQ(held.labels(), (Labels{"buy@4", "sell@7"}));
}

// --- Thresholds ------------------------------------------------------------------

TEST(MeanReversionThresholds, EntryIsInclusiveOnBothSides) {
    // Lookback 5, one close five above (or below) four 10s. The window is
    // [10 10 10 10 15]: mean 11, variance (4 x 1 + 16) / 5 = 4, deviation 2,
    // z = 4 / 2 = +2 exactly (every number is an exact integer count of micros). Likewise
    // [10 10 10 10 5]: mean 9, variance 4, z = -2. At entry 2.0 both are on the
    // boundary and fire; the next representable thresholds above do not.
    expect_labels({
        {"upper boundary fires", 5, 2.0, 0.5, units({10, 10, 10, 10, 15}), {"sell@5"}},
        {"lower boundary fires", 5, 2.0, 0.5, units({10, 10, 10, 10, 5}), {"buy@5"}},
        {"just above, upper", 5, 2.0000001, 0.5, units({10, 10, 10, 10, 15}), {}},
        {"just above, lower", 5, 2.0000001, 0.5, units({10, 10, 10, 10, 5}), {}},
        {"just below fires, upper", 5, 1.9999999, 0.5, units({10, 10, 10, 10, 15}), {"sell@5"}},
        {"just below fires, lower", 5, 1.9999999, 0.5, units({10, 10, 10, 10, 5}), {"buy@5"}},
    });
}

TEST(MeanReversionThresholds, TheBoundaryIsReachableOnlyBecauseTheWindowIsBigEnough) {
    // |z| <= sqrt(N - 1): a threshold of 2 needs N >= 5. Four bars cannot reach it,
    // whatever the closes: [10 10 10 100] is the most extreme possible and gives
    // z = sqrt(3) = 1.7320508, below 2.
    expect_labels({
        {"the extreme four-bar window", 4, 2.0, 0.5, units({10, 10, 10, 100}), {}},
        {"the same at entry 1.7", 4, 1.7, 0.5, units({10, 10, 10, 100}), {"sell@4"}},
    });
}

TEST(MeanReversionThresholds, RearmIsInclusiveOnBothSides) {
    // Rearm 1.0, entry 1.4, lookback 4. After a Buy on [10 10 10 6], bar 5 is
    // [10 10 6 6]: mean 8, variance 4, z = -2/2 = -1 exactly, which is <= 1.0 and
    // rearms. Bar 6 [10 6 6 2] has mean 6, variance 8, z = -4/2.8284271 = -1.4142136
    // <= -1.4: a second Buy, possible only because bar 5 rearmed. With rearm
    // 0.9999999 bar 5 is not inside the band, the latch holds, and bar 6 is silent.
    // The upper side mirrors it: [10 10 10 14], [10 10 14 14] (z = +1), [10 14 14 18].
    expect_labels({
        {"lower, rearm exactly 1", 4, 1.4, 1.0, units({10, 10, 10, 6, 6, 2}), {"buy@4", "buy@6"}},
        {"lower, rearm just below 1", 4, 1.4, 0.9999999, units({10, 10, 10, 6, 6, 2}), {"buy@4"}},
        {"upper, rearm exactly 1", 4, 1.4, 1.0, units({10, 10, 10, 14, 14, 18}), {"sell@4", "sell@6"}},
        {"upper, rearm just below 1", 4, 1.4, 0.9999999, units({10, 10, 10, 14, 14, 18}), {"sell@4"}},
    });
}

// --- Suppression and rearming -------------------------------------------------------

TEST(MeanReversionLatch, ARepeatedExtremeDuringOneExcursionEmitsOnce) {
    // [10 10 10 6 2 1 1 1 1 10], lookback 4, entry 1.5, rearm 0.5:
    //  bar 4 z = -1.7320508 Buy; bar 5 [10 10 6 2] z = -1.5075567 still beyond the
    //  entry: suppressed; bars 6-8 z = -1.0528, -0.7276, -0.5774: between the bands,
    //  held; bar 9 [1 1 1 1] is constant: rearm; bar 10 [1 1 1 10] z = +1.7320508 Sell.
    // The mirror image ends with a Buy.
    expect_labels({
        {"downward excursion", 4, 1.5, 0.5, units({10, 10, 10, 6, 2, 1, 1, 1, 1, 10}), {"buy@4", "sell@10"}},
        {"upward excursion", 4, 1.5, 0.5, units({10, 10, 10, 14, 18, 19, 19, 19, 19, 1}), {"sell@4", "buy@10"}},
    });
}

TEST(MeanReversionLatch, BetweenTheBandsTheLatchHoldsAndAnotherExtremeIsStillSuppressed) {
    // Same start, but bar 9 is a fresh extreme: [1 1 1 0.1] has z = -1.7320508 while
    // bars 5-8 never came inside the rearm band (their |z| stayed above 0.5, the
    // smallest being 0.5774). The latch never reset, so there is no second Buy.
    expect_labels({
        {"never rearmed", 4, 1.5, 0.5, {10_px, 10_px, 10_px, 6_px, 2_px, 1_px, 1_px, 1_px, 0.1_px}, {"buy@4"}},
    });
}

TEST(MeanReversionLatch, ANeutralBarRearmsTheSameSide) {
    // The fixture again: bar 5 is inside the band, so bar 6 may buy a second time.
    // A constant window rearms too: [10 10 10 6 6 6 6 2] buys at 4, goes through
    // z = -1 and -0.5774 (held), reaches [6 6 6 6] (constant, rearm) at bar 7 and
    // buys again at bar 8, where [6 6 6 2] has z = -1.7320508.
    expect_labels({
        {"band rearm", 4, 1.5, 0.5, kFixture, {"buy@4", "buy@6", "sell@7"}},
        {"constant-window rearm", 4, 1.5, 0.5, units({10, 10, 10, 6, 6, 6, 6, 2}), {"buy@4", "buy@8"}},
    });
}

TEST(MeanReversionLatch, OppositeExtremesFlipDirectlyWithNoNeutralBarBetween) {
    // Bar 6 -> 7 of the fixture is Buy then Sell; these two are the tightest cases.
    // [10 10 10 6 30]: bar 5 [10 10 6 30] has mean 14, variance 100, z = +1.7056057.
    // [10 10 10 14 2]: bar 5 [10 10 14 2] has mean 9, variance 19, z = -1.6059101.
    expect_labels({
        {"down to up", 4, 1.5, 0.5, units({10, 10, 10, 6, 30}), {"buy@4", "sell@5"}},
        {"up to down", 4, 1.5, 0.5, units({10, 10, 10, 14, 2}), {"sell@4", "buy@5"}},
    });
}

// --- Constant windows ---------------------------------------------------------------

TEST(MeanReversionConstantWindow, EmitsNothingWhateverTheLevel) {
    expect_labels({
        {"flat at 5", 4, 1.5, 0.5, units({5, 5, 5, 5, 5, 5, 5, 5}), {}},
        {"flat at 100.1", 4, 1.5, 0.5, Closes(12, 100.1_px), {}},
        {"flat at the smallest price, one micro", 3, 1.0, 0.5, Closes(9, micros(1)), {}},
        {"flat at the largest price, INT64_MAX millionths", 3, 1.0, 0.5, Closes(9, kMaxPrice), {}},
    });
}

TEST(MeanReversionConstantWindow, ThirtyTenthsDoNotLookLikeADeviation) {
    // 0.1 is exactly 100'000 micros, so thirty of them have a deviation of exactly 0 (with
    // doubles, 0.1 summed thirty times is not 3.0 and a naive pass divides noise by noise).
    // Thirty equal closes must stay silent, and keep doing so as a ring that has turned
    // several times.
    Harness h{make_config(30, 1.5, 0.5)};
    h.feed_closes(Closes(200, 0.1_px));
    EXPECT_TRUE(h.signals.empty());

    // A real move afterwards is still seen: one close of 0.2 among 29 of 0.1 has
    // z = +sqrt(29) = 5.385 >= 1.5.
    h.feed_closes({0.2_px}, "AAPL", 1000);
    EXPECT_EQ(h.labels(), (Labels{"sell@1000"}));
}

TEST(MeanReversionConstantWindow, ASpreadBelowOnePartInATrillionOfTheMeanIsStillConstant) {
    // A window is constant when its deviation is at most 1e-12 of its mean. One micro
    // (1e-6) higher in a window of four has a deviation of 0.433 micros, so:
    //   at 100 (1e8 micros) the ratio is 4.3e-9: a real deviation, z = +sqrt(3) = 1.73 >= 1.5;
    //   at 2,000,000 (2e12 micros) the ratio is 2.2e-13: below the tolerance, no deviation,
    //   so it neither signals nor reads that one close as a +1.7 deviation.
    // 100.001 (1e-5 relative) is real.
    expect_labels({
        {"one micro higher at 100 is real", 4, 1.5, 0.5, {100_px, 100_px, 100_px, 100.000001_px}, {"sell@4"}},
        {"one micro higher at two million is constant", 4, 1.5, 0.5,
         {2000000_px, 2000000_px, 2000000_px, 2000000.000001_px}, {}},
        {"one close a thousandth higher", 4, 1.5, 0.5, {100_px, 100_px, 100_px, 100.001_px}, {"sell@4"}},
    });
}

// --- Eviction and symbols ------------------------------------------------------------

TEST(MeanReversionWindow, ACloseThatHasLeftTheWindowNoLongerCounts) {
    // Lookback 3, entry 1.2. [1000 10 10] is warm but quiet (z = -0.7071). On the
    // next bar the 1000 is evicted: [10 10 6] has mean 26/3, variance 32/9 and
    // z = (6 - 8.667)/1.8856 = -1.4142 <= -1.2: Buy, with a mean that no longer
    // contains the 1000. If it were still counted the window would be [1000 10 6].
    Harness h{make_config(3, 1.2, 0.5)};
    h.feed_closes(units({1000, 10, 10, 6}));
    EXPECT_EQ(h.labels(), (Labels{"buy@4"}));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_NEAR(number(h.signals[0], "mean"), 26.0 / 3.0, 1e-12);
    EXPECT_NEAR(number(h.signals[0], "z_score"), -std::sqrt(2.0), 1e-9);
}

TEST(MeanReversionWindow, ARingThatHasTurnedUsesOnlyTheLastLookbackCloses) {
    // Lookback 4: 50 60 70 80 then twenties. Bar 5 [60 70 80 20] is a Buy (z -1.646),
    // the old fifties-to-eighties then scroll out: z -0.99 and -0.5774 (held), and at
    // bar 8 [20 20 20 20] is constant (rearm). Bar 12 is [20 20 20 12]: mean 18,
    // variance (4+4+4+36)/4 = 12, z = -6/3.4641016 = -1.7320508: a second Buy whose
    // statistics contain nothing older than bar 9.
    Harness h{make_config()};
    h.feed_closes(units({50, 60, 70, 80, 20, 20, 20, 20, 20, 20, 20, 12}));
    EXPECT_EQ(h.labels(), (Labels{"buy@5", "buy@12"}));
    ASSERT_EQ(h.signals.size(), 2u);
    EXPECT_NEAR(number(h.signals[1], "mean"), 18.0, 1e-12);
    EXPECT_NEAR(number(h.signals[1], "z_score"), -std::sqrt(3.0), 1e-9);
}

TEST(MeanReversionSymbols, AreIndependentWhenInterleaved) {
    // AAPL runs the fixture; MSFT runs [5 5 5 5 9] (constant, then a jump: its first
    // full window is [5 5 5 5], and bar 5 [5 5 5 9] is z = +1.7320508, a Sell).
    // Interleaved bar by bar at the same minutes, each must give what it would alone.
    const Closes msft = units({5, 5, 5, 5, 9});
    Harness alone_aapl{make_config(4, 1.5, 0.5, {"AAPL", "MSFT"})};
    alone_aapl.feed_closes(kFixture, "AAPL");
    Harness alone_msft{make_config(4, 1.5, 0.5, {"AAPL", "MSFT"})};
    alone_msft.feed_closes(msft, "MSFT");

    Harness both{make_config(4, 1.5, 0.5, {"AAPL", "MSFT"})};
    for (long long minute = 1; minute <= static_cast<long long>(kFixture.size()); ++minute) {
        const auto i = static_cast<std::size_t>(minute) - 1;
        both.feed(bar("AAPL", kFixture[i], minute));
        if (i < msft.size()) {
            both.feed(bar("MSFT", msft[i], minute));
        }
    }
    EXPECT_EQ(both.labels("AAPL"), (Labels{"buy@4", "buy@6", "sell@7"}));
    EXPECT_EQ(both.labels("MSFT"), (Labels{"sell@5"}));
    EXPECT_EQ(both.labels("AAPL"), alone_aapl.labels("AAPL"));
    EXPECT_EQ(both.labels("MSFT"), alone_msft.labels("MSFT"));
}

TEST(MeanReversionSymbols, OneSymbolsLatchAndClockNeverConstrainAnother) {
    // AAPL is LowerExtreme after bar 4; MSFT, fed after it with earlier timestamps
    // than AAPL's last, still starts Neutral and buys on its own first extreme.
    Harness h{make_config(4, 1.5, 0.5, {"AAPL", "MSFT"})};
    h.feed_closes(units({10, 10, 10, 6}), "AAPL", 100);
    h.feed_closes(units({10, 10, 10, 6}), "MSFT", 1);
    EXPECT_EQ(h.labels("AAPL"), (Labels{"buy@103"}));
    EXPECT_EQ(h.labels("MSFT"), (Labels{"buy@4"}));
}

// --- Signals -----------------------------------------------------------------------

TEST(MeanReversionSignals, CarryTheRequestFieldsAndLeaveTheEnginesFieldsAlone) {
    MeanReversionConfig cfg = make_config();
    cfg.requested_quantity  = 25;
    Harness h{cfg};
    h.feed_closes(kFixture);
    ASSERT_EQ(h.signals.size(), 3u);

    for (const domain::TradeSignal& signal : h.signals) {
        EXPECT_EQ(signal.symbol, "AAPL");
        EXPECT_EQ(signal.requested_quantity, std::optional<common::Quantity>{25});
        EXPECT_EQ(signal.order_type, std::optional<domain::OrderType>{domain::OrderType::Market});
        EXPECT_FALSE(signal.target_exposure.has_value());
        EXPECT_FALSE(signal.limit_price.has_value());
        EXPECT_FALSE(signal.confidence.has_value());
        // Identity and time belong to the StrategyEngine.
        EXPECT_FALSE(signal.id.valid());
        EXPECT_TRUE(signal.strategy_id.empty());
        EXPECT_EQ(signal.created_at, common::Timestamp{});
    }
}

TEST(MeanReversionSignals, CarryExactlyTheDocumentedMetadata) {
    Harness h{make_config()};
    h.feed_closes(kFixture);
    ASSERT_EQ(h.signals.size(), 3u);

    const std::set<std::string> keys{"trigger", "lookback", "close", "mean", "standard_deviation",
                                     "z_score", "entry_threshold", "rearm_threshold"};
    for (const domain::TradeSignal& signal : h.signals) {
        std::set<std::string> actual;
        for (const auto& [key, value] : signal.metadata) {
            actual.insert(key);
            EXPECT_FALSE(value.empty()) << key;
        }
        EXPECT_EQ(actual, keys);
        EXPECT_EQ(signal.metadata.at("lookback"), "4");
        EXPECT_EQ(signal.metadata.at("entry_threshold"), "1.5");
        EXPECT_EQ(signal.metadata.at("rearm_threshold"), "0.5");
        for (const char* numeric : {"close", "mean", "standard_deviation", "z_score"}) {
            EXPECT_TRUE(std::isfinite(number(signal, numeric))) << numeric;
        }
    }
    EXPECT_EQ(h.signals[0].metadata.at("trigger"), "z_score_at_or_below_lower_entry");
    EXPECT_EQ(h.signals[1].metadata.at("trigger"), "z_score_at_or_below_lower_entry");
    EXPECT_EQ(h.signals[2].metadata.at("trigger"), "z_score_at_or_above_upper_entry");
    EXPECT_EQ(h.signals[0].metadata.at("close"), "6");
    EXPECT_EQ(h.signals[0].metadata.at("mean"), "9");
    EXPECT_LT(number(h.signals[0], "z_score"), 0.0);
    EXPECT_GT(number(h.signals[2], "z_score"), 0.0);
}

TEST(MeanReversionSignals, TheCloseInTheMetadataIsTheExactDecimalOfTheIntegerPrice) {
    // Lookback 2, entry 1: the second bar is the first full window and |z| is exactly 1.
    // The close is written from the integer (150'020'000 micros), never through a double.
    MeanReversionConfig cfg = make_config(2, 1.0, 0.5);
    Harness h{cfg};
    h.feed_closes({100_px, 150.02_px});
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_EQ(h.signals[0].side, SignalSide::Sell);
    EXPECT_EQ(h.signals[0].metadata.at("close"), "150.02");

    Harness micro{cfg};
    micro.feed_closes({100_px, 100.000001_px});   // one micro above
    ASSERT_EQ(micro.signals.size(), 1u);
    EXPECT_EQ(micro.signals[0].metadata.at("close"), "100.000001");
}
