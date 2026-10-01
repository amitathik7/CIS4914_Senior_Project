// StrategyEngine lifetime: destruction, and bus handlers that outlive the
// engine's interest in them. The bus holds a std::function it was given; if
// that function pointed at the engine, a bus that is slow or careless about
// dropping it would call into freed memory. These tests make the double behave
// that way (Faults::ignore_unsubscribe) and check the engine stays safe and
// silent. Run under AddressSanitizer they also prove no freed memory is touched.

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "strategy_engine_fixture.hpp"

namespace {

// For the tests that need to control exactly when the engine dies, so they own
// their bus and clock instead of using the fixture's.
struct Rig {
    support::RecordingEventBus bus;
    common::ManualClock        clock{kT0};
    support::CallLog           calls;

    std::shared_ptr<support::FakeStrategy> strategy(std::string id) {
        return std::make_shared<support::FakeStrategy>(std::move(id), &calls);
    }
    std::size_t market_subscriptions() const {
        return bus.subscription_count(events::EventType::MarketData);
    }
    void publish_market(domain::MarketEvent event) {
        events::Event envelope;
        envelope.type    = events::EventType::MarketData;
        envelope.payload = std::move(event);
        bus.publish(std::move(envelope));
    }
};

}  // namespace

TEST(StrategyEngineLifetime, DestroyingARunningEngineStopsItCleanly) {
    Rig rig;
    const auto a = rig.strategy("a");
    {
        strategy::StrategyEngine engine{rig.bus, rig.clock};
        engine.register_strategy(a);
        engine.start();
        ASSERT_EQ(rig.market_subscriptions(), 1u);
    }

    EXPECT_EQ(a->stops, 1);
    EXPECT_EQ(rig.bus.unsubscribe_calls(), 1u);
    EXPECT_EQ(rig.market_subscriptions(), 0u) << "no handler may be left behind on the bus";
}

TEST(StrategyEngineLifetime, DestroyingAnEngineThatNeverStartedTouchesNeitherStrategiesNorBus) {
    Rig rig;
    const auto a = rig.strategy("a");
    {
        strategy::StrategyEngine engine{rig.bus, rig.clock};
        engine.register_strategy(a);
    }

    EXPECT_EQ(a->starts, 0);
    EXPECT_EQ(a->stops, 0);
    EXPECT_EQ(rig.bus.subscribe_calls(), 0u);
    EXPECT_EQ(rig.bus.unsubscribe_calls(), 0u);
}

TEST(StrategyEngineLifetime, DestroyingAnEngineThatAlreadyStoppedDoesNotStopItAgain) {
    Rig rig;
    const auto a = rig.strategy("a");
    {
        strategy::StrategyEngine engine{rig.bus, rig.clock};
        engine.register_strategy(a);
        engine.start();
        engine.stop();
    }

    EXPECT_EQ(a->stops, 1);
    EXPECT_EQ(rig.bus.unsubscribe_calls(), 1u);
}

TEST(StrategyEngineLifetime, TheDestructorAbsorbsEveryShutdownFailure) {
    // A destructor that throws terminates the process, so merely surviving this
    // is the assertion.
    Rig rig;
    const auto a = rig.strategy("a");
    a->throw_on_stop = true;
    {
        strategy::StrategyEngine engine{rig.bus, rig.clock};
        engine.register_strategy(a);
        engine.start();
        rig.bus.faults.throw_on_unsubscribe = true;
    }

    EXPECT_EQ(a->stops, 1);
}

TEST(StrategyEngineLifetime, AHandlerTheBusKeepsIsInertOnceTheEngineIsDestroyed) {
    Rig rig;
    rig.bus.faults.ignore_unsubscribe = true;   // the bus "forgets" to drop the handler
    const auto a = rig.strategy("a");
    {
        strategy::StrategyEngine engine{rig.bus, rig.clock};
        engine.register_strategy(a);
        engine.start();
    }
    ASSERT_EQ(rig.market_subscriptions(), 1u) << "the stale handler is still on the bus: the hazard";

    // If the handler still pointed at the destroyed engine this would be a
    // use-after-free. It must reach nothing.
    EXPECT_NO_THROW(rig.publish_market(market_event("AAPL")));
    EXPECT_TRUE(a->received.empty());
}

TEST_F(StrategyEngineTest, AHandlerTheBusKeepsIsInertOnceTheEngineIsStopped) {
    bus.faults.ignore_unsubscribe = true;
    const auto a = add("a");
    a->emits = {signal_for("AAPL")};
    engine.start();
    engine.stop();
    ASSERT_EQ(market_subscriptions(), 1u);

    publish_market(market_event("AAPL"));

    EXPECT_TRUE(a->received.empty());
    EXPECT_TRUE(bus.signals().empty());
    EXPECT_EQ(engine.stats().events_dropped, 0u) << "the stale handler must not even reach the engine";
}

TEST_F(StrategyEngineTest, AStaleHandlerIsNotRevivedByARestartAndNeverDeliversTwice) {
    bus.faults.ignore_unsubscribe = true;
    const auto a = add("a");
    engine.start();
    engine.stop();
    engine.start();
    ASSERT_EQ(market_subscriptions(), 2u) << "one stale handler plus the live one";

    publish_market(market_event("AAPL"));

    EXPECT_EQ(a->received.size(), 1u) << "exactly one callback per event, however many handlers the bus holds";
    EXPECT_EQ(engine.stats().events_routed, 1u);
}
