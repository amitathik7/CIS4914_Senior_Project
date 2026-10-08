// What the reference strategies REPORT through the diagnostics observer: the verdict,
// reason, numbers and state for every bar of the documented worked examples, every
// ignore reason, and agreement between a snapshot and the signal it accompanies.
//
// The expected values are hand calculations (the worked tables in
// docs/strategies/*.md, re-derived here from the closes), written as literals. Nothing
// is read back from the implementation under test. That the observer changes no
// decision, signal or counter is in strategy_diagnostics_invariance_test.cpp.

#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "strategy_diagnostics_fixture.hpp"

namespace {

using namespace diag_test;
using strategy::BarAction;
using strategy::BarReason;
using strategy::BarVerdict;

double parse_number(const std::string& text) {
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    EXPECT_EQ(result.ec, std::errc{}) << text;
    EXPECT_EQ(result.ptr, text.data() + text.size()) << text;
    return value;
}

// ----- Moving-average crossover, 2/3, closes 3 2 1 2 3 4 3 2 1 ------------------------

struct SmaBar {
    BarReason             reason;
    BarAction             action;
    std::size_t           fill;
    std::optional<double> short_sma;
    std::optional<double> long_sma;
    const char*           before;
    const char*           now;
    const char*           after;
};

// short = mean of the last 2 closes, long = mean of the last 3. Derived by hand.
const std::vector<SmaBar> kSmaExpected{
    {BarReason::WarmingUp, BarAction::None, 1, std::nullopt, std::nullopt, "none", "not_evaluated", "none"},
    {BarReason::WarmingUp, BarAction::None, 2, std::nullopt, std::nullopt, "none", "not_evaluated", "none"},
    // bar 3: (2+1)/2 = 1.5 vs (3+2+1)/3 = 2 -> below: the baseline, never signalled
    {BarReason::BaselineEstablished, BarAction::None, 3, 1.5, 2.0, "none", "short_below_long", "short_below_long"},
    // bar 4: (1+2)/2 = 1.5 vs (2+1+2)/3 = 5/3
    {BarReason::SameSide, BarAction::None, 3, 1.5, 5.0 / 3.0, "short_below_long", "short_below_long", "short_below_long"},
    // bar 5: (2+3)/2 = 2.5 vs (1+2+3)/3 = 2 -> crosses above
    {BarReason::CrossoverBuy, BarAction::Buy, 3, 2.5, 2.0, "short_below_long", "short_above_long", "short_above_long"},
    {BarReason::SameSide, BarAction::None, 3, 3.5, 3.0, "short_above_long", "short_above_long", "short_above_long"},
    {BarReason::SameSide, BarAction::None, 3, 3.5, 10.0 / 3.0, "short_above_long", "short_above_long", "short_above_long"},
    // bar 8: (3+2)/2 = 2.5 vs (4+3+2)/3 = 3 -> crosses below
    {BarReason::CrossoverSell, BarAction::Sell, 3, 2.5, 3.0, "short_above_long", "short_below_long", "short_below_long"},
    {BarReason::SameSide, BarAction::None, 3, 1.5, 2.0, "short_below_long", "short_below_long", "short_below_long"},
};

TEST(StrategyDiagnosticsSma, EveryBarOfTheWorkedExampleIsReportedWithItsNumbersAndState) {
    strategy::MovingAverageCrossoverStrategy sma{sma_config(2, 3)};
    Collector collector;
    Sink sink;
    sma.set_observer(&collector);

    const std::vector<common::Price> closes = units({3, 2, 1, 2, 3, 4, 3, 2, 1});
    long long minute = 0;
    for (const common::Price close : closes) {
        sma.on_market_event(event_at("AAPL", close, ++minute), sink);
    }

    ASSERT_EQ(collector.all.size(), kSmaExpected.size()) << "exactly one snapshot per event";
    for (std::size_t i = 0; i < kSmaExpected.size(); ++i) {
        const SmaBar& want = kSmaExpected[i];
        const Seen&   got  = collector.all[i];
        SCOPED_TRACE("bar " + std::to_string(i + 1));

        EXPECT_EQ(got.strategy_id, "sma_crossover");
        EXPECT_EQ(got.reason, want.reason) << got.reason_text();
        EXPECT_EQ(got.action, want.action);
        EXPECT_EQ(got.window_fill, std::optional<std::size_t>{want.fill});
        EXPECT_EQ(got.window_size, 3u);
        EXPECT_EQ(got.verdict, want.reason == BarReason::WarmingUp ? BarVerdict::WarmingUp
                                                                   : BarVerdict::Evaluated);
        ASSERT_EQ(got.indicators.size(), 3u);
        EXPECT_EQ(got.state("relation_before"), want.before);
        EXPECT_EQ(got.state("relation_now"), want.now);
        EXPECT_EQ(got.state("relation_after"), want.after);

        if (want.short_sma.has_value()) {
            ASSERT_TRUE(got.number("short_sma").has_value());
            ASSERT_TRUE(got.number("long_sma").has_value());
            ASSERT_TRUE(got.number("short_minus_long").has_value());
            EXPECT_DOUBLE_EQ(*got.number("short_sma"), *want.short_sma);
            EXPECT_DOUBLE_EQ(*got.number("long_sma"), *want.long_sma);
            EXPECT_DOUBLE_EQ(*got.number("short_minus_long"), *want.short_sma - *want.long_sma);
        } else {
            EXPECT_FALSE(got.number("short_sma").has_value()) << "unavailable, not zero";
            EXPECT_FALSE(got.number("long_sma").has_value());
            EXPECT_FALSE(got.number("short_minus_long").has_value());
        }
    }
    ASSERT_EQ(sink.signals.size(), 2u);
    EXPECT_EQ(sink.signals[0].side, domain::SignalSide::Buy);
    EXPECT_EQ(sink.signals[1].side, domain::SignalSide::Sell);
}

TEST(StrategyDiagnosticsSma, EqualAveragesAreReportedAsEqualAndNeverEstablishABaseline) {
    strategy::MovingAverageCrossoverStrategy sma{sma_config(2, 3)};
    Collector collector;
    Sink sink;
    sma.set_observer(&collector);

    long long minute = 0;
    for (const common::Price close : units({5, 5, 5, 5})) {
        sma.on_market_event(event_at("AAPL", close, ++minute), sink);
    }
    ASSERT_EQ(collector.all.size(), 4u);
    for (std::size_t i = 2; i < 4; ++i) {   // bars 3 and 4: the window is full
        EXPECT_EQ(collector.all[i].reason, BarReason::AveragesEqual);
        EXPECT_EQ(collector.all[i].verdict, BarVerdict::Evaluated);
        EXPECT_EQ(collector.all[i].state("relation_now"), "equal");
        EXPECT_EQ(collector.all[i].state("relation_before"), "none");
        EXPECT_EQ(collector.all[i].state("relation_after"), "none") << "no baseline from a tie";
        EXPECT_DOUBLE_EQ(*collector.all[i].number("short_minus_long"), 0.0);
    }
    EXPECT_TRUE(sink.signals.empty());
}

TEST(StrategyDiagnosticsSma, EveryIgnoreReasonIsReportedAndLeavesTheWindowAlone) {
    strategy::MovingAverageCrossoverStrategy sma{sma_config(2, 3)};
    Collector collector;
    Sink sink;
    sma.set_observer(&collector);

    // One accepted bar first, so "unchanged" has a window fill of 1 to be unchanged.
    sma.on_market_event(event_at("AAPL", 10_px, 100), sink);

    struct Case {
        domain::MarketEvent event;
        BarReason           reason;
        bool                has_window;
    };
    const std::vector<Case> cases{
        {event_at("AAPL", 10_px, 101, domain::MarketEventType::Trade), BarReason::NotABar, false},
        {event_at("MSFT", 10_px, 101), BarReason::SymbolNotAllowlisted, false},
        {event_at("aapl", 10_px, 101), BarReason::SymbolNotAllowlisted, false},
        {event_at("AAPL", std::nullopt, 101), BarReason::PriceAbsent, true},
        {event_at("AAPL", kMinPrice, 101), BarReason::PriceInvalid, true},
        {event_at("AAPL", -kMaxPrice, 101), BarReason::PriceInvalid, true},
        {event_at("AAPL", common::Price{}, 101), BarReason::PriceInvalid, true},
        {event_at("AAPL", -1_px, 101), BarReason::PriceInvalid, true},
        {event_at("AAPL", micros(-1), 101), BarReason::PriceInvalid, true},   // minus one millionth
        {event_at("AAPL", kMaxPrice, 101), BarReason::PriceAboveMaxClose, true},
        {event_at("AAPL", 11_px, 100), BarReason::TimeNotAfterLastAccepted, true},   // duplicate
        {event_at("AAPL", 11_px, 99), BarReason::TimeNotAfterLastAccepted, true},    // older
    };
    for (const Case& each : cases) {
        sma.on_market_event(each.event, sink);
    }

    ASSERT_EQ(collector.all.size(), cases.size() + 1);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Seen& got = collector.all[i + 1];
        SCOPED_TRACE("case " + std::to_string(i) + ": " + std::string{strategy::to_string(cases[i].reason)});
        EXPECT_EQ(got.reason, cases[i].reason) << got.reason_text();
        EXPECT_EQ(got.verdict, BarVerdict::Ignored);
        EXPECT_EQ(got.action, BarAction::None);
        ASSERT_EQ(got.indicators.size(), 3u);
        EXPECT_FALSE(got.number("short_sma").has_value());
        EXPECT_FALSE(got.number("long_sma").has_value());
        if (cases[i].has_window) {
            EXPECT_EQ(got.window_fill, std::optional<std::size_t>{1}) << "an ignored event adds nothing";
            EXPECT_EQ(got.state("relation_before"), "none");
            EXPECT_EQ(got.state("relation_after"), "none");
        } else {
            EXPECT_FALSE(got.window_fill.has_value()) << "never reached a symbol's state";
            EXPECT_TRUE(got.states.empty());
        }
    }
    EXPECT_TRUE(sink.signals.empty());
}

