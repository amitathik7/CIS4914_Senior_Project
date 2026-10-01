// MeanReversionStrategy under awkward input and over long runs: events it must
// ignore without a trace, numerical edge cases (overflow, cancellation, extreme
// magnitudes) and recovery from them, how on_start() resets a run, and a long
// random run checked against an exact integer reference. The behaviour on
// well-formed bars is in mean_reversion_strategy_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "mean_reversion_fixture.hpp"

namespace {

using namespace mr_test;
using domain::MarketEventType;

const Labels kFixtureLabels{"buy@4", "buy@6", "sell@7"};

// Every kind of event that must be ignored, all stamped far in the future
// (minute 1000). If any of them advanced the timestamp, the history or the latch,
// the real bars that follow (minutes 1 and up) would be dropped or wrong.
std::vector<domain::MarketEvent> ignorable_events() {
    std::vector<domain::MarketEvent> events;
    for (const MarketEventType type : {MarketEventType::Unknown, MarketEventType::Trade,
                                       MarketEventType::Quote, MarketEventType::Status}) {
        domain::MarketEvent event = bar("AAPL", 5.0, 1000);
        event.type                = type;
        events.push_back(event);
    }
    events.push_back(bar("TSLA", 5.0, 1000));   // not on the allowlist
    events.push_back(bar("aapl", 5.0, 1000));   // not an exact match
    domain::MarketEvent missing = bar("AAPL", 5.0, 1000);
    missing.price.reset();
    events.push_back(missing);
    for (const double bad : {kNaN, kInf, -kInf, 0.0, -0.0, -5.0}) {
        events.push_back(bar("AAPL", bad, 1000));
    }
    return events;
}

void expect_finite_metadata(const domain::TradeSignal& signal) {
    for (const char* key : {"close", "mean", "standard_deviation", "z_score", "entry_threshold",
                            "rearm_threshold"}) {
        EXPECT_TRUE(std::isfinite(number(signal, key))) << key << " = " << signal.metadata.at(key);
    }
}

// A clean run of `closes` is the yardstick: noise must leave both the labels and
// every metadata value exactly as they were.
void expect_noise_changes_nothing(const Closes& closes, std::size_t lookback, double entry,
                                  double rearm, const Labels& expected_labels) {
    Harness clean{make_config(lookback, entry, rearm)};
    clean.feed_closes(closes);
    ASSERT_EQ(clean.labels(), expected_labels);

    Harness noisy{make_config(lookback, entry, rearm)};
    long long minute = 0;
    for (const double close : closes) {
        for (const domain::MarketEvent& ignored : ignorable_events()) {
            noisy.feed(ignored);
        }
        noisy.feed(bar("AAPL", close, ++minute));
    }
    EXPECT_EQ(noisy.labels(), clean.labels());
    ASSERT_EQ(noisy.signals.size(), clean.signals.size());
    for (std::size_t i = 0; i < clean.signals.size(); ++i) {
        EXPECT_EQ(noisy.signals[i].metadata, clean.signals[i].metadata) << "signal " << i;
    }
}

}  // namespace

// --- Ignored events ----------------------------------------------------------------

TEST(MeanReversionIgnoredEvents, ChangeNothing) {
    expect_noise_changes_nothing(kFixture, 4, 1.5, 0.5, kFixtureLabels);
}

TEST(MeanReversionIgnoredEvents, CannotRearmOrResetALatch) {
    // A bad bar is not an observation, so it must not read as a neutral one. These
    // runs hold a latch across the noise (a suppressed repeat) and cross a constant
    // window (a genuine rearm): the noisy run must match the clean one exactly.
    expect_noise_changes_nothing({10, 10, 10, 6, 2, 1, 1, 1, 0.1}, 4, 1.5, 0.5, {"buy@4"});
    expect_noise_changes_nothing({10, 10, 10, 6, 6, 6, 6, 2}, 4, 1.5, 0.5, {"buy@4", "buy@8"});
}

