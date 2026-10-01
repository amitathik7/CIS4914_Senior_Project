// MovingAverageCrossoverStrategy registered with the real StrategyEngine, over a
// RecordingEventBus and a ManualClock: the signals as the engine stamps and
// publishes them, across a restart, next to a second instance, and when the bus
// refuses one. The strategy's own behaviour is in
// moving_average_crossover_strategy_test.cpp.

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "moving_average_crossover_fixture.hpp"
#include "strategy_engine_fixture.hpp"

namespace {

using namespace std::chrono_literals;
using sma_test::Closes;
using sma_test::kFixture;
using sma_test::make_config;
using strategy::MovingAverageCrossoverConfig;
using strategy::MovingAverageCrossoverStrategy;

class SmaCrossoverEngineTest : public StrategyEngineTest {
protected:
    std::shared_ptr<MovingAverageCrossoverStrategy> add_sma(MovingAverageCrossoverConfig cfg) {
        auto sma = std::make_shared<MovingAverageCrossoverStrategy>(std::move(cfg));
        engine.register_strategy(sma);
        return sma;
    }

    // A finalized bar `minute` minutes after kT0, the way a replay would stamp it.
    static domain::MarketEvent bar_at(const std::string& symbol, double close, long long minute) {
        domain::MarketEvent event = sma_test::bar(symbol, close, minute);
        event.exchange_time       = kT0 + std::chrono::minutes{minute};
        return event;
    }

    // Delivers one bar through the bus. The clock moves to the bar's time first,
    // as a replay advances it, so a signal's created_at is the bar's time.
    void replay(const std::string& symbol, double close, long long minute) {
        clock.set(kT0 + std::chrono::minutes{minute});
        publish_market(bar_at(symbol, close, minute));
    }

    // Bars at minutes 1, 2, 3, ...
    void replay_closes(const Closes& closes, const std::string& symbol = "AAPL") {
        long long minute = 0;
        for (const double close : closes) {
            replay(symbol, close, ++minute);
        }
    }
};

}  // namespace

TEST_F(SmaCrossoverEngineTest, TheSpecFixtureYieldsTheExpectedStampedSignalsAtTheExpectedTimes) {
    MovingAverageCrossoverConfig cfg = make_config(2, 3);
    cfg.requested_quantity           = 10.0;
    add_sma(cfg);
    engine.start();

    replay_closes(kFixture);

    ASSERT_EQ(signal_events().size(), 2u) << "exactly two Signal events, nothing else";
    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 2u);

    for (const domain::TradeSignal& signal : signals) {
        EXPECT_EQ(signal.strategy_id, "sma_crossover") << "the engine signs with id()";
        EXPECT_TRUE(signal.id.valid());
        EXPECT_EQ(signal.symbol, "AAPL");
        EXPECT_EQ(signal.requested_quantity, std::optional<double>{10.0});
        EXPECT_EQ(signal.order_type, std::optional<domain::OrderType>{domain::OrderType::Market});
        EXPECT_FALSE(signal.target_exposure.has_value());
        EXPECT_FALSE(signal.limit_price.has_value());
        EXPECT_FALSE(signal.confidence.has_value());
    }

    // Bar 5: Buy. Bar 8: Sell. Stamped with the clock at the bar's time and ids 1, 2.
    EXPECT_EQ(signals[0].side, domain::SignalSide::Buy);
    EXPECT_EQ(signals[0].created_at, kT0 + 5min);
    EXPECT_EQ(signals[0].id.value, 1u);
    EXPECT_EQ(signals[0].metadata.at("trigger"), "short_crossed_above_long");
    EXPECT_EQ(signals[0].metadata.at("short_sma"), "2.5");
    EXPECT_EQ(signals[0].metadata.at("long_sma"), "2");

    EXPECT_EQ(signals[1].side, domain::SignalSide::Sell);
    EXPECT_EQ(signals[1].created_at, kT0 + 8min);
    EXPECT_EQ(signals[1].id.value, 2u);
    EXPECT_EQ(signals[1].metadata.at("trigger"), "short_crossed_below_long");
    EXPECT_EQ(signals[1].metadata.at("short_sma"), "2.5");
    EXPECT_EQ(signals[1].metadata.at("long_sma"), "3");

    const strategy::StrategyEngine::Stats stats = engine.stats();
    EXPECT_EQ(stats.events_routed, 9u);
    EXPECT_EQ(stats.signals_published, 2u);
    EXPECT_EQ(stats.signals_rejected, 0u);
    EXPECT_EQ(stats.publish_errors, 0u);
    EXPECT_EQ(stats.signals_dropped, 0u);
    EXPECT_EQ(stats.strategy_errors, 0u);
}