// ----- Mean reversion: lookback 4, entry 1.5, rearm 0.5 ----------------------------------

struct MrBar {
    BarReason             reason;
    BarAction             action;
    std::size_t           fill;
    std::optional<double> mean;
    std::optional<double> variance;   // population variance, by hand
    std::optional<double> deviation;  // close - mean, by hand
    const char*           before;
    const char*           after;
};

void expect_mr_bars(const std::vector<common::Price>& closes, const std::vector<MrBar>& expected) {
    strategy::MeanReversionStrategy mr{mr_config(4, 1.5, 0.5)};
    Collector collector;
    Sink sink;
    mr.set_observer(&collector);
    long long minute = 0;
    for (const common::Price close : closes) {
        mr.on_market_event(event_at("AAPL", close, ++minute), sink);
    }

    ASSERT_EQ(collector.all.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const MrBar& want = expected[i];
        const Seen&  got  = collector.all[i];
        SCOPED_TRACE("bar " + std::to_string(i + 1));
        EXPECT_EQ(got.strategy_id, "mean_reversion");
        EXPECT_EQ(got.reason, want.reason) << got.reason_text();
        EXPECT_EQ(got.action, want.action);
        EXPECT_EQ(got.window_fill, std::optional<std::size_t>{want.fill});
        EXPECT_EQ(got.window_size, 4u);
        EXPECT_EQ(got.state("latch_before"), want.before);
        EXPECT_EQ(got.state("latch_after"), want.after);
        ASSERT_EQ(got.indicators.size(), 3u);

        if (!want.mean.has_value()) {
            EXPECT_FALSE(got.number("mean").has_value());
            EXPECT_FALSE(got.number("standard_deviation").has_value());
            EXPECT_FALSE(got.number("z_score").has_value());
            continue;
        }
        const double stddev = std::sqrt(*want.variance);
        ASSERT_TRUE(got.number("mean").has_value());
        ASSERT_TRUE(got.number("standard_deviation").has_value());
        EXPECT_NEAR(*got.number("mean"), *want.mean, 1e-12);
        EXPECT_NEAR(*got.number("standard_deviation"), stddev, 1e-12);
        if (want.deviation.has_value()) {
            ASSERT_TRUE(got.number("z_score").has_value());
            EXPECT_NEAR(*got.number("z_score"), *want.deviation / stddev, 1e-12);
        } else {
            EXPECT_FALSE(got.number("z_score").has_value()) << "constant window: z is not meaningful";
        }
    }
}

