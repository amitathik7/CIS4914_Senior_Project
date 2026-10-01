// StrategyEngine failure isolation: one strategy's failure must not cost the
// others their event or take the engine down, and every failure must leave a
// trace the caller can read.

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "strategy_engine_fixture.hpp"

namespace {
using support::CallLog;
}  // namespace

TEST_F(StrategyEngineTest, AThrowingStrategyDoesNotCostTheOthersTheEvent) {
    const auto a = add("a");
    const auto b = add("b");
    const auto c = add("c");
    b->throw_on_event = true;
    engine.start();
    calls.clear();

    EXPECT_NO_THROW(publish_market(market_event("AAPL")));

    EXPECT_EQ(calls, (CallLog{"a:event:AAPL", "b:event:AAPL", "c:event:AAPL"}))
        << "c runs after b failed";
    EXPECT_EQ(a->received.size(), 1u);
    EXPECT_EQ(c->received.size(), 1u);
    EXPECT_TRUE(engine.is_running()) << "a strategy failure must not stop the engine";
    EXPECT_EQ(market_subscriptions(), 1u);
}

TEST_F(StrategyEngineTest, AThrowingStrategyKeepsReceivingLaterEvents) {
    const auto a = add("a");
    const auto b = add("b");
    b->throw_on_event = true;
    engine.start();

    publish_market(market_event("AAPL"));
    publish_market(market_event("MSFT"));
    publish_market(market_event("AAPL"));

    EXPECT_EQ(a->received.size(), 3u);
    EXPECT_EQ(b->received.size(), 3u) << "a failing strategy is not unregistered";
    EXPECT_EQ(engine.stats().strategy_errors, 3u);
    EXPECT_EQ(engine.stats().events_routed, 3u);
}

TEST_F(StrategyEngineTest, OnMarketEventNeverThrowsWhateverTheStrategiesThrow) {
    const auto a = add("a");
    const auto b = add("b");
    const auto c = add("c");
    a->throw_on_event = true;   // std::runtime_error
    b->on_event_hook = [](const domain::MarketEvent&, strategy::ISignalSink&) { throw 42; };
    c->on_event_hook = [](const domain::MarketEvent&, strategy::ISignalSink&) {
        throw std::out_of_range{"index"};
    };
    engine.start();

    EXPECT_NO_THROW(engine.on_market_event(market_event("AAPL")));

    const auto stats = engine.strategy_stats();
    EXPECT_EQ(stats[0].errors, 1u);
    EXPECT_EQ(stats[1].errors, 1u);
    EXPECT_EQ(stats[1].last_error, "on_market_event: non-standard exception");
    EXPECT_EQ(stats[2].errors, 1u);
    EXPECT_EQ(stats[2].last_error, "on_market_event: index");
    EXPECT_EQ(engine.stats().strategy_errors, 3u);
}

TEST_F(StrategyEngineTest, SignalsEmittedBeforeAThrowStayPublished) {
    // Publication is immediate and cannot be recalled, so a failing callback
    // keeps whatever it had already emitted. Pinned so a change is deliberate.
    const auto a = add("a");
    a->emits          = {signal_for("AAPL")};
    a->throw_on_event = true;
    engine.start();

    publish_market(market_event("AAPL"));

    EXPECT_EQ(bus.signals().size(), 1u);
    EXPECT_EQ(engine.stats().signals_published, 1u);
    EXPECT_EQ(engine.stats().strategy_errors, 1u);
}

TEST_F(StrategyEngineTest, FailuresAreAttributedToTheStrategyThatCausedThem) {
    add("a");
    const auto b = add("b");
    add("c");
    b->throw_on_event = true;
    engine.start();

    publish_market(market_event("AAPL"));

    const auto stats = engine.strategy_stats();
    ASSERT_EQ(stats.size(), 3u);
    for (const auto& each : stats) {
        EXPECT_EQ(each.events_delivered, 1u) << each.id << " (b's failing call still counts as delivered)";
    }
    EXPECT_EQ(stats[0].errors, 0u);
    EXPECT_TRUE(stats[0].last_error.empty());
    EXPECT_EQ(stats[1].id, "b");
    EXPECT_EQ(stats[1].errors, 1u);
    EXPECT_EQ(stats[1].last_error, "on_market_event: b: on_market_event failed");
    EXPECT_EQ(stats[2].errors, 0u);
}

TEST_F(StrategyEngineTest, AnEventFedBackByASynchronousBusHandlerIsDroppedNotNested) {
    // A synchronous bus runs Signal subscribers inside publish(), which is inside
    // the strategy's callback. One that feeds the engine another event would
    // nest a second callback inside the first, which the single-threaded model
    // promises strategies never see.
    const auto a = add("a");
    a->emits = {signal_for("AAPL")};
    bus.subscribe(events::EventType::Signal, [this](const events::Event&) {
        engine.on_market_event(market_event("NESTED"));
    });
    engine.start();

    publish_market(market_event("OUTER"));

    ASSERT_EQ(a->received.size(), 1u);
    EXPECT_EQ(a->received[0].symbol, "OUTER");
    EXPECT_EQ(engine.stats().events_dropped, 1u);

    // Only the nested delivery was refused; the guard does not stick.
    publish_market(market_event("NEXT"));
    ASSERT_EQ(a->received.size(), 2u);
    EXPECT_EQ(a->received[1].symbol, "NEXT");
}

TEST_F(StrategyEngineTest, AMarketDataEventCarryingTheWrongPayloadIsDroppedAndCounted) {
    const auto a = add("a");
    engine.start();

    events::Event bogus;
    bogus.type    = events::EventType::MarketData;
    bogus.payload = domain::Order{};   // a producer broke the MarketData contract

    EXPECT_NO_THROW(bus.publish(std::move(bogus)));

    EXPECT_TRUE(a->received.empty());
    EXPECT_EQ(engine.stats().events_dropped, 1u);
    EXPECT_EQ(engine.stats().events_routed, 0u);
}
