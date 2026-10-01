// StrategyEngine registration: what is accepted, what is refused, and the order
// strategies are kept in. Real engine, RecordingEventBus, FakeStrategy.

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "strategy_engine_fixture.hpp"
#include "trading_engine/common/errors.hpp"

namespace {
using Ids = std::vector<std::string>;
}  // namespace

TEST_F(StrategyEngineTest, StartsEmptyAndStopped) {
    EXPECT_EQ(engine.strategy_count(), 0u);
    EXPECT_FALSE(engine.is_running());
    EXPECT_TRUE(engine.strategy_stats().empty());
}

TEST_F(StrategyEngineTest, RejectsANullStrategy) {
    EXPECT_THROW(engine.register_strategy(nullptr), common::ValidationError);
    EXPECT_EQ(engine.strategy_count(), 0u);
}

TEST_F(StrategyEngineTest, RejectsAnEmptyId) {
    const auto nameless = std::make_shared<support::FakeStrategy>("");
    EXPECT_THROW(engine.register_strategy(nameless), common::ValidationError);
    EXPECT_EQ(engine.strategy_count(), 0u);
}

TEST_F(StrategyEngineTest, RejectsADuplicateIdAndKeepsTheFirstStrategy) {
    const auto first    = add("sma_crossover");
    const auto imposter = std::make_shared<support::FakeStrategy>("sma_crossover");

    const std::string message = thrown_what([&] { engine.register_strategy(imposter); });
    EXPECT_NE(message.find("sma_crossover"), std::string::npos)
        << "the error should name the clashing id, got: " << message;
    EXPECT_THROW(engine.register_strategy(imposter), common::ValidationError);
    EXPECT_EQ(engine.strategy_count(), 1u);

    // Only the first one is wired in: the rejected one is never started or fed.
    engine.start();
    publish_market(market_event("AAPL"));
    EXPECT_EQ(first->received.size(), 1u);
    EXPECT_EQ(imposter->starts, 0);
    EXPECT_TRUE(imposter->received.empty());
}

TEST_F(StrategyEngineTest, RejectsTheSameInstanceRegisteredTwice) {
    const auto once = add("a");
    EXPECT_THROW(engine.register_strategy(once), common::ValidationError);
    EXPECT_EQ(engine.strategy_count(), 1u);
}

TEST_F(StrategyEngineTest, ARejectedRegistrationLeavesTheEngineUsable) {
    add("a");
    EXPECT_THROW(engine.register_strategy(nullptr), common::ValidationError);
    EXPECT_THROW(engine.register_strategy(std::make_shared<support::FakeStrategy>("")),
                 common::ValidationError);
    EXPECT_THROW(engine.register_strategy(std::make_shared<support::FakeStrategy>("a")),
                 common::ValidationError);
    add("b");

    EXPECT_EQ(ids(), (Ids{"a", "b"}));
    EXPECT_EQ(engine.strategy_count(), 2u);
}

TEST_F(StrategyEngineTest, KeepsRegistrationOrderRatherThanSortingById) {
    add("zeta");
    add("alpha");
    add("mid");

    EXPECT_EQ(ids(), (Ids{"zeta", "alpha", "mid"}));
    EXPECT_EQ(engine.strategy_count(), 3u);
}

TEST_F(StrategyEngineTest, RefusesRegistrationWhileRunning) {
    add("a");
    engine.start();

    const auto late = std::make_shared<support::FakeStrategy>("late");
    EXPECT_THROW(engine.register_strategy(late), std::logic_error);
    EXPECT_EQ(engine.strategy_count(), 1u);
    EXPECT_EQ(late->starts, 0);
    EXPECT_TRUE(engine.is_running()) << "a refused registration must not disturb a running engine";
}

TEST_F(StrategyEngineTest, RefusesRegistrationFromInsideOnStart) {
    // The engine is mid-startup, iterating its strategies: growing that list
    // now would be unsafe, so it is refused like any other running state.
    const auto a = add("a");
    a->on_start_hook = [this] {
        engine.register_strategy(std::make_shared<support::FakeStrategy>("sneaky"));
    };

    EXPECT_THROW(engine.start(), std::logic_error);   // surfaces as a failed startup
    EXPECT_EQ(engine.strategy_count(), 1u);
    EXPECT_FALSE(engine.is_running());
    EXPECT_EQ(market_subscriptions(), 0u);
}

TEST_F(StrategyEngineTest, AcceptsRegistrationAgainOnceStopped) {
    const auto a = add("a");
    engine.start();
    engine.stop();

    const auto b = add("b");   // joins the next run
    engine.start();
    publish_market(market_event("AAPL"));

    EXPECT_EQ(a->starts, 2);
    EXPECT_EQ(b->starts, 1);
    EXPECT_EQ(a->received.size(), 1u);
    EXPECT_EQ(b->received.size(), 1u);
    EXPECT_EQ(ids(), (Ids{"a", "b"}));
}
