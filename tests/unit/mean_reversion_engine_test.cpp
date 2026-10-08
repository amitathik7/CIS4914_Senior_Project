// MeanReversionStrategy registered with the real StrategyEngine, over a
// RecordingEventBus and a ManualClock: the signals as the engine stamps and
// publishes them, across a restart, next to the moving-average crossover strategy
// under a different id, and when the bus refuses one. The strategy's own behaviour
// is in mean_reversion_strategy_test.cpp.

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "mean_reversion_fixture.hpp"
#include "strategy_engine_fixture.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

namespace {

using namespace std::chrono_literals;
using mr_test::Closes;
using mr_test::kFixture;
using mr_test::make_config;
using strategy::MeanReversionConfig;
using strategy::MeanReversionStrategy;
using strategy::MovingAverageCrossoverConfig;
using strategy::MovingAverageCrossoverStrategy;

class MeanReversionEngineTest : public StrategyEngineTest {
protected:
    std::shared_ptr<MeanReversionStrategy> add_mean_reversion(MeanReversionConfig cfg) {
        auto mr = std::make_shared<MeanReversionStrategy>(std::move(cfg));
        engine.register_strategy(mr);
        return mr;
    }

    // A finalized bar `minute` minutes after kT0, the way a replay would stamp it.
    static domain::MarketEvent bar_at(const std::string& symbol, common::Price close, long long minute) {
        domain::MarketEvent event = mr_test::bar(symbol, close, minute);
        event.exchange_time       = kT0 + std::chrono::minutes{minute};
        return event;
    }

    // Delivers one bar through the bus. The clock moves to the bar's time first,
    // as a replay advances it, so a signal's created_at is the bar's time.
    void replay(const std::string& symbol, common::Price close, long long minute) {
        clock.set(kT0 + std::chrono::minutes{minute});
        publish_market(bar_at(symbol, close, minute));
    }

    // Bars at minutes 1, 2, 3, ...
    void replay_closes(const Closes& closes, const std::string& symbol = "AAPL") {
        long long minute = 0;
        for (const common::Price close : closes) {
            replay(symbol, close, ++minute);
        }
    }
};

}  // namespace

TEST_F(MeanReversionEngineTest, TheFixtureYieldsTheExpectedStampedSignalsAtTheExpectedTimes) {
    MeanReversionConfig cfg = make_config();
    cfg.requested_quantity  = 10;
    add_mean_reversion(cfg);
    engine.start();

    replay_closes(kFixture);

    ASSERT_EQ(signal_events().size(), 3u) << "exactly three Signal events, nothing else";
    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 3u);

    for (const domain::TradeSignal& signal : signals) {
        EXPECT_EQ(signal.strategy_id, "mean_reversion") << "the engine signs with id()";
        EXPECT_TRUE(signal.id.valid());
        EXPECT_EQ(signal.symbol, "AAPL");
        EXPECT_EQ(signal.requested_quantity, std::optional<common::Quantity>{10});
        EXPECT_EQ(signal.order_type, std::optional<domain::OrderType>{domain::OrderType::Market});
        EXPECT_FALSE(signal.target_exposure.has_value());
        EXPECT_FALSE(signal.limit_price.has_value());
        EXPECT_FALSE(signal.confidence.has_value());
    }

    // Bars 4, 6 and 7: Buy, Buy, Sell. Stamped with the clock at the bar's time
    // and ids 1, 2, 3; bar 5 (the rearm) published nothing.
    EXPECT_EQ(signals[0].side, domain::SignalSide::Buy);
    EXPECT_EQ(signals[0].created_at, kT0 + 4min);
    EXPECT_EQ(signals[0].id.value, 1u);
    EXPECT_EQ(signals[0].metadata.at("trigger"), "z_score_at_or_below_lower_entry");
    EXPECT_EQ(signals[0].metadata.at("mean"), "9");

    EXPECT_EQ(signals[1].side, domain::SignalSide::Buy);
    EXPECT_EQ(signals[1].created_at, kT0 + 6min);
    EXPECT_EQ(signals[1].id.value, 2u);

    EXPECT_EQ(signals[2].side, domain::SignalSide::Sell);
    EXPECT_EQ(signals[2].created_at, kT0 + 7min);
    EXPECT_EQ(signals[2].id.value, 3u);
    EXPECT_EQ(signals[2].metadata.at("trigger"), "z_score_at_or_above_upper_entry");

    const strategy::StrategyEngine::Stats stats = engine.stats();
    EXPECT_EQ(stats.events_routed, 7u);
    EXPECT_EQ(stats.signals_published, 3u);
    EXPECT_EQ(stats.signals_rejected, 0u);
    EXPECT_EQ(stats.publish_errors, 0u);
    EXPECT_EQ(stats.signals_dropped, 0u);
    EXPECT_EQ(stats.strategy_errors, 0u);
}