TEST(MeanReversionIgnoredEvents, DuplicateAndOlderBarsChangeNothing) {
    Harness noisy{make_config()};
    for (std::size_t i = 0; i < kFixture.size(); ++i) {
        const auto minute = static_cast<long long>(i) + 1;
        noisy.feed(bar("AAPL", kFixture[i], minute));
        noisy.feed(bar("AAPL", kFixture[i], minute));   // an exact re-delivery
        noisy.feed(bar("AAPL", 1000.0, minute));        // the same time, another price
        noisy.feed(bar("AAPL", 1000.0, minute - 1));    // not later than the last accepted bar
        noisy.feed(bar("AAPL", 1000.0, 0));             // older than every accepted bar
    }
    Harness clean{make_config()};
    clean.feed_closes(kFixture);
    EXPECT_EQ(noisy.labels(), kFixtureLabels);
    ASSERT_EQ(noisy.signals.size(), clean.signals.size());
    for (std::size_t i = 0; i < clean.signals.size(); ++i) {
        EXPECT_EQ(noisy.signals[i].metadata, clean.signals[i].metadata) << "signal " << i;
    }
}

TEST(MeanReversionIgnoredEvents, GapsInTimeAreNeverFilledIn) {
    // The window counts accepted bars: the fixture's signals still fire when the
    // bars are hours apart, at the minutes they actually carry.
    Harness h{make_config()};
    const std::vector<long long> minutes{1, 2, 3, 10, 20, 21, 50};
    for (std::size_t i = 0; i < kFixture.size(); ++i) {
        h.feed(bar("AAPL", kFixture[i], minutes[i]));
    }
    EXPECT_EQ(h.labels(), (Labels{"buy@10", "buy@21", "sell@50"}));
}

TEST(MeanReversionIgnoredEvents, ASymbolsRejectedBarDoesNotTouchAnotherSymbolsClock) {
    // AAPL is at minute 7. A MSFT bar at minute 2 is MSFT's first accepted bar; an
    // AAPL bar at minute 2 is old. Each symbol keeps its own last timestamp.
    Harness h{make_config(2, 1.0, 0.5, {"AAPL", "MSFT"})};
    h.feed(bar("AAPL", 10.0, 7));
    h.feed(bar("AAPL", 12.0, 2));    // older than AAPL's last: ignored
    h.feed(bar("MSFT", 10.0, 1));
    h.feed(bar("MSFT", 12.0, 2));    // MSFT's second bar: first full window, a Sell
    EXPECT_EQ(h.labels("MSFT"), (Labels{"sell@2"}));
    EXPECT_TRUE(h.labels("AAPL").empty()) << "AAPL has only one accepted bar";
}

// --- Numerical edge cases ------------------------------------------------------------

TEST(MeanReversionNumerics, TheZScoreDoesNotDependOnThePriceLevel) {
    // z is a ratio, so the fixture scaled by anything gives the fixture's labels
    // and z-scores, from the tiniest normal prices to the largest: 1e300 times 30
    // would overflow a plain sum of squares many times over.
    Harness reference{make_config()};
    reference.feed_closes(kFixture);
    ASSERT_EQ(reference.signals.size(), 3u);

    for (const double scale : {1e-300, 1e-150, 1e-3, 1e3, 1e150, 1e300}) {
        SCOPED_TRACE(scale);
        Closes closes;
        for (const double close : kFixture) {
            closes.push_back(close * scale);
        }
        Harness h{make_config()};
        h.feed_closes(closes);
        EXPECT_EQ(h.labels(), kFixtureLabels);
        ASSERT_EQ(h.signals.size(), 3u);
        for (std::size_t i = 0; i < 3; ++i) {
            expect_finite_metadata(h.signals[i]);
            EXPECT_NEAR(number(h.signals[i], "z_score"), number(reference.signals[i], "z_score"), 1e-9);
            EXPECT_NEAR(number(h.signals[i], "mean") / scale, number(reference.signals[i], "mean"),
                        1e-9);
            EXPECT_NEAR(number(h.signals[i], "standard_deviation") / scale,
                        number(reference.signals[i], "standard_deviation"), 1e-9);
        }
    }
}

