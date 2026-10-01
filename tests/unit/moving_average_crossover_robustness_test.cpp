// MovingAverageCrossoverStrategy under awkward input and over long runs: events
// it must ignore without a trace, how on_start() resets a run, and a long random
// run checked against an exact integer reference. The behaviour on well-formed
// bars is in moving_average_crossover_strategy_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "moving_average_crossover_fixture.hpp"

namespace {

using namespace sma_test;
using domain::MarketEventType;

// Every kind of event that must be ignored, all stamped far in the future
// (minute 1000). If any of them advanced the timestamp, the history or the
// averages, the real bars that follow (minutes 1-9) would be dropped or wrong.
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
    for (const double bad : {kNaN, kInf, -kInf, 0.0, -5.0, kMax}) {   // kMax: too big to sum safely
        events.push_back(bar("AAPL", bad, 1000));
    }
    return events;
}

// The clean run's signals are the yardstick: noise must leave both the labels
// and every metadata value (the averages) exactly as they were.
void expect_same_signals_as_a_clean_run(const Harness& noisy) {
    Harness clean{make_config(2, 3)};
    clean.feed_closes(kFixture);
    ASSERT_EQ(clean.labels(), (Labels{"buy@5", "sell@8"}));

    EXPECT_EQ(noisy.labels(), clean.labels());
    ASSERT_EQ(noisy.signals.size(), clean.signals.size());
    for (std::size_t i = 0; i < clean.signals.size(); ++i) {
        EXPECT_EQ(noisy.signals[i].metadata, clean.signals[i].metadata) << "signal " << i;
    }
}

}  // namespace

// --- Ignored events ----------------------------------------------------------------

TEST(SmaCrossoverIgnoredEvents, ChangeNothing) {
    Harness noisy{make_config(2, 3)};
    long long minute = 0;
    for (const double close : kFixture) {
        for (const domain::MarketEvent& ignored : ignorable_events()) {
            noisy.feed(ignored);
        }
        noisy.feed(bar("AAPL", close, ++minute));
    }
    expect_same_signals_as_a_clean_run(noisy);
}

TEST(SmaCrossoverIgnoredEvents, DuplicateAndOlderBarsChangeNothing) {
    Harness noisy{make_config(2, 3)};
    for (std::size_t i = 0; i < kFixture.size(); ++i) {
        const auto minute = static_cast<long long>(i) + 1;
        noisy.feed(bar("AAPL", kFixture[i], minute));
        noisy.feed(bar("AAPL", kFixture[i], minute));   // an exact re-delivery
        noisy.feed(bar("AAPL", 1000.0, minute));        // the same time, another price
        noisy.feed(bar("AAPL", 1000.0, minute - 1));    // not later than the last accepted bar
        noisy.feed(bar("AAPL", 1000.0, 0));             // older than every accepted bar
    }
    expect_same_signals_as_a_clean_run(noisy);
}

TEST(SmaCrossoverIgnoredEvents, GapsInTimeAreNeverFilledIn) {
    // The windows count accepted bars: the fixture's bar 5 and bar 8 still
    // trigger when the bars are hours apart, at the minutes they actually carry.
    Harness h{make_config(2, 3)};
    const std::vector<long long> minutes{1, 2, 3, 10, 20, 21, 50, 100, 101};
    for (std::size_t i = 0; i < kFixture.size(); ++i) {
        h.feed(bar("AAPL", kFixture[i], minutes[i]));
    }
    EXPECT_EQ(h.labels(), (Labels{"buy@20", "sell@100"}));
}

TEST(SmaCrossoverIgnoredEvents, ACloseTooLargeToSumSafelyIsIgnoredButHugeSafeOnesWork) {
    // The largest double would overflow a rolling sum, so it is ignored like any
    // invalid bar: it must not take the minute-1000 timestamp, which would drop
    // every bar after it. 1e300 is absurd for a price but inside the limit, so
    // the sums stay finite and the relationships do not depend on the scale.
    Harness h{make_config(2, 3)};
    h.feed(bar("AAPL", kMax, 1000));
    long long minute = 0;
    for (const double close : kFixture) {
        h.feed(bar("AAPL", close * 1e300, ++minute));
    }
    EXPECT_EQ(h.labels(), (Labels{"buy@5", "sell@8"}));
    for (const domain::TradeSignal& signal : h.signals) {
        EXPECT_TRUE(std::isfinite(number(signal, "short_sma")));
        EXPECT_TRUE(std::isfinite(number(signal, "long_sma")));
    }
}

// --- Precision ----------------------------------------------------------------------

TEST(SmaCrossoverPrecision, AHugeCloseLeavingTheWindowDoesNotTakeTheSmallOnesWithIt) {
    // 2/3 pair on 1e16, 1, 1, 1, 2, 3. Doubles near 1e16 are 2 apart, so a plain
    // running total drops an added 1, and once the 1e16 has left the window the
    // totals read 0 where the true sums are 2 and 3. Exactly: bar 3 is
    // short 1 below long (1e16+2)/3, the baseline; bar 4 is 1 against 1, equal;
    // bar 5 is short (1+2)/2 = 1.5 above long (1+1+2)/3 = 4/3: Buy. A plain total
    // would report 0.5 and 0.33 there, so the averages are checked, not just the label.
    Harness h{make_config(2, 3)};
    h.feed_closes({1e16, 1, 1, 1, 2, 3});
    EXPECT_EQ(h.labels(), (Labels{"buy@5"}));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_EQ(number(h.signals[0], "short_sma"), 1.5);
    EXPECT_EQ(number(h.signals[0], "long_sma"), 4.0 / 3.0);
}