TEST(StrategyDiagnosticsMeanReversion, EveryBarOfTheRearmWorkedExampleIsReported) {
    // Windows of the last 4 closes, mean and population variance by hand:
    //   bar 4: 10 10 10 6   mean 9      var 3        close-mean -3     -> z -1.732  Buy
    //   bar 5: 10 10 6 9    mean 8.75   var 2.6875   close-mean 0.25   -> z  0.152  rearm
    //   bar 6: 10 6 9 2     mean 6.75   var 9.6875   close-mean -4.75  -> z -1.526  Buy
    //   bar 7: 6 9 2 30     mean 11.75  var 117.1875 close-mean 18.25  -> z  1.686  Sell (flip)
    expect_mr_bars(units({10, 10, 10, 6, 9, 2, 30}),
        {
            {BarReason::WarmingUp, BarAction::None, 1, std::nullopt, std::nullopt, std::nullopt, "neutral", "neutral"},
            {BarReason::WarmingUp, BarAction::None, 2, std::nullopt, std::nullopt, std::nullopt, "neutral", "neutral"},
            {BarReason::WarmingUp, BarAction::None, 3, std::nullopt, std::nullopt, std::nullopt, "neutral", "neutral"},
            {BarReason::EntryBuy, BarAction::Buy, 4, 9.0, 3.0, -3.0, "neutral", "lower_extreme"},
            {BarReason::InsideRearmBand, BarAction::None, 4, 8.75, 2.6875, 0.25, "lower_extreme", "neutral"},
            {BarReason::EntryBuy, BarAction::Buy, 4, 6.75, 9.6875, -4.75, "neutral", "lower_extreme"},
            {BarReason::EntrySell, BarAction::Sell, 4, 11.75, 117.1875, 18.25, "lower_extreme", "upper_extreme"},
        });
}