TEST_F(SmaCrossoverEngineTest, TheEngineRoutesEveryEventAndTheStrategyIgnoresWhatItDoesNotTrade) {
    add_sma(make_config(2, 3));
    engine.start();

    // The engine does not filter: all of these reach the strategy, which ignores them.
    long long minute = 0;
    for (const double close : kFixture) {
        ++minute;
        replay("TSLA", close, minute);   // a bar for a symbol it does not trade
        domain::MarketEvent print = bar_at("AAPL", close, minute);
        print.type                = domain::MarketEventType::Trade;   // a print, not a bar
        publish_market(std::move(print));
    }
    EXPECT_TRUE(bus.signals().empty());
    EXPECT_EQ(engine.stats().events_routed, 18u);
    EXPECT_EQ(engine.strategy_stats()[0].events_delivered, 18u);
    EXPECT_EQ(engine.strategy_stats()[0].errors, 0u) << "ignoring is not an error";

    // None of it left a trace: the same minutes are still fresh for the real bars.
    replay_closes(kFixture);
    EXPECT_EQ(bus.signals().size(), 2u);
}

TEST_F(SmaCrossoverEngineTest, ARestartResetsTheStrategyWhileSignalIdsKeepCounting) {
    add_sma(make_config(2, 3));
    engine.start();
    replay_closes(kFixture);
    ASSERT_EQ(bus.signals().size(), 2u);

    engine.stop();
    engine.start();             // on_start() clears the run; the engine never resets its ids
    replay_closes(kFixture);    // the same bars at the same times: fresh again

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 4u);
    const std::vector<domain::SignalSide> sides{domain::SignalSide::Buy, domain::SignalSide::Sell,
                                                domain::SignalSide::Buy, domain::SignalSide::Sell};
    const std::vector<common::Timestamp> times{kT0 + 5min, kT0 + 8min, kT0 + 5min, kT0 + 8min};
    for (std::size_t i = 0; i < signals.size(); ++i) {
        EXPECT_EQ(signals[i].side, sides[i]) << "signal " << i;
        EXPECT_EQ(signals[i].created_at, times[i]) << "signal " << i;
        EXPECT_EQ(signals[i].id.value, i + 1) << "signal " << i;
    }
    EXPECT_EQ(engine.strategy_stats()[0].errors, 0u);
}