// --- Lifecycle ----------------------------------------------------------------------

TEST(SmaCrossoverLifecycle, OnStartForgetsTheLastRunSoItsBarsAreFreshAgain) {
    Harness h{make_config(2, 3)};
    h.feed_closes(kFixture);
    ASSERT_EQ(h.labels(), (Labels{"buy@5", "sell@8"}));

    // The same minutes again: ignored as duplicates unless the last timestamp was
    // cleared, and the signals only repeat if history and baseline were cleared too.
    h.sma.on_start();
    h.feed_closes(kFixture);
    EXPECT_EQ(h.labels(), (Labels{"buy@5", "sell@8", "buy@5", "sell@8"}));
}

TEST(SmaCrossoverLifecycle, OnStartForgetsTheBaseline) {
    // The first run ends above (1, 2, 3: baseline above). The second starts
    // falling and its first comparison is below. If the old baseline survived, that
    // would be an above -> below crossing and a Sell; after a reset it is a baseline.
    Harness h{make_config(2, 3)};
    h.feed_closes({1, 2, 3});
    h.sma.on_start();
    h.feed_closes({3, 2, 1});
    EXPECT_TRUE(h.signals.empty());
}

TEST(SmaCrossoverLifecycle, OnStartForgetsAHalfWarmHistory) {
    // Two 100s are in a 2/3 pair's history when the run restarts. If they stayed,
    // the fixture's bars would be averaged with them and the signals would move.
    Harness h{make_config(2, 3)};
    h.feed_closes({100, 100});
    h.sma.on_start();
    h.feed_closes(kFixture);
    EXPECT_EQ(h.labels(), (Labels{"buy@5", "sell@8"}));
}

// --- Against an exact reference -----------------------------------------------------

namespace {

// An exact reference for closes given in whole cents. short_sum/S versus
// long_sum/L is the integer comparison L*short_sum versus S*long_sum, so no
// rounding is involved. It works in two passes with brute-force sums: first the
// relationship of every warmed-up bar, then, with the zeros dropped, every change
// of sign between neighbours is a crossover at the bar where the new sign appears.
Labels reference_labels(const std::vector<long long>& cents, std::size_t s, std::size_t l) {
    std::vector<std::pair<std::size_t, int>> nonzero;   // (1-based bar, sign)
    for (std::size_t bar_number = l; bar_number <= cents.size(); ++bar_number) {
        long long short_sum = 0;
        long long long_sum  = 0;
        for (std::size_t i = bar_number - s; i < bar_number; ++i) {
            short_sum += cents[i];
        }
        for (std::size_t i = bar_number - l; i < bar_number; ++i) {
            long_sum += cents[i];
        }
        const long long difference =
            static_cast<long long>(l) * short_sum - static_cast<long long>(s) * long_sum;
        if (difference != 0) {
            nonzero.emplace_back(bar_number, difference > 0 ? 1 : -1);
        }
    }
    Labels out;
    for (std::size_t i = 1; i < nonzero.size(); ++i) {
        if (nonzero[i].second != nonzero[i - 1].second) {
            out.push_back((nonzero[i].second > 0 ? "buy@" : "sell@") +
                          std::to_string(nonzero[i].first));
        }
    }
    return out;
}

// A walk in whole cents, never below 50. std::mt19937's output is fixed by the
// standard, so every platform builds the same series (a distribution would not).
std::vector<long long> walk(std::mt19937& rng, std::size_t bars, long long max_step,
                            long long start) {
    std::vector<long long> out;
    long long price = start;
    const auto span = static_cast<std::uint32_t>(2 * max_step + 1);
    for (std::size_t i = 0; i < bars; ++i) {
        price += static_cast<long long>(rng() % span) - max_step;
        price = std::max(price, 50LL);
        out.push_back(price);
    }
    return out;
}

}  // namespace

TEST(SmaCrossoverReference, AgreesWithAnExactIntegerReferenceOnLongRandomWalks) {
    // One-cent steps make exact ties (plateaus) frequent; the held-flat stretches
    // at $100.10, a price with no exact binary form, bring the averages back to an
    // exact tie again and again, which is where rounding error would show; and the
    // 100-cent steps cover wide moves. The prices handed to the strategy are the
    // doubles cents/100, so they are inexact decimals, as real prices are.
    std::mt19937 rng{20260930U};
    std::vector<long long> cents;
    const auto append = [&cents](const std::vector<long long>& more) {
        cents.insert(cents.end(), more.begin(), more.end());
    };
    append(walk(rng, 3000, 1, 10000));
    append(std::vector<long long>(300, 10010));
    append(walk(rng, 3000, 1, 10010));
    append(std::vector<long long>(300, 10010));
    append(walk(rng, 6000, 100, 25000));

    const std::vector<std::pair<std::size_t, std::size_t>> windows{
        {1, 2}, {2, 3}, {3, 5}, {5, 20}, {19, 20}, {7, 30}, {1, 50}};
    for (const auto& [short_window, long_window] : windows) {
        SCOPED_TRACE(std::to_string(short_window) + "/" + std::to_string(long_window));
        const Labels expected = reference_labels(cents, short_window, long_window);
        EXPECT_GE(expected.size(), 10u) << "a reference with no signals proves nothing";

        Harness h{make_config(short_window, long_window)};
        for (std::size_t i = 0; i < cents.size(); ++i) {
            h.feed(bar("AAPL", static_cast<double>(cents[i]) / 100.0,
                       static_cast<long long>(i) + 1));
        }
        EXPECT_EQ(h.labels(), expected);
    }
}