TEST_F(MeanReversionEngineTest, TheEngineRoutesEveryEventAndTheStrategyIgnoresWhatItDoesNotTrade) {
    add_mean_reversion(make_config());
    engine.start();

    // The engine does not filter: all of these reach the strategy, which ignores them.
    long long minute = 0;
    for (const common::Price close : kFixture) {
        ++minute;
        replay("TSLA", close, minute);   // a bar for a symbol it does not trade
        domain::MarketEvent print = bar_at("AAPL", close, minute);
        print.type                = domain::MarketEventType::Trade;   // a print, not a bar
        publish_market(std::move(print));
    }
    EXPECT_TRUE(bus.signals().empty());
    EXPECT_EQ(engine.stats().events_routed, 14u);
    EXPECT_EQ(engine.strategy_stats()[0].events_delivered, 14u);
    EXPECT_EQ(engine.strategy_stats()[0].errors, 0u) << "ignoring is not an error";

    // None of it left a trace: the same minutes are still fresh for the real bars.
    replay_closes(kFixture);
    EXPECT_EQ(bus.signals().size(), 3u);
}

TEST_F(MeanReversionEngineTest, ARestartResetsTheStrategyWhileSignalIdsKeepCounting) {
    add_mean_reversion(make_config());
    engine.start();
    replay_closes(kFixture);
    ASSERT_EQ(bus.signals().size(), 3u);

    engine.stop();
    engine.start();             // on_start() clears the run; the engine never resets its ids
    replay_closes(kFixture);    // the same bars at the same times: fresh again

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 6u);
    const std::vector<domain::SignalSide> sides{
        domain::SignalSide::Buy, domain::SignalSide::Buy, domain::SignalSide::Sell,
        domain::SignalSide::Buy, domain::SignalSide::Buy, domain::SignalSide::Sell};
    const std::vector<common::Timestamp> times{kT0 + 4min, kT0 + 6min, kT0 + 7min,
                                               kT0 + 4min, kT0 + 6min, kT0 + 7min};
    for (std::size_t i = 0; i < signals.size(); ++i) {
        EXPECT_EQ(signals[i].side, sides[i]) << "signal " << i;
        EXPECT_EQ(signals[i].created_at, times[i]) << "signal " << i;
        EXPECT_EQ(signals[i].id.value, i + 1) << "signal " << i;
    }
    EXPECT_EQ(engine.strategy_stats()[0].errors, 0u);
}

TEST_F(MeanReversionEngineTest, RunsBesideTheCrossoverStrategyAndEachIsSignedAsItself) {
    // The crossover's own defaults are sma_crossover and the mean reversion's are
    // mean_reversion, so the two coexist with no configuration beyond the
    // parameters. Both read the closes 3 2 1 2 3 4 3 2 1:
    //  SMA 2/3:        Buy at bar 5 (short 2.5 over long 2), Sell at bar 8.
    //  Mean reversion: lookback 4, entry 1.2, rearm 0.5. Bar 5 [2 1 2 3] has mean 2 and
    //  variance 0.5, z = +1.4142 -> Sell; bar 6 z = +1.3416 held; bar 7 [2 3 4 3] has
    //  z = 0, rearm; bar 8 [3 4 3 2] z = -1.4142 -> Buy; bar 9 z = -1.3416 held.
    // The two disagree on direction at the same bars: opposite philosophies, and not
    // this strategy's business to reconcile. Signals are published in registration order.
    MovingAverageCrossoverConfig sma_cfg;
    sma_cfg.short_window = 2;
    sma_cfg.long_window  = 3;
    sma_cfg.symbols      = {"AAPL"};
    engine.register_strategy(std::make_shared<MovingAverageCrossoverStrategy>(sma_cfg));
    add_mean_reversion(make_config(4, 1.2, 0.5));
    EXPECT_EQ(ids(), (std::vector<std::string>{"sma_crossover", "mean_reversion"}));
    engine.start();

    replay_closes(mr_test::units({3, 2, 1, 2, 3, 4, 3, 2, 1}));

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 4u);
    const std::vector<std::string> owners{"sma_crossover", "mean_reversion", "sma_crossover",
                                          "mean_reversion"};
    const std::vector<domain::SignalSide> sides{domain::SignalSide::Buy, domain::SignalSide::Sell,
                                                domain::SignalSide::Sell, domain::SignalSide::Buy};
    const std::vector<common::Timestamp> times{kT0 + 5min, kT0 + 5min, kT0 + 8min, kT0 + 8min};
    for (std::size_t i = 0; i < signals.size(); ++i) {
        EXPECT_EQ(signals[i].strategy_id, owners[i]) << "signal " << i;
        EXPECT_EQ(signals[i].side, sides[i]) << "signal " << i;
        EXPECT_EQ(signals[i].created_at, times[i]) << "signal " << i;
        EXPECT_EQ(signals[i].id.value, i + 1) << "signal " << i;
    }
    EXPECT_EQ(engine.strategy_stats()[0].errors, 0u);
    EXPECT_EQ(engine.strategy_stats()[1].errors, 0u);
}