TEST(StrategyDiagnosticsMeanReversion, SuppressionAndTheConstantWindowAreReportedForTheRepeatSeries) {
    // 10 10 10 6 2 1 1 1 1 10, windows of 4:
    //   bar 4  10 10 10 6   mean 9     var 3       dev -3     z -1.732 Buy
    //   bar 5  10 10 6 2    mean 7     var 11      dev -5     z -1.508 beyond entry, already requested
    //   bar 6  10 6 2 1     mean 4.75  var 12.6875 dev -3.75  z -1.053 between the bands
    //   bar 7  6 2 1 1      mean 2.5   var 4.25    dev -1.5   z -0.728 between
    //   bar 8  2 1 1 1      mean 1.25  var 0.1875  dev -0.25  z -0.577 between
    //   bar 9  1 1 1 1      constant: no z, the latch rearms
    //   bar 10 1 1 1 10     mean 3.25  var 15.1875 dev 6.75   z  1.732 Sell
    expect_mr_bars(units({10, 10, 10, 6, 2, 1, 1, 1, 1, 10}),
        {
            {BarReason::WarmingUp, BarAction::None, 1, std::nullopt, std::nullopt, std::nullopt, "neutral", "neutral"},
            {BarReason::WarmingUp, BarAction::None, 2, std::nullopt, std::nullopt, std::nullopt, "neutral", "neutral"},
            {BarReason::WarmingUp, BarAction::None, 3, std::nullopt, std::nullopt, std::nullopt, "neutral", "neutral"},
            {BarReason::EntryBuy, BarAction::Buy, 4, 9.0, 3.0, -3.0, "neutral", "lower_extreme"},
            {BarReason::ExcursionAlreadyRequested, BarAction::None, 4, 7.0, 11.0, -5.0, "lower_extreme", "lower_extreme"},
            {BarReason::BetweenBands, BarAction::None, 4, 4.75, 12.6875, -3.75, "lower_extreme", "lower_extreme"},
            {BarReason::BetweenBands, BarAction::None, 4, 2.5, 4.25, -1.5, "lower_extreme", "lower_extreme"},
            {BarReason::BetweenBands, BarAction::None, 4, 1.25, 0.1875, -0.25, "lower_extreme", "lower_extreme"},
            {BarReason::ConstantWindow, BarAction::None, 4, 1.0, 0.0, std::nullopt, "lower_extreme", "neutral"},
            {BarReason::EntrySell, BarAction::Sell, 4, 3.25, 15.1875, 6.75, "neutral", "upper_extreme"},
        });
}

