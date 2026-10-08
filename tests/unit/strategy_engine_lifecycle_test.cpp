// StrategyEngine lifecycle: start / stop / restart, rollback when startup
// fails, and what happens to events that arrive when the engine is not running.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "strategy_engine_fixture.hpp"

namespace {
using support::CallLog;
}  // namespace

// --- Start -------------------------------------------------------------------

TEST_F(StrategyEngineTest, ConstructingAndRegisteringDoNotSubscribe) {
    add("a");
    EXPECT_EQ(bus.subscribe_calls(), 0u);
    EXPECT_EQ(market_subscriptions(), 0u);
    EXPECT_FALSE(engine.is_running());
}

TEST_F(StrategyEngineTest, StartRunsOnStartInRegistrationOrderThenSubscribesOnce) {
    const auto a = add("a");
    const auto b = add("b");

    engine.start();

    EXPECT_TRUE(engine.is_running());
    EXPECT_EQ(calls, (CallLog{"a:start", "b:start"}));
    EXPECT_EQ(a->starts, 1);
    EXPECT_EQ(b->starts, 1);
    EXPECT_EQ(market_subscriptions(), 1u);
    EXPECT_EQ(bus.subscribe_calls(), 1u);
}

TEST_F(StrategyEngineTest, StartSubscribesOnlyAfterEveryStrategyHasStarted) {
    // No event may reach a strategy that has not started yet, so the engine
    // must not be listening while on_start() callbacks are still running.
    const auto a = add("a");
    const auto b = add("b");
    std::size_t seen_by_a = 99;
    std::size_t seen_by_b = 99;
    a->on_start_hook = [&] { seen_by_a = market_subscriptions(); };
    b->on_start_hook = [&] { seen_by_b = market_subscriptions(); };

    engine.start();

    EXPECT_EQ(seen_by_a, 0u);
    EXPECT_EQ(seen_by_b, 0u);
    EXPECT_EQ(market_subscriptions(), 1u);
}

TEST_F(StrategyEngineTest, RepeatedStartDoesNotDuplicateSubscriptionsOrCallbacks) {
    const auto a = add("a");

    engine.start();
    engine.start();
    engine.start();

    EXPECT_EQ(a->starts, 1);
    EXPECT_EQ(bus.subscribe_calls(), 1u);
    EXPECT_EQ(market_subscriptions(), 1u);

    publish_market(market_event("AAPL"));
    EXPECT_EQ(a->received.size(), 1u) << "one event must produce exactly one callback";
}

// --- Stop --------------------------------------------------------------------

TEST_F(StrategyEngineTest, StopUnsubscribesThenCallsOnStopInReverseRegistrationOrder) {
    const auto a = add("a");
    const auto b = add("b");
    const auto c = add("c");
    engine.start();
    calls.clear();

    // on_stop() must find the engine already unsubscribed.
    std::vector<std::size_t> subscriptions_seen;
    for (const auto& each : {a, b, c}) {
        each->on_stop_hook = [&] { subscriptions_seen.push_back(market_subscriptions()); };
    }

    engine.stop();

    EXPECT_FALSE(engine.is_running());
    EXPECT_EQ(calls, (CallLog{"c:stop", "b:stop", "a:stop"}));
    EXPECT_EQ(subscriptions_seen, (std::vector<std::size_t>{0u, 0u, 0u}));
    EXPECT_EQ(bus.unsubscribe_calls(), 1u);
    EXPECT_EQ(market_subscriptions(), 0u);
}

TEST_F(StrategyEngineTest, RepeatedStopDoesNotRepeatTheTeardown) {
    const auto a = add("a");
    engine.start();

    engine.stop();
    engine.stop();
    engine.stop();

    EXPECT_EQ(a->stops, 1);
    EXPECT_EQ(bus.unsubscribe_calls(), 1u);
}

TEST_F(StrategyEngineTest, StopOnAnEngineThatNeverStartedDoesNothing) {
    const auto a = add("a");

    engine.stop();

    EXPECT_EQ(a->stops, 0);
    EXPECT_EQ(bus.unsubscribe_calls(), 0u);
}