TEST(MeanReversionNumerics, TheLargestDoublesNeitherOverflowTheSumNorTheSquares) {
    // [M M M M/2]: mean 0.875 M, deviations 0.125 M (x3) and -0.375 M, variance
    // 0.046875 M^2 = 3/64 M^2, z = -0.375 / sqrt(3/64) = -sqrt(3). Both the squares
    // (M^2) and a four-term sum of M overflow a double; the answer must not.
    Harness h{make_config()};
    h.feed_closes({kMax, kMax, kMax, kMax / 2});
    EXPECT_EQ(h.labels(), (Labels{"buy@4"}));
    ASSERT_EQ(h.signals.size(), 1u);
    expect_finite_metadata(h.signals[0]);
    EXPECT_NEAR(number(h.signals[0], "z_score"), -std::sqrt(3.0), 1e-9);
    EXPECT_NEAR(number(h.signals[0], "mean") / kMax, 0.875, 1e-12);
    EXPECT_NEAR(number(h.signals[0], "standard_deviation") / kMax, std::sqrt(3.0 / 64.0), 1e-12);

    // The sum of fifty M overflows by itself. Forty-nine M and one M/2 give
    // z = -sqrt(49) = -7: a lone close is at most sqrt(N-1) deviations away.
    Harness wide{make_config(50, 1.5, 0.5)};
    Closes closes(49, kMax);
    closes.push_back(kMax / 2);
    wide.feed_closes(closes);
    EXPECT_EQ(wide.labels(), (Labels{"buy@50"}));
    ASSERT_EQ(wide.signals.size(), 1u);
    EXPECT_NEAR(number(wide.signals[0], "z_score"), -7.0, 1e-9);

    // A constant window of the largest double is just constant.
    Harness flat{make_config()};
    flat.feed_closes(Closes(8, kMax));
    EXPECT_TRUE(flat.signals.empty());
}

TEST(MeanReversionNumerics, ClosesOfWildlyDifferentMagnitudeAreMeasuredAgainstTheLargest) {
    // [1e300 1e300 1e300 1e-300]: next to 1e300 the last close is zero, so it is
    // 0.75e300 below the mean 0.75e300 with deviation 0.433e300: z = -sqrt(3), a
    // Buy. The mirror image, one 1e300 after three 1e-300, is z = +sqrt(3), a Sell.
    Harness down{make_config()};
    down.feed_closes({1e300, 1e300, 1e300, 1e-300});
    EXPECT_EQ(down.labels(), (Labels{"buy@4"}));
    expect_finite_metadata(down.signals.at(0));
    EXPECT_NEAR(number(down.signals[0], "z_score"), -std::sqrt(3.0), 1e-9);

    Harness up{make_config()};
    up.feed_closes({1e-300, 1e-300, 1e-300, 1e300});
    EXPECT_EQ(up.labels(), (Labels{"sell@4"}));
    expect_finite_metadata(up.signals.at(0));
    EXPECT_NEAR(number(up.signals[0], "z_score"), std::sqrt(3.0), 1e-9);
}

TEST(MeanReversionNumerics, SubnormalPricesWorkLikeAnyOther) {
    // The fixture in whole multiples of the smallest positive double: exactly
    // representable, so the statistics are exactly the fixture's.
    const double unit = std::numeric_limits<double>::denorm_min();
    Closes closes;
    for (const double close : kFixture) {
        closes.push_back(close * unit);
    }
    Harness h{make_config()};
    h.feed_closes(closes);
    EXPECT_EQ(h.labels(), kFixtureLabels);
    for (const domain::TradeSignal& signal : h.signals) {
        expect_finite_metadata(signal);
    }
    EXPECT_NEAR(number(h.signals[0], "z_score"), -std::sqrt(3.0), 1e-9);
}

TEST(MeanReversionNumerics, ASmallSpreadOnALargeLevelIsNotLostToCancellation) {
    // [1e9 1e9 1e9 1e9+1]: mean 1e9 + 0.25, variance (3 x 0.0625 + 0.5625) / 4 =
    // 0.1875, deviation 0.4330127, z = 0.75 / 0.4330127 = +sqrt(3). E[x^2] - E[x]^2
    // would subtract two numbers near 1e18 whose difference is 0.19, below the
    // rounding step (128) of either, and return noise.
    Harness h{make_config()};
    h.feed_closes({1e9, 1e9, 1e9, 1e9 + 1.0});
    EXPECT_EQ(h.labels(), (Labels{"sell@4"}));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_NEAR(number(h.signals[0], "standard_deviation"), std::sqrt(0.1875), 1e-9);
    EXPECT_NEAR(number(h.signals[0], "z_score"), std::sqrt(3.0), 1e-9);
    EXPECT_NEAR(number(h.signals[0], "mean"), 1e9 + 0.25, 1e-6);
}