TEST_F(SmaCrossoverEngineTest, TwoInstancesWithDifferentIdsRunSideBySideAndAreSignedAsThemselves) {
    MovingAverageCrossoverConfig fast = make_config(1, 2);
    fast.strategy_id                  = "sma_fast";
    MovingAverageCrossoverConfig slow = make_config(2, 3);
    slow.strategy_id                  = "sma_slow";
    add_sma(fast);
    add_sma(slow);
    engine.start();

    replay_closes(kFixture);

    // fast (1/2): Buy at bar 4 (short 2, long 1.5), Sell at bar 7 (short 3, long 3.5).
    // slow (2/3): Buy at bar 5, Sell at bar 8. Published in the order they happened.
    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 4u);
    const std::vector<std::string> owners{"sma_fast", "sma_slow", "sma_fast", "sma_slow"};
    const std::vector<domain::SignalSide> sides{domain::SignalSide::Buy, domain::SignalSide::Buy,
                                                domain::SignalSide::Sell, domain::SignalSide::Sell};
    const std::vector<common::Timestamp> times{kT0 + 4min, kT0 + 5min, kT0 + 7min, kT0 + 8min};
    for (std::size_t i = 0; i < signals.size(); ++i) {
        EXPECT_EQ(signals[i].strategy_id, owners[i]) << "signal " << i;
        EXPECT_EQ(signals[i].side, sides[i]) << "signal " << i;
        EXPECT_EQ(signals[i].created_at, times[i]) << "signal " << i;
        EXPECT_EQ(signals[i].id.value, i + 1) << "signal " << i;
    }
    EXPECT_EQ(signals[0].metadata.at("short_window"), "1");
    EXPECT_EQ(signals[0].metadata.at("short_sma"), "2");
    EXPECT_EQ(signals[0].metadata.at("long_sma"), "1.5");
    EXPECT_EQ(signals[2].metadata.at("short_sma"), "3");
    EXPECT_EQ(signals[2].metadata.at("long_sma"), "3.5");
}

TEST_F(SmaCrossoverEngineTest, TwoInstancesWithTheDefaultIdCollideAtRegistration) {
    add_sma(make_config(2, 3));

    const std::string what = thrown_what([&] { add_sma(make_config(1, 2)); });

    EXPECT_NE(what.find("sma_crossover"), std::string::npos) << what;
    EXPECT_EQ(engine.strategy_count(), 1u) << "a rejected registration leaves the engine unchanged";
}

TEST_F(SmaCrossoverEngineTest, TheDocumentedExampleBuildsRegistersAndTrades) {
    // The construction shown in docs/strategies/moving_average_crossover.md.
    engine.register_strategy(std::make_shared<MovingAverageCrossoverStrategy>(
        MovingAverageCrossoverConfig{
            .strategy_id        = "sma_5_20",
            .short_window       = 5,
            .long_window        = 20,
            .requested_quantity = 10,
            .symbols            = {"AAPL", "MSFT"},
        }));
    engine.start();

    // 20 closes falling from 120 to 101, then a 200: the 5/20 Buy of the strategy test.
    Closes closes;
    for (int i = 0; i < 20; ++i) {
        closes.push_back(120.0 - i);
    }
    closes.push_back(200.0);
    replay_closes(closes);

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].strategy_id, "sma_5_20");
    EXPECT_EQ(signals[0].side, domain::SignalSide::Buy);
    EXPECT_EQ(signals[0].requested_quantity, std::optional<double>{10.0});
    EXPECT_EQ(signals[0].created_at, kT0 + 21min);
}

TEST_F(SmaCrossoverEngineTest, ASignalTheBusRefusesIsLostNotResent) {
    add_sma(make_config(2, 3));
    engine.start();
    replay_closes({3, 2, 1, 2});   // bars 1-4: the baseline and a quiet bar

    // Bar 5 is the Buy, and the bus refuses it. A direct call: the double would
    // refuse a market event published through it as well.
    bus.faults.reject_publish = true;
    clock.set(kT0 + 5min);
    engine.on_market_event(bar_at("AAPL", kFixture[4], 5));
    bus.faults.reject_publish = false;
    EXPECT_EQ(engine.stats().signals_rejected, 1u);

    // The crossover was committed when the signal was emitted, so later bars (still
    // above) do not offer it again; only the real Sell at bar 8 follows.
    for (long long minute = 6; minute <= 9; ++minute) {
        replay("AAPL", kFixture[static_cast<std::size_t>(minute) - 1], minute);
    }
    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].side, domain::SignalSide::Sell);
    EXPECT_EQ(signals[0].created_at, kT0 + 8min);
    EXPECT_EQ(engine.stats().signals_published, 1u);
    EXPECT_EQ(engine.stats().signals_rejected, 1u);
}