TEST_F(StrategyEngineTest, StopIsolatesAnOnStopFailure) {
    add("a");
    const auto b = add("b");
    add("c");
    b->throw_on_stop = true;
    engine.start();
    calls.clear();

    EXPECT_NO_THROW(engine.stop());

    EXPECT_FALSE(engine.is_running());
    EXPECT_EQ(calls, (CallLog{"c:stop", "b:stop", "a:stop"})) << "every strategy must still be stopped";

    const auto stats = engine.strategy_stats();
    ASSERT_EQ(stats.size(), 3u);
    EXPECT_EQ(stats[0].errors, 0u);
    EXPECT_EQ(stats[1].errors, 1u);
    EXPECT_EQ(stats[1].last_error, "on_stop: b: on_stop failed");
    EXPECT_EQ(stats[2].errors, 0u);
    EXPECT_EQ(engine.stats().strategy_errors, 1u);
}

TEST_F(StrategyEngineTest, StopCompletesEvenIfUnsubscribeFails) {
    const auto a = add("a");
    engine.start();
    bus.faults.throw_on_unsubscribe = true;

    EXPECT_NO_THROW(engine.stop());

    EXPECT_FALSE(engine.is_running());
    EXPECT_EQ(a->stops, 1) << "strategies are stopped even though the bus misbehaved";
    EXPECT_EQ(engine.stats().bus_errors, 1u);

    // The bus still holds the handler, but the engine has disowned it.
    EXPECT_EQ(market_subscriptions(), 1u);
    bus.faults.throw_on_unsubscribe = false;
    publish_market(market_event("AAPL"));
    EXPECT_TRUE(a->received.empty());
}

// --- Restart -----------------------------------------------------------------

TEST_F(StrategyEngineTest, RestartResubscribesOnceAndRestartsEachStrategyOnce) {
    const auto a = add("a");

    engine.start();
    engine.stop();
    engine.start();

    EXPECT_TRUE(engine.is_running());
    EXPECT_EQ(a->starts, 2);
    EXPECT_EQ(a->stops, 1);
    EXPECT_EQ(bus.subscribe_calls(), 2u);
    EXPECT_EQ(market_subscriptions(), 1u) << "the first subscription must be gone, not stacked";

    publish_market(market_event("AAPL"));
    EXPECT_EQ(a->received.size(), 1u);

    engine.stop();
    EXPECT_EQ(a->stops, 2);
    EXPECT_EQ(market_subscriptions(), 0u);
}

TEST_F(StrategyEngineTest, StatisticsKeepAccumulatingAcrossARestart) {
    add("a");

    engine.start();
    publish_market(market_event("AAPL"));
    engine.stop();
    engine.start();
    publish_market(market_event("AAPL"));
    publish_market(market_event("AAPL"));

    EXPECT_EQ(engine.stats().events_routed, 3u);
    EXPECT_EQ(engine.strategy_stats()[0].events_delivered, 3u);
}

// --- Startup failure ---------------------------------------------------------

TEST_F(StrategyEngineTest, AFailedOnStartRollsBackTheStrategiesThatHadStarted) {
    add("a");
    const auto b = add("b");
    const auto c = add("c");
    b->throw_on_start = true;

    try {
        engine.start();
        FAIL() << "start() should have rethrown the strategy's exception";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "b: on_start failed") << "the original exception must propagate";
    }

    // a started and is rolled back. b's on_start() never completed, so b is not
    // stopped; c was never reached.
    EXPECT_EQ(calls, (CallLog{"a:start", "b:start", "a:stop"}));
    EXPECT_EQ(c->starts, 0);
    EXPECT_FALSE(engine.is_running());
    EXPECT_EQ(bus.subscribe_calls(), 0u);
    EXPECT_EQ(market_subscriptions(), 0u);

    // Once the fault is gone the engine starts everything from scratch.
    b->throw_on_start = false;
    calls.clear();
    engine.start();
    EXPECT_TRUE(engine.is_running());
    EXPECT_EQ(calls, (CallLog{"a:start", "b:start", "c:start"}));
    EXPECT_EQ(market_subscriptions(), 1u);
}