TEST(MeanReversionNumerics, AVariationBelowOnePartInATrillionIsNegligible) {
    // The same step of 1 on 1e13 is a relative 1e-13 spread (deviation 0.433 against
    // a mean of 1e13), below the documented 1e-12 tolerance: constant, no signal.
    // On 1e9 (above) it is a real deviation.
    Harness h{make_config()};
    h.feed_closes({1e13, 1e13, 1e13, 1e13 + 1.0});
    EXPECT_TRUE(h.signals.empty());
}

TEST(MeanReversionNumerics, ExtremeWindowsLeaveNothingBehindOnceTheyHaveScrolledOut) {
    // Four bars at the largest double (constant), then four tens, then the fixture.
    // Windows containing both scales are measured against the largest (bar 5, [M M M
    // 10], is a legitimate z = -sqrt(3) Buy; bars 6 and 7 hold). By bar 8 the window
    // is four tens: constant, rearm. From bar 12 the extreme closes are long gone and
    // the fixture must behave exactly as it does on a fresh strategy: Buy at its bar
    // 4 (overall 12), the rearm, Buy at its bar 6 (14) and Sell at its bar 7 (15).
    Closes closes(4, kMax);
    closes.insert(closes.end(), 4, 10.0);
    closes.insert(closes.end(), kFixture.begin(), kFixture.end());

    Harness h{make_config()};
    h.feed_closes(closes);
    EXPECT_EQ(h.labels(), (Labels{"buy@5", "buy@12", "buy@14", "sell@15"}));

    Harness clean{make_config()};
    clean.feed_closes(kFixture);
    ASSERT_EQ(h.signals.size(), 4u);
    ASSERT_EQ(clean.signals.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        SCOPED_TRACE("fixture signal " + std::to_string(i));
        expect_finite_metadata(h.signals[i + 1]);
        for (const char* key : {"mean", "standard_deviation", "z_score"}) {
            EXPECT_NEAR(number(h.signals[i + 1], key), number(clean.signals[i], key), 1e-12) << key;
        }
    }
}

// --- Lifecycle ----------------------------------------------------------------------

TEST(MeanReversionLifecycle, OnStartForgetsTheLastRunSoItsBarsAreFreshAgain) {
    Harness h{make_config()};
    h.feed_closes(kFixture);
    ASSERT_EQ(h.labels(), kFixtureLabels);

    // The same minutes again: ignored as duplicates unless the last timestamp was
    // cleared, and the signals only repeat if history and latch were cleared too.
    h.mr.on_start();
    h.feed_closes(kFixture);
    EXPECT_EQ(h.labels(), (Labels{"buy@4", "buy@6", "sell@7", "buy@4", "buy@6", "sell@7"}));
}

TEST(MeanReversionLifecycle, OnStartForgetsTheLatch) {
    // The run ends LowerExtreme after a Buy at bar 4. If that survived, the same
    // extreme window in the second run would be a suppressed repeat.
    Harness h{make_config()};
    h.feed_closes({10, 10, 10, 6});
    h.mr.on_start();
    h.feed_closes({10, 10, 10, 6});
    EXPECT_EQ(h.labels(), (Labels{"buy@4", "buy@4"}));
}

TEST(MeanReversionLifecycle, OnStartForgetsAHalfWarmHistory) {
    // Two 100s sit in the history when the run restarts. If they stayed, the
    // fixture's bars would be measured with them and the signals would move.
    Harness h{make_config()};
    h.feed_closes({100, 100});
    h.mr.on_start();
    h.feed_closes(kFixture);
    EXPECT_EQ(h.labels(), kFixtureLabels);
}

// --- Against an exact reference -----------------------------------------------------

