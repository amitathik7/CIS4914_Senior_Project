// MovingAverageCrossoverStrategy under awkward input and over long runs: events
// it must ignore without a trace, how on_start() resets a run, and a long random
// run checked against an exact integer reference. The behaviour on well-formed
// bars is in moving_average_crossover_strategy_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "moving_average_crossover_fixture.hpp"
#include "trading_engine/common/errors.hpp"

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
        domain::MarketEvent event = bar("AAPL", 5_px, 1000);
        event.type                = type;
        events.push_back(event);
    }
    events.push_back(bar("TSLA", 5_px, 1000));   // not on the allowlist
    events.push_back(bar("aapl", 5_px, 1000));   // not an exact match
    domain::MarketEvent missing = bar("AAPL", 5_px, 1000);
    missing.price.reset();
    events.push_back(missing);
    // zero and negatives are invalid; the int64 maximum is too big to sum safely (above max_close)
    for (const common::Price bad : {common::Price{}, -5_px, -kMaxPrice, trading_engine::test_support::kMinPrice,
                                    kMaxPrice}) {
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
    for (const common::Price close : kFixture) {
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
        noisy.feed(bar("AAPL", 1000_px, minute));       // the same time, another price
        noisy.feed(bar("AAPL", 1000_px, minute - 1));   // not later than the last accepted bar
        noisy.feed(bar("AAPL", 1000_px, 0));            // older than every accepted bar
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
    // The largest int64 would overflow a rolling sum, so it is ignored like any
    // invalid bar: it must not take the minute-1000 timestamp, which would drop
    // every bar after it. A close of 4e11 currency units is absurd for a price but
    // inside the limit, so the sums stay exact and the relationships do not depend on
    // the scale.
    Harness h{make_config(2, 3)};
    h.feed(bar("AAPL", kMaxPrice, 1000));
    long long minute = 0;
    for (const common::Price close : kFixture) {
        h.feed(bar("AAPL", close * 100'000'000'000LL, ++minute));   // up to 4e17 micros
    }
    EXPECT_EQ(h.labels(), (Labels{"buy@5", "sell@8"}));
    for (const domain::TradeSignal& signal : h.signals) {
        EXPECT_TRUE(std::isfinite(number(signal, "short_sma")));
        EXPECT_TRUE(std::isfinite(number(signal, "long_sma")));
    }
}

TEST(SmaCrossoverIgnoredEvents, TheLargestAcceptedCloseIsINT64MaxOverLongWindowSquared) {
    // For a 2/3 pair max_close is INT64_MAX / 3 / 3. A close of exactly that is used,
    // one micro more is ignored (and, being ignored, takes no timestamp).
    const common::Price max_close = micros(std::numeric_limits<std::int64_t>::max() / 3 / 3);

    Harness h{make_config(2, 3)};
    h.feed(bar("AAPL", max_close + micros(1), 1000));   // ignored
    // 1, 1, M, 1, 1 (the 1s are currency units): bar 3 short (1+M)/2 and long (2+M)/3 -> above, the
    // baseline; bar 4 short (M+1)/2 above long (M+2)/3; bar 5 short 1 below long (M+2)/3:
    // Sell. Every sum and product on the way is within int64.
    h.feed(bar("AAPL", 1_px, 1));
    h.feed(bar("AAPL", 1_px, 2));
    h.feed(bar("AAPL", max_close, 3));
    h.feed(bar("AAPL", 1_px, 4));
    h.feed(bar("AAPL", 1_px, 5));
    EXPECT_EQ(h.labels(), (Labels{"sell@5"}));
}

namespace {

bool constructs(std::size_t short_window, std::size_t long_window) {
    try {
        const strategy::MovingAverageCrossoverStrategy accepted{make_config(short_window, long_window)};
        (void)accepted;
        return true;
    } catch (const common::ConfigError&) {
        return false;
    }
}

}  // namespace

TEST(SmaCrossoverConfig, ALongWindowSoLargeThatNoCloseCouldBeSummedIsRefusedWhateverItsMagnitude) {
    // max_close = INT64_MAX / long_window^2 must be at least one currency unit (10^6 millionths):
    // 3'037'000 gives exactly 1'000'000 and is accepted, 3'037'001 gives 999'999 and is refused.
    EXPECT_TRUE(constructs(2, 3'037'000));
    EXPECT_FALSE(constructs(2, 3'037'001));
    EXPECT_FALSE(constructs(2, 4'000'000'000ULL));
    // Windows of 2^63 and above are NEGATIVE as an int64. The bound is computed in unsigned
    // arithmetic, so none of them wraps into a huge accepted bound (long_window = SIZE_MAX used to
    // cast to -1 and give max_close = INT64_MAX, letting the warm-up sums overflow).
    for (const std::size_t huge : {std::size_t{1} << 32, std::size_t{1} << 62, std::size_t{1} << 63,
                                   (std::size_t{1} << 63) + 1, std::numeric_limits<std::size_t>::max() - 1,
                                   std::numeric_limits<std::size_t>::max()}) {
        EXPECT_FALSE(constructs(2, huge)) << huge;
    }
}

// --- Precision ----------------------------------------------------------------------

TEST(SmaCrossoverPrecision, AHugeCloseLeavingTheWindowDoesNotTakeTheSmallOnesWithIt) {
    // 2/3 pair on 10^18 micros, then 1, 1, 1, 2, 3 MICROS (a millionth of a currency unit; bare
    // numbers here are micros on purpose). A double near 10^18 is 128 apart, so a floating-point
    // running total would drop every added 1 and, once the huge close had left the window,
    // read 0 where the true sums are 2 and 3. int64 sums are exact: bar 3 is short 1 below long
    // (10^18+2)/3, the baseline; bar 4 is 1 against 1, equal; bar 5 is short (1+2)/2 = 1.5 micros
    // above long (1+1+2)/3 = 4/3 micros: Buy. The averages are checked, not just the label.
    Harness h{make_config(2, 3)};
    h.feed_closes({micros(1'000'000'000'000'000'000LL), micros(1), micros(1), micros(1), micros(2), micros(3)});
    EXPECT_EQ(h.labels(), (Labels{"buy@5"}));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_EQ(number(h.signals[0], "short_sma"), 1.5e-6);
    EXPECT_EQ(number(h.signals[0], "long_sma"), 4.0 / 3.0e6);
}

// --- The overflow bound, at its limit -----------------------------------------------

namespace {

// An exact 128-bit product: an independent reference for the strategy's int64 cross-multiplication,
// which must never overflow (MSVC has no __int128, so it is built from 32-bit limbs).
struct U128 {
    std::uint64_t hi;
    std::uint64_t lo;
};

U128 multiply(std::uint64_t a, std::uint64_t b) {
    constexpr std::uint64_t kLow = 0xFFFFFFFFULL;
    const std::uint64_t p0 = (a & kLow) * (b & kLow);
    const std::uint64_t p1 = (a & kLow) * (b >> 32);
    const std::uint64_t p2 = (a >> 32) * (b & kLow);
    const std::uint64_t p3 = (a >> 32) * (b >> 32);
    const std::uint64_t middle = (p0 >> 32) + (p1 & kLow) + (p2 & kLow);
    return {p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32), (p0 & kLow) | (middle << 32)};
}

int compare(U128 x, U128 y) {
    if (x.hi != y.hi) {
        return x.hi < y.hi ? -1 : 1;
    }
    if (x.lo != y.lo) {
        return x.lo < y.lo ? -1 : 1;
    }
    return 0;
}

// The labels the DEFINITION gives, with brute-force sums per bar and the comparison
// short_sum * L  versus  long_sum * S  done exactly in 128 bits.
Labels exact_labels(const std::vector<std::uint64_t>& closes, std::size_t s, std::size_t l) {
    std::vector<std::pair<std::size_t, int>> nonzero;   // (1-based bar, sign)
    for (std::size_t bar_number = l; bar_number <= closes.size(); ++bar_number) {
        std::uint64_t short_sum = 0;
        std::uint64_t long_sum  = 0;
        for (std::size_t i = bar_number - s; i < bar_number; ++i) {
            short_sum += closes[i];
        }
        for (std::size_t i = bar_number - l; i < bar_number; ++i) {
            long_sum += closes[i];
        }
        const int sign = compare(multiply(short_sum, l), multiply(long_sum, s));
        if (sign != 0) {
            nonzero.emplace_back(bar_number, sign);
        }
    }
    Labels out;
    for (std::size_t i = 1; i < nonzero.size(); ++i) {
        if (nonzero[i].second != nonzero[i - 1].second) {
            out.push_back((nonzero[i].second > 0 ? "buy@" : "sell@") + std::to_string(nonzero[i].first));
        }
    }
    return out;
}

}  // namespace

TEST(SmaCrossoverOverflowBound, TheDecisionStaysExactWhenEveryCloseIsNearTheLargestAcceptedOne) {
    // For each window pair the closes mix max_close = INT64_MAX / L^2 (the largest accepted), values one
    // below it, half of it, and one currency unit, in plateaus and in random runs. The sums then sit at
    // the bound and short_sum * L, long_sum * S are within a few percent of INT64_MAX. If either could
    // wrap, the sign (so a signal) would flip against the 128-bit reference.
    const std::vector<std::pair<std::size_t, std::size_t>> windows{{1, 2}, {2, 3}, {5, 20}, {19, 20}, {7, 30}, {3, 1000}};
    for (const auto& [short_window, long_window] : windows) {
        SCOPED_TRACE(std::to_string(short_window) + "/" + std::to_string(long_window));
        const std::uint64_t largest = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                                      long_window / long_window;
        const std::uint64_t one_unit = 1'000'000;
        ASSERT_GE(largest, one_unit);

        std::mt19937 rng{static_cast<std::uint32_t>(20261003U + long_window)};
        std::vector<std::uint64_t> closes;
        while (closes.size() < 5000) {
            const std::size_t length = 1 + rng() % 40;
            const std::size_t mode   = rng() % 6;
            const std::uint64_t flat = mode == 0 ? largest : mode == 1 ? largest - 1 : mode == 2 ? largest / 2 : one_unit;
            for (std::size_t i = 0; i < length; ++i) {
                if (mode <= 3) {
                    closes.push_back(flat);   // a plateau (exact ties are common)
                } else if (mode == 4) {
                    closes.push_back(largest - rng() % 100);   // hugging the bound
                } else {
                    closes.push_back(one_unit + (static_cast<std::uint64_t>(rng()) << 32 | rng()) % (largest - one_unit + 1));
                }
            }
        }
        closes.resize(5000);

        const Labels expected = exact_labels(closes, short_window, long_window);
        ASSERT_GE(expected.size(), 10u) << "a reference with no signals proves nothing";

        Harness h{make_config(short_window, long_window)};
        for (std::size_t i = 0; i < closes.size(); ++i) {
            h.feed(bar("AAPL", micros(static_cast<std::int64_t>(closes[i])), static_cast<long long>(i) + 1));
        }
        EXPECT_EQ(h.labels(), expected);
    }
}

TEST(SmaCrossoverOverflowBound, TheWorstCaseProductsFitInInt64AndAnyLargerCloseIsIgnored) {
    // The largest possible products for a window pair (S, L): short_sum = S * M and long_sum = L * M
    // with M = max_close, so short_sum * L = L * S * M and long_sum * S = the same, both at most
    // INT64_MAX * S / L < INT64_MAX. Runs of M (exact ties, the extreme sums) around single dips to one
    // currency unit must give the 128-bit reference's labels, and a close of M + 1 millionth, which is
    // above the bound, must be ignored (it must take neither a sum nor the clock). The dips make the
    // relation change, so the labels are not empty.
    for (const auto& [short_window, long_window] : std::vector<std::pair<std::size_t, std::size_t>>{{1, 2}, {19, 20}, {1999, 2000}}) {
        SCOPED_TRACE(std::to_string(short_window) + "/" + std::to_string(long_window));
        const std::uint64_t largest = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                                      long_window / long_window;
        std::vector<std::uint64_t> closes(3 * long_window, largest);
        closes.push_back(1'000'000);
        closes.insert(closes.end(), 2 * long_window, largest);
        closes.push_back(1'000'000);
        closes.insert(closes.end(), 2 * long_window, largest);

        const Labels expected = exact_labels(closes, short_window, long_window);
        ASSERT_FALSE(expected.empty());

        Harness h{make_config(short_window, long_window)};
        for (std::size_t i = 0; i < closes.size(); ++i) {
            // First an out-of-range close at the SAME minute as the real bar: if it were accepted it would
            // add its (overflowing) sum and take the minute, and the real bar after it would be dropped
            // as a duplicate, so the labels would diverge from the reference.
            h.feed(bar("AAPL", micros(static_cast<std::int64_t>(largest) + 1), static_cast<long long>(i) + 1));
            h.feed(bar("AAPL", micros(static_cast<std::int64_t>(closes[i])), static_cast<long long>(i) + 1));
        }
        EXPECT_EQ(h.labels(), expected);
    }
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
    h.feed_closes(units({1, 2, 3}));
    h.sma.on_start();
    h.feed_closes(units({3, 2, 1}));
    EXPECT_TRUE(h.signals.empty());
}

TEST(SmaCrossoverLifecycle, OnStartForgetsAHalfWarmHistory) {
    // Two 100s are in a 2/3 pair's history when the run restarts. If they stayed,
    // the fixture's bars would be averaged with them and the signals would move.
    Harness h{make_config(2, 3)};
    h.feed_closes(units({100, 100}));
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
    // at $100.10 bring the averages back to an exact tie again and again, which is
    // where an inexact comparison would show; and the 100-cent steps cover wide moves.
    // The prices handed to the strategy are the cents scaled to Price micros
    // (cents * 10'000), exact integers, so the strategy and this reference see the
    // same numbers.
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
            h.feed(bar("AAPL", micros(cents[i] * 10'000), static_cast<long long>(i) + 1));   // cents -> millionths
        }
        EXPECT_EQ(h.labels(), expected);
    }
}