TEST_F(StrategyEngineTest, AFailedSubscribeRollsBackEveryStrategyAndLeavesNothingSubscribed) {
    add("a");
    add("b");
    bus.faults.throw_on_subscribe = true;

    EXPECT_THROW(engine.start(), std::runtime_error);

    EXPECT_EQ(calls, (CallLog{"a:start", "b:start", "b:stop", "a:stop"}));
    EXPECT_FALSE(engine.is_running());
    EXPECT_EQ(market_subscriptions(), 0u);

    bus.faults.throw_on_subscribe = false;
    calls.clear();
    engine.start();
    EXPECT_TRUE(engine.is_running());
    EXPECT_EQ(calls, (CallLog{"a:start", "b:start"}));
    EXPECT_EQ(market_subscriptions(), 1u);
}

TEST_F(StrategyEngineTest, RollbackSurvivesAnOnStopFailureAndStillReportsTheStartupError) {
    const auto a = add("a");
    const auto b = add("b");
    a->throw_on_stop  = true;   // fails while being rolled back
    b->throw_on_start = true;   // the failure that aborts startup

    EXPECT_EQ(thrown_what([&] { engine.start(); }), "b: on_start failed");

    EXPECT_FALSE(engine.is_running());
    const auto stats = engine.strategy_stats();
    EXPECT_EQ(stats[0].errors, 1u);
    EXPECT_EQ(stats[0].last_error, "on_stop: a: on_stop failed");
}

// --- Events while not running -----------------------------------------------

TEST_F(StrategyEngineTest, EventsBeforeStartAreDroppedAndCounted) {
    const auto a = add("a");
    a->emits = {signal_for("AAPL")};

    engine.on_market_event(market_event("AAPL"));

    EXPECT_TRUE(a->received.empty());
    EXPECT_TRUE(bus.published().empty()) << "a stopped engine must not emit signals";
    EXPECT_EQ(engine.stats().events_dropped, 1u);
    EXPECT_EQ(engine.stats().events_routed, 0u);
}

TEST_F(StrategyEngineTest, EventsAfterStopAreDroppedAndCounted) {
    const auto a = add("a");
    a->emits = {signal_for("AAPL")};
    engine.start();
    engine.stop();

    engine.on_market_event(market_event("AAPL"));   // a source still holding the engine as its sink
    publish_market(market_event("MSFT"));           // nobody is subscribed any more

    EXPECT_TRUE(a->received.empty());
    EXPECT_TRUE(bus.signals().empty());
    EXPECT_EQ(engine.stats().events_dropped, 1u) << "only the direct call can reach a stopped engine";
    EXPECT_EQ(engine.stats().events_routed, 0u);
}

TEST_F(StrategyEngineTest, EventsArrivingMidStartupOrMidShutdownAreDropped) {
    const auto a = add("a");
    a->on_start_hook = [this] { engine.on_market_event(market_event("EARLY")); };
    a->on_stop_hook  = [this] { engine.on_market_event(market_event("LATE")); };

    engine.start();
    engine.stop();

    EXPECT_TRUE(a->received.empty());
    EXPECT_EQ(engine.stats().events_dropped, 2u);
}

// --- Single-threaded model --------------------------------------------------

TEST_F(StrategyEngineTest, EveryCallbackRunsOnTheThreadThatDrivesTheEngine) {
    const auto a = add("a");
    std::vector<std::thread::id> seen;
    a->on_start_hook = [&] { seen.push_back(std::this_thread::get_id()); };
    a->on_event_hook = [&](const domain::MarketEvent&, strategy::ISignalSink&) {
        seen.push_back(std::this_thread::get_id());
    };
    a->on_stop_hook = [&] { seen.push_back(std::this_thread::get_id()); };

    engine.start();
    publish_market(market_event("AAPL"));
    engine.stop();

    ASSERT_EQ(seen.size(), 3u);
    for (const std::thread::id id : seen) {
        EXPECT_EQ(id, std::this_thread::get_id()) << "the engine must not hand work to another thread";
    }
}

TEST_F(StrategyEngineTest, TheEngineLeavesTheBusLifecycleToItsOwner) {
    add("a");

    engine.start();
    publish_market(market_event("AAPL"));
    engine.stop();
    engine.start();
    engine.stop();

    EXPECT_EQ(bus.lifecycle_calls(), 0u) << "start / request_shutdown / wait_until_drained are not the engine's";
}