namespace {

enum class RefLatch { Neutral, Lower, Upper };

// An independent model for whole-number closes. With S the sum and Q the sum of
// squares over a window of N closes:
//     z = (N*close - S) / sqrt(N*Q - S*S)
// (population deviation, current close included), so z^2 versus t^2 for a
// threshold t = p/100 is the integer comparison A^2 * 100^2 versus p^2 * B with
// A = N*close - S and B = N*Q - S*S. No rounding is involved. B = 0 is a constant
// window. A tie (equality) is reported so the test can pick another seed instead of
// asserting on the one case where float and exact arithmetic may differ by an ulp.
struct Reference {
    Labels labels;
    bool   tie{false};
};

Reference reference_labels(const std::vector<long long>& closes, std::size_t n, long long entry_p,
                           long long rearm_p) {
    Reference out;
    RefLatch latch = RefLatch::Neutral;
    for (std::size_t bar_number = n; bar_number <= closes.size(); ++bar_number) {
        long long s = 0;
        long long q = 0;
        for (std::size_t i = bar_number - n; i < bar_number; ++i) {
            s += closes[i];
            q += closes[i] * closes[i];
        }
        const auto count = static_cast<long long>(n);
        const long long a = count * closes[bar_number - 1] - s;
        const long long b = count * q - s * s;
        if (b == 0) {
            latch = RefLatch::Neutral;
            continue;
        }
        const long long lhs = a * a * 100 * 100;
        if (lhs == entry_p * entry_p * b || lhs == rearm_p * rearm_p * b) {
            out.tie = true;
        }
        const bool beyond = lhs > entry_p * entry_p * b || lhs == entry_p * entry_p * b;
        const bool inside = lhs < rearm_p * rearm_p * b || lhs == rearm_p * rearm_p * b;
        if (beyond && a < 0 && latch != RefLatch::Lower) {
            out.labels.push_back("buy@" + std::to_string(bar_number));
            latch = RefLatch::Lower;
        } else if (beyond && a > 0 && latch != RefLatch::Upper) {
            out.labels.push_back("sell@" + std::to_string(bar_number));
            latch = RefLatch::Upper;
        } else if (!beyond && inside) {
            latch = RefLatch::Neutral;
        }
    }
    return out;
}

// A series in whole currency units: a random walk clamped to [90, 110], with flat
// stretches so constant windows occur at every lookback. std::mt19937's output is
// fixed by the standard, so every platform builds the same series.
std::vector<long long> series(std::uint32_t seed) {
    std::mt19937 rng{seed};
    std::vector<long long> out;
    long long price = 100;
    for (int block = 0; block < 40; ++block) {
        for (int i = 0; i < 100; ++i) {
            price += static_cast<long long>(rng() % 9) - 4;
            price = std::clamp(price, 90LL, 110LL);
            out.push_back(price);
        }
        out.insert(out.end(), 25, price);   // a flat stretch
        // A spike to stretch the deviation, then back.
        out.push_back(price + 25);
        out.push_back(price);
    }
    return out;
}

}  // namespace

TEST(MeanReversionReference, AgreesWithAnExactIntegerReferenceOnLongRandomRuns) {
    struct Setup { std::size_t lookback; double entry; long long entry_p; double rearm; long long rearm_p; };
    const Setup setups[] = {
        {3, 1.37, 137, 0.43, 43},   {5, 1.37, 137, 0.43, 43},   {8, 2.01, 201, 0.57, 57},
        {20, 2.01, 201, 0.57, 57},  {20, 1.13, 113, 0.07, 7},   {40, 2.59, 259, 1.01, 101},
    };
    for (const std::uint32_t seed : {20261001U, 77U}) {
        const std::vector<long long> closes = series(seed);
        for (const Setup& setup : setups) {
            SCOPED_TRACE("seed " + std::to_string(seed) + ", lookback " +
                         std::to_string(setup.lookback) + ", entry " + std::to_string(setup.entry) +
                         ", rearm " + std::to_string(setup.rearm));
            const Reference expected =
                reference_labels(closes, setup.lookback, setup.entry_p, setup.rearm_p);
            ASSERT_FALSE(expected.tie) << "an exact tie: pick other thresholds or another seed";
            EXPECT_GE(expected.labels.size(), 10u) << "a reference with no signals proves nothing";

            Harness h{make_config(setup.lookback, setup.entry, setup.rearm)};
            for (std::size_t i = 0; i < closes.size(); ++i) {
                h.feed(bar("AAPL", static_cast<double>(closes[i]), static_cast<long long>(i) + 1));
            }
            EXPECT_EQ(h.labels(), expected.labels);
        }
    }
}