TEST(StrategyDiagnosticsMeanReversion, EveryIgnoreReasonIsReportedAndAnyPositiveCloseIsAccepted) {
    strategy::MeanReversionStrategy mr{mr_config(4, 1.5, 0.5)};
    Collector collector;
    Sink sink;
    mr.set_observer(&collector);
    mr.on_market_event(event_at("AAPL", 10_px, 100), sink);

    const std::vector<std::pair<domain::MarketEvent, BarReason>> cases{
        {event_at("AAPL", 10_px, 101, domain::MarketEventType::Quote), BarReason::NotABar},
        {event_at("MSFT", 10_px, 101), BarReason::SymbolNotAllowlisted},
        {event_at("AAPL", std::nullopt, 101), BarReason::PriceAbsent},
        {event_at("AAPL", kMinPrice, 101), BarReason::PriceInvalid},
        {event_at("AAPL", -kMaxPrice, 101), BarReason::PriceInvalid},
        {event_at("AAPL", common::Price{}, 101), BarReason::PriceInvalid},
        {event_at("AAPL", -2_px, 101), BarReason::PriceInvalid},
        {event_at("AAPL", 11_px, 100), BarReason::TimeNotAfterLastAccepted},
        {event_at("AAPL", 11_px, 50), BarReason::TimeNotAfterLastAccepted},
    };
    for (const auto& each : cases) {
        mr.on_market_event(each.first, sink);
    }
    ASSERT_EQ(collector.all.size(), cases.size() + 1);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(collector.all[i + 1].reason, cases[i].second) << collector.all[i + 1].reason_text();
        EXPECT_EQ(collector.all[i + 1].verdict, BarVerdict::Ignored);
    }

    // The mean-reversion strategy has no input ceiling: INT64_MAX is an accepted close.
    mr.on_market_event(event_at("AAPL", kMaxPrice, 101), sink);
    EXPECT_EQ(collector.all.back().reason, BarReason::WarmingUp);
    EXPECT_EQ(collector.all.back().window_fill, std::optional<std::size_t>{2});
    EXPECT_TRUE(sink.signals.empty());
}

// ----- Agreement between a snapshot and the signal it accompanies ---------------------

// Feeds `events` to a strategy with an observer and checks, call by call, that a signal
// is emitted exactly when the snapshot's action says so, with the same side and the
// same numbers (the metadata text parsed back to the double the strategy used).
template <class Strategy>
void expect_snapshots_agree_with_signals(Strategy& strategy,
                                         const std::vector<domain::MarketEvent>& events,
                                         const std::vector<std::string>& metadata_keys,
                                         std::size_t expected_signals) {
    Collector collector;
    Sink sink;
    strategy.set_observer(&collector);

    std::size_t signalled = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const std::size_t before = sink.signals.size();
        strategy.on_market_event(events[i], sink);
        const std::size_t emitted = sink.signals.size() - before;

        ASSERT_EQ(collector.all.size(), i + 1) << "one snapshot per call, call " << i;
        const Seen& seen = collector.all.back();
        ASSERT_EQ(emitted, seen.action == BarAction::None ? 0u : 1u) << "call " << i;
        if (emitted == 0) {
            continue;
        }
        ++signalled;
        const domain::TradeSignal& signal = sink.signals.back();
        EXPECT_EQ(signal.side, seen.action == BarAction::Buy ? domain::SignalSide::Buy
                                                             : domain::SignalSide::Sell);
        EXPECT_EQ(signal.symbol, seen.event.symbol);
        for (const std::string& key : metadata_keys) {
            ASSERT_TRUE(seen.number(key).has_value()) << key;
            EXPECT_EQ(*seen.number(key), parse_number(signal.metadata.at(key)))
                << key << " at call " << i << ": the snapshot and the signal use one value";
        }
    }
    EXPECT_EQ(signalled, expected_signals);
}

