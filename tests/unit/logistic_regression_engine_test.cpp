// LogisticRegressionStrategy registered with the real StrategyEngine, over a
// RecordingEventBus and a ManualClock, next to the two reference strategies: the
// signals as the engine stamps and publishes them, confidence carried through, and
// a direct run giving the same decisions as the engine run. The strategy's own
// behaviour is in logistic_regression_strategy_test.cpp.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "logistic_regression_fixture.hpp"
#include "strategy_engine_fixture.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

namespace {

using lr_test::Closes;
using lr_test::make_config;

class LogisticRegressionEngineTest : public StrategyEngineTest {
protected:
    void replay_closes(const Closes& closes, const std::string& symbol = "AAPL") {
        long long minute = 0;
        for (const common::Price close : closes) {
            ++minute;
            clock.set(kT0 + std::chrono::minutes{minute});
            domain::MarketEvent event = lr_test::bar(symbol, close, minute);
            event.exchange_time       = kT0 + std::chrono::minutes{minute};
            publish_market(std::move(event));
        }
    }
};

TEST_F(LogisticRegressionEngineTest, RunsBesideTheReferenceStrategiesAndMatchesADirectRun) {
    strategy::MovingAverageCrossoverConfig sma;
    sma.short_window = 5;
    sma.long_window  = 20;
    sma.symbols      = {"AAPL"};
    strategy::MeanReversionConfig mr;
    mr.symbols = {"AAPL"};
    engine.register_strategy(std::make_shared<strategy::MovingAverageCrossoverStrategy>(sma));
    engine.register_strategy(std::make_shared<strategy::MeanReversionStrategy>(mr));
    engine.register_strategy(std::make_shared<strategy::LogisticRegressionStrategy>(make_config()));
    engine.start();

    const Closes closes = lr_test::trending(300);
    replay_closes(closes);

    std::vector<domain::TradeSignal> ml_signals;
    for (const domain::TradeSignal& signal : bus.signals()) {
        if (signal.strategy_id == "ml_logistic") {
            ml_signals.push_back(signal);
        }
    }
    ASSERT_FALSE(ml_signals.empty());
    for (const domain::TradeSignal& signal : ml_signals) {
        EXPECT_TRUE(signal.id.valid());
        EXPECT_TRUE(signal.confidence.has_value()) << "the engine forwards confidence unchanged";
        EXPECT_GE(signal.created_at, kT0);
    }
    EXPECT_EQ(engine.stats().strategy_errors, 0u);

    // The same bars fed directly give the same decisions, in the same order.
    lr_test::Harness direct{make_config()};
    direct.feed_closes(closes);
    ASSERT_EQ(direct.signals.size(), ml_signals.size());
    for (std::size_t i = 0; i < ml_signals.size(); ++i) {
        EXPECT_EQ(ml_signals[i].side, direct.signals[i].side);
        EXPECT_EQ(ml_signals[i].confidence, direct.signals[i].confidence);
        EXPECT_EQ(ml_signals[i].metadata, direct.signals[i].metadata);
    }
}

TEST_F(LogisticRegressionEngineTest, ARestartStartsTheModelCold) {
    auto ml = std::make_shared<strategy::LogisticRegressionStrategy>(make_config());
    engine.register_strategy(ml);
    engine.start();
    const Closes closes = lr_test::trending(200);
    replay_closes(closes);
    const std::size_t first_run = bus.signals().size();
    const auto first_parameters = ml->parameters("AAPL");
    ASSERT_GT(first_run, 0u);

    engine.stop();
    engine.start();
    EXPECT_EQ(ml->training_samples("AAPL"), std::optional<std::size_t>{0});
    replay_closes(closes);
    EXPECT_EQ(bus.signals().size(), 2 * first_run);
    EXPECT_EQ(ml->parameters("AAPL"), first_parameters);
}

}  // namespace