TEST_F(MeanReversionEngineTest, TwoInstancesWithDifferentIdsRunSideBySide) {
    MeanReversionConfig quick = make_config(4, 1.2, 0.5);
    quick.strategy_id         = "mean_reversion_4";
    MeanReversionConfig slow  = make_config(2, 1.0, 0.5);
    slow.strategy_id          = "mean_reversion_2";
    add_mean_reversion(quick);
    add_mean_reversion(slow);
    engine.start();

    // Lookback 2 with entry 1.0 sees a Sell on any rising pair and a Buy on any
    // falling one (z = +-1 exactly, the inclusive boundary), then holds while the
    // direction stays: closes 1 2 3 2: Sell at bar 2 (2 over 1), held at bar 3, Buy at 4.
    replay_closes(mr_test::units({1, 2, 3, 2}));

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 2u);
    EXPECT_EQ(signals[0].strategy_id, "mean_reversion_2");
    EXPECT_EQ(signals[0].side, domain::SignalSide::Sell);
    EXPECT_EQ(signals[0].created_at, kT0 + 2min);
    EXPECT_EQ(signals[1].strategy_id, "mean_reversion_2");
    EXPECT_EQ(signals[1].side, domain::SignalSide::Buy);
    EXPECT_EQ(signals[1].created_at, kT0 + 4min);
    // The lookback-4 instance's only full window, [1 2 3 2], has z = 0: no signal.
}

TEST_F(MeanReversionEngineTest, TwoInstancesWithTheDefaultIdCollideAtRegistration) {
    add_mean_reversion(make_config());

    const std::string what = thrown_what([&] { add_mean_reversion(make_config(8)); });

    EXPECT_NE(what.find("mean_reversion"), std::string::npos) << what;
    EXPECT_EQ(engine.strategy_count(), 1u) << "a rejected registration leaves the engine unchanged";
}

TEST_F(MeanReversionEngineTest, TheDocumentedExampleBuildsRegistersAndTrades) {
    // The construction shown in docs/STRATEGIES.md (section 3), next to a crossover instance
    // with its own id.
    engine.register_strategy(std::make_shared<MovingAverageCrossoverStrategy>(
        MovingAverageCrossoverConfig{
            .strategy_id        = "sma_5_20",
            .short_window       = 5,
            .long_window        = 20,
            .requested_quantity = 10,
            .symbols            = {"AAPL", "MSFT"},
        }));
    engine.register_strategy(std::make_shared<MeanReversionStrategy>(MeanReversionConfig{
        .strategy_id        = "mean_reversion_20",
        .lookback           = 20,
        .entry_threshold    = 2.0,
        .rearm_threshold    = 0.5,
        .requested_quantity = 10,
        .symbols            = {"AAPL", "MSFT"},
    }));
    engine.start();

    // Nineteen closes of 100 then a 110: the mean reversion's first full window is
    // z = +4.3589 (see the strategy test), a Sell. The crossover sees equal averages
    // all the way, then its silent baseline, and says nothing.
    Closes closes(19, common::Price::from_units(100));
    closes.push_back(common::Price::from_units(110));
    replay_closes(closes);

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].strategy_id, "mean_reversion_20");
    EXPECT_EQ(signals[0].side, domain::SignalSide::Sell);
    EXPECT_EQ(signals[0].requested_quantity, std::optional<common::Quantity>{10});
    EXPECT_EQ(signals[0].created_at, kT0 + 20min);
}

TEST_F(MeanReversionEngineTest, ASignalTheBusRefusesIsLostNotResentWhileTheExcursionLasts) {
    add_mean_reversion(make_config());
    engine.start();
    const Closes closes = mr_test::units({10, 10, 10, 6, 2, 1, 1, 1, 1, 10});
    replay_closes(Closes(closes.begin(), closes.begin() + 3));   // bars 1-3: warming up

    // Bar 4 is the Buy, and the bus refuses it. A direct call: the double would
    // refuse a market event published through it as well.
    bus.faults.reject_publish = true;
    clock.set(kT0 + 4min);
    engine.on_market_event(bar_at("AAPL", closes[3], 4));
    bus.faults.reject_publish = false;
    EXPECT_EQ(engine.stats().signals_rejected, 1u);

    // The latch was committed when the signal was emitted, so bar 5 (still beyond
    // the entry, z = -1.5076) and the held bars after it do not offer the Buy again;
    // only the Sell at bar 10 follows, after the constant window at bar 9 rearms.
    for (long long minute = 5; minute <= 10; ++minute) {
        replay("AAPL", closes[static_cast<std::size_t>(minute) - 1], minute);
    }
    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].side, domain::SignalSide::Sell);
    EXPECT_EQ(signals[0].created_at, kT0 + 10min);
    EXPECT_EQ(engine.stats().signals_published, 1u);
    EXPECT_EQ(engine.stats().signals_rejected, 1u);
}