std::vector<domain::MarketEvent> two_symbol_stream() {
    const std::vector<common::Price> aapl = units({3, 2, 1, 2, 3, 4, 3, 2, 1});
    const std::vector<common::Price> msft = units({10, 10, 10, 6, 9, 2, 30, 30, 30});
    std::vector<domain::MarketEvent> events;
    for (std::size_t i = 0; i < aapl.size(); ++i) {
        events.push_back(event_at("AAPL", aapl[i], static_cast<long long>(i) + 1));
        events.push_back(event_at("MSFT", msft[i], static_cast<long long>(i) + 1));
    }
    return events;
}

TEST(StrategyDiagnosticsAgreement, CrossoverSnapshotsMatchTheirSignalsOnTwoInterleavedSymbols) {
    strategy::MovingAverageCrossoverStrategy sma{sma_config(2, 3, {"AAPL", "MSFT"})};
    // Hand-derived: AAPL Buy at bar 5 and Sell at bar 8, MSFT Buy at bar 7.
    expect_snapshots_agree_with_signals(sma, two_symbol_stream(), {"short_sma", "long_sma"}, 3);
}

TEST(StrategyDiagnosticsAgreement, MeanReversionSnapshotsMatchTheirSignalsOnTwoInterleavedSymbols) {
    strategy::MeanReversionStrategy mr{mr_config(4, 1.2, 0.5, {"AAPL", "MSFT"})};
    // Hand-derived: AAPL Sell at 5 and Buy at 8; MSFT Buy at 4 and 6, Sell at 7.
    expect_snapshots_agree_with_signals(mr, two_symbol_stream(),
                                        {"mean", "standard_deviation", "z_score"}, 5);
}

// ----- The vocabulary itself -------------------------------------------------------

TEST(StrategyDiagnosticsVocabulary, EveryReasonHasADistinctSnakeCaseCode) {
    std::set<std::string> seen;
    for (int value = 0; value <= static_cast<int>(BarReason::BetweenBands); ++value) {
        const std::string code{strategy::to_string(static_cast<BarReason>(value))};
        EXPECT_NE(code, "invalid") << value;
        EXPECT_FALSE(code.empty());
        for (const char c : code) {
            EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_') << code;
        }
        EXPECT_TRUE(seen.insert(code).second) << "duplicate code " << code;
    }
    EXPECT_EQ(seen.size(), 19u);
    EXPECT_EQ(strategy::to_string(static_cast<BarReason>(200)), "invalid");
    EXPECT_EQ(strategy::to_string(static_cast<BarVerdict>(200)), "invalid");
    EXPECT_EQ(strategy::to_string(static_cast<BarAction>(200)), "invalid");
}

TEST(StrategyDiagnosticsVocabulary, ASnapshotKeepsAtMostItsCapacityAndSaysNothingAboutTheRest) {
    strategy::BarSnapshot snapshot;
    for (std::size_t i = 0; i < strategy::BarSnapshot::kMaxIndicators + 3; ++i) {
        snapshot.add_indicator("x", 1.0);
    }
    for (std::size_t i = 0; i < strategy::BarSnapshot::kMaxStates + 3; ++i) {
        snapshot.add_state("s", "v");
    }
    EXPECT_EQ(snapshot.indicator_list().size(), strategy::BarSnapshot::kMaxIndicators);
    EXPECT_EQ(snapshot.state_list().size(), strategy::BarSnapshot::kMaxStates);
}

}  // namespace
