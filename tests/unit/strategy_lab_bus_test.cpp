// The lab's SynchronousDemoBus against the bus assumptions the StrategyEngine documents
// (docs/STRATEGIES.md section 4): the lifecycle owned by the composition root, serialized
// delivery, a handler that publishes, unsubscribe as a barrier, a subscribe that throws
// leaving nothing behind. And the real StrategyEngine running on top of it.
//
// It is NOT the production queue and these tests do not say it is: nothing here is
// asynchronous, bounded or threaded.

#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "lab/demo_bus.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

namespace {

using namespace std::chrono_literals;
using trading_engine::lab::SynchronousDemoBus;
namespace common   = trading_engine::common;
namespace domain   = trading_engine::domain;
namespace events   = trading_engine::events;
namespace strategy = trading_engine::strategy;
using events::EventType;

const common::Timestamp kStart = common::Timestamp{} + std::chrono::hours{7};

events::Event market(const std::string& symbol, double price) {
    events::Event event;
    event.type = EventType::MarketData;
    domain::MarketEvent payload;
    payload.symbol = symbol;
    payload.type   = domain::MarketEventType::Bar;
    payload.price  = price;
    event.payload  = payload;
    return event;
}

events::Event signal(const std::string& symbol) {
    events::Event event;
    event.type = EventType::Signal;
    domain::TradeSignal payload;
    payload.symbol = symbol;
    event.payload  = payload;
    return event;
}

class DemoBusTest : public ::testing::Test {
protected:
    common::ManualClock clock{kStart};
    SynchronousDemoBus  bus{clock};
};

// ---- lifecycle, stamping, ordering -------------------------------------------------------

TEST_F(DemoBusTest, RefusesPublicationUntilStartedAndAfterShutdown) {
    EXPECT_FALSE(bus.running());
    EXPECT_FALSE(bus.publish(market("AAPL", 1.0))) << "not started";
    EXPECT_EQ(bus.counters().rejected_not_running, 1u);

    bus.start();
    EXPECT_TRUE(bus.running());
    EXPECT_TRUE(bus.publish(market("AAPL", 1.0)));

    bus.request_shutdown();
    EXPECT_FALSE(bus.running());
    EXPECT_FALSE(bus.publish(market("AAPL", 1.0))) << "a bus that stopped accepting events says so";
    EXPECT_EQ(bus.counters().rejected_not_running, 2u);
    EXPECT_NO_THROW(bus.wait_until_drained()) << "nothing is queued: nothing to wait for";
    EXPECT_THROW(bus.start(), std::logic_error) << "a shut-down bus cannot be restarted";
    EXPECT_EQ(bus.depth(), 0u) << "synchronous: never a queue";
}

TEST_F(DemoBusTest, StampsASequenceAcrossAllTypesAndTheInjectedClockTime) {
    bus.start();
    std::vector<events::Event> seen;
    bus.subscribe(EventType::MarketData, [&](const events::Event& e) { seen.push_back(e); });
    bus.subscribe(EventType::Signal, [&](const events::Event& e) { seen.push_back(e); });

    clock.set(kStart + 5min);
    bus.publish(market("AAPL", 1.0));
    clock.set(kStart + 6min);
    bus.publish(signal("AAPL"));
    bus.publish(market("MSFT", 2.0));

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].sequence, 1u);
    EXPECT_EQ(seen[1].sequence, 2u) << "one counter for every event type";
    EXPECT_EQ(seen[2].sequence, 3u);
    EXPECT_EQ(seen[0].enqueued_at, kStart + 5min) << "set by the bus from the injected clock";
    EXPECT_EQ(seen[1].enqueued_at, kStart + 6min);
    EXPECT_EQ(bus.last_sequence(), 3u);
    EXPECT_EQ(bus.counters().published_by_type[static_cast<std::size_t>(EventType::MarketData)], 2u);
    EXPECT_EQ(bus.counters().published_by_type[static_cast<std::size_t>(EventType::Signal)], 1u);
}

TEST_F(DemoBusTest, DeliversOnlyToSubscribersOfThatTypeInSubscriptionOrder) {
    bus.start();
    std::vector<int> order;
    bus.subscribe(EventType::MarketData, [&](const events::Event&) { order.push_back(1); });
    bus.subscribe(EventType::Signal, [&](const events::Event&) { order.push_back(99); });
    bus.subscribe(EventType::MarketData, [&](const events::Event&) { order.push_back(2); });
    bus.publish(market("AAPL", 1.0));
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_EQ(bus.subscription_count(EventType::MarketData), 2u);
    EXPECT_EQ(bus.subscription_count(EventType::Fill), 0u);
}

// ---- reentrancy ------------------------------------------------------------------------------

TEST_F(DemoBusTest, AHandlerMayPublishAndTheNestedDeliveryFinishesBeforeTheOuterPublishReturns) {
    // The engine publishes Signal events from inside its MarketData handler.
    bus.start();
    std::vector<std::string> log;
    bus.subscribe(EventType::MarketData, [&](const events::Event&) {
        log.push_back("market:begin");
        EXPECT_TRUE(bus.publish(signal("AAPL")));
        log.push_back("market:end");
    });
    bus.subscribe(EventType::Signal,
                  [&](const events::Event& e) { log.push_back("signal seq " + std::to_string(e.sequence)); });
    EXPECT_TRUE(bus.publish(market("AAPL", 1.0)));
    EXPECT_EQ(log, (std::vector<std::string>{"market:begin", "signal seq 2", "market:end"}))
        << "depth first: the market event took sequence 1 before its handler ran";
}

TEST_F(DemoBusTest, RunawayRecursionBecomesAnErrorAndTheBusRecovers) {
    bus.start();
    std::size_t depth_reached = 0;
    const auto id = bus.subscribe(EventType::MarketData, [&](const events::Event&) {
        ++depth_reached;
        bus.publish(market("AAPL", 1.0));   // publishes itself forever
    });
    EXPECT_THROW(bus.publish(market("AAPL", 1.0)), std::logic_error);
    EXPECT_EQ(depth_reached, SynchronousDemoBus::kMaxDepth);

    bus.unsubscribe(id);
    EXPECT_TRUE(bus.publish(market("AAPL", 1.0))) << "the nesting counter unwound with the exception";
}

// ---- unsubscribe is a barrier; subscribe during delivery ---------------------------------------

TEST_F(DemoBusTest, AHandlerUnsubscribedByAnEarlierHandlerIsSkippedInThatDeliveryAndAfter) {
    bus.start();
    common::SubscriptionId victim{};
    int first_calls = 0;
    int victim_calls = 0;
    bus.subscribe(EventType::MarketData, [&](const events::Event&) {
        ++first_calls;
        bus.unsubscribe(victim);
    });
    victim = bus.subscribe(EventType::MarketData, [&](const events::Event&) { ++victim_calls; });

    bus.publish(market("AAPL", 1.0));
    bus.publish(market("AAPL", 2.0));
    EXPECT_EQ(first_calls, 2);
    EXPECT_EQ(victim_calls, 0) << "once unsubscribe returns, the handler never starts again";
    EXPECT_EQ(bus.subscription_count(EventType::MarketData), 1u);
}

TEST_F(DemoBusTest, AHandlerMayUnsubscribeItself) {
    bus.start();
    common::SubscriptionId self{};
    int calls = 0;
    self = bus.subscribe(EventType::MarketData, [&](const events::Event&) {
        ++calls;
        bus.unsubscribe(self);
    });
    bus.publish(market("AAPL", 1.0));
    bus.publish(market("AAPL", 2.0));
    EXPECT_EQ(calls, 1);
}

TEST_F(DemoBusTest, ASubscriptionMadeDuringADeliveryHearsOnlyLaterEvents) {
    bus.start();
    int late_calls = 0;
    bool subscribed = false;
    bus.subscribe(EventType::MarketData, [&](const events::Event&) {
        if (!subscribed) {
            subscribed = true;
            bus.subscribe(EventType::MarketData, [&](const events::Event&) { ++late_calls; });
        }
    });
    bus.publish(market("AAPL", 1.0));
    EXPECT_EQ(late_calls, 0) << "not the event in flight";
    bus.publish(market("AAPL", 2.0));
    EXPECT_EQ(late_calls, 1);
}

TEST_F(DemoBusTest, ASubscribeThatIsRefusedLeavesNoSubscriptionBehind) {
    EXPECT_THROW(bus.subscribe(EventType::MarketData, events::EventHandler{}), std::invalid_argument);
    EXPECT_EQ(bus.subscription_count(EventType::MarketData), 0u);
}

TEST_F(DemoBusTest, AnExceptionFromAHandlerReachesThePublisherAndTheBusKeepsWorking) {
    bus.start();
    const auto id = bus.subscribe(EventType::MarketData, [](const events::Event&) { throw std::runtime_error("handler"); });
    EXPECT_THROW(bus.publish(market("AAPL", 1.0)), std::runtime_error);
    bus.unsubscribe(id);
    EXPECT_TRUE(bus.publish(market("AAPL", 2.0)));
    EXPECT_EQ(bus.last_sequence(), 2u);
}

// ---- serialized delivery ------------------------------------------------------------------------

TEST_F(DemoBusTest, UseFromASecondThreadIsRefusedLoudlyInsteadOfRacing) {
    bus.start();   // this thread becomes the owner
    std::exception_ptr failure;
    std::thread other([&] {
        try {
            bus.publish(market("AAPL", 1.0));
        } catch (...) {
            failure = std::current_exception();
        }
    });
    other.join();
    ASSERT_TRUE(failure != nullptr) << "a second thread must not get through";
    EXPECT_THROW(std::rethrow_exception(failure), std::logic_error);
    EXPECT_TRUE(bus.publish(market("AAPL", 1.0))) << "the owner thread is unaffected";
    EXPECT_EQ(bus.last_sequence(), 1u);
}

// ---- fault injection -----------------------------------------------------------------------------

TEST(DemoBusFaults, RejectSignalsRefusesEverySignalRecordsItAndLeavesMarketDataAlone) {
    common::ManualClock clock{kStart};
    SynchronousDemoBus bus{clock, SynchronousDemoBus::Fault::RejectSignals};
    bus.start();
    int signals_heard = 0;
    bus.subscribe(EventType::Signal, [&](const events::Event&) { ++signals_heard; });

    EXPECT_TRUE(bus.publish(market("AAPL", 1.0)));
    EXPECT_FALSE(bus.publish(signal("AAPL")));
    EXPECT_FALSE(bus.publish(signal("MSFT")));
    EXPECT_EQ(signals_heard, 0) << "a refused signal is not delivered";
    EXPECT_EQ(bus.counters().refused_by_fault, 2u);
    EXPECT_EQ(bus.counters().published_by_type[static_cast<std::size_t>(EventType::Signal)], 0u);
    ASSERT_EQ(bus.failures().size(), 2u);
    EXPECT_EQ(bus.failures()[0].kind, SynchronousDemoBus::FailedPublication::Kind::Rejected);
    EXPECT_EQ(std::get<domain::TradeSignal>(bus.failures()[0].event.payload).symbol, "AAPL");
    EXPECT_EQ(std::get<domain::TradeSignal>(bus.failures()[1].event.payload).symbol, "MSFT");
    EXPECT_EQ(bus.failures()[0].event.sequence, 0u) << "an event that was never accepted has no sequence";
    EXPECT_EQ(bus.last_sequence(), 1u) << "only the market event was accepted";
}

TEST(DemoBusFaults, ThrowOnSignalMakesPublishThrowAndRecordsTheFailure) {
    common::ManualClock clock{kStart};
    SynchronousDemoBus bus{clock, SynchronousDemoBus::Fault::ThrowOnSignal};
    bus.start();
    EXPECT_TRUE(bus.publish(market("AAPL", 1.0)));
    EXPECT_THROW(bus.publish(signal("AAPL")), std::runtime_error);
    ASSERT_EQ(bus.failures().size(), 1u);
    EXPECT_EQ(bus.failures()[0].kind, SynchronousDemoBus::FailedPublication::Kind::Threw);
    EXPECT_NE(bus.failures()[0].message.find("throw-on-signal"), std::string::npos);
}

// ---- the real StrategyEngine on top of it ---------------------------------------------------------

strategy::MovingAverageCrossoverConfig crossover_2_3() {
    strategy::MovingAverageCrossoverConfig config;
    config.short_window = 2;
    config.long_window  = 3;
    config.symbols      = {"AAPL"};
    return config;
}

TEST(DemoBusEngine, TheEngineSubscribesOnStartAndWithdrawsOnStopAndAfterARestart) {
    common::ManualClock clock{kStart};
    SynchronousDemoBus bus{clock};
    {
        strategy::StrategyEngine engine{bus, clock};
        engine.register_strategy(std::make_shared<strategy::MovingAverageCrossoverStrategy>(crossover_2_3()));
        EXPECT_EQ(bus.subscription_count(EventType::MarketData), 0u);
        bus.start();
        engine.start();
        EXPECT_EQ(bus.subscription_count(EventType::MarketData), 1u);
        engine.stop();
        EXPECT_EQ(bus.subscription_count(EventType::MarketData), 0u);
        engine.start();
        EXPECT_EQ(bus.subscription_count(EventType::MarketData), 1u) << "a restart subscribes afresh, once";
    }   // the engine is destroyed while the bus lives on
    EXPECT_EQ(bus.subscription_count(EventType::MarketData), 0u) << "the destructor withdrew it";
}

TEST(DemoBusEngine, SignalsAreStampedWithTheClockAtDeliveryAndPublishedThroughTheBus) {
    common::ManualClock clock{kStart};
    SynchronousDemoBus bus{clock};
    strategy::StrategyEngine engine{bus, clock};
    engine.register_strategy(std::make_shared<strategy::MovingAverageCrossoverStrategy>(crossover_2_3()));

    std::vector<events::Event> signals;
    bus.subscribe(EventType::Signal, [&](const events::Event& e) { signals.push_back(e); });
    bus.start();
    engine.start();

    // closes 3 2 1 2 3: a Buy on the fifth bar (the documented crossover fixture)
    const std::vector<double> closes{3, 2, 1, 2, 3};
    for (std::size_t i = 0; i < closes.size(); ++i) {
        clock.set(kStart + std::chrono::minutes{static_cast<long long>(i) + 1});   // BEFORE delivery
        events::Event event = market("AAPL", closes[i]);
        auto& payload       = std::get<domain::MarketEvent>(event.payload);
        payload.exchange_time = clock.now();
        bus.publish(std::move(event));
    }

    ASSERT_EQ(signals.size(), 1u);
    const auto& published = std::get<domain::TradeSignal>(signals[0].payload);
    EXPECT_EQ(published.side, domain::SignalSide::Buy);
    EXPECT_EQ(published.id.value, 1u);
    EXPECT_EQ(published.created_at, kStart + 5min) << "the clock reading when the signal was emitted";
    EXPECT_EQ(signals[0].enqueued_at, kStart + 5min) << "and the bus stamped its own time from the same clock";
    EXPECT_EQ(signals[0].sequence, 6u) << "five market events take 1..5; the signal published inside the fifth is the sixth event";
}

TEST(DemoBusEngine, AfterShutdownTheEngineReceivesNothingAndStoppingAfterTheDrainIsClean) {
    common::ManualClock clock{kStart};
    SynchronousDemoBus bus{clock};
    strategy::StrategyEngine engine{bus, clock};
    engine.register_strategy(std::make_shared<strategy::MovingAverageCrossoverStrategy>(crossover_2_3()));
    bus.start();
    engine.start();
    EXPECT_TRUE(bus.publish(market("AAPL", 1.0)));
    EXPECT_EQ(engine.stats().events_routed, 1u);

    // The composition root's order: shut the bus, drain it, then stop the engine.
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_FALSE(bus.publish(market("AAPL", 2.0)));
    EXPECT_EQ(engine.stats().events_routed, 1u) << "a refused event never reaches the engine";
    engine.stop();
    EXPECT_EQ(engine.stats().bus_errors, 0u);
}

TEST(DemoBusEngine, AnEngineWhoseUnsubscribeIsRefusedAbsorbsAndCountsTheBusError) {
    // Deliberate misuse: stop() from a second thread. The engine's documented behaviour is to
    // absorb a bus that throws from unsubscribe() and count it.
    common::ManualClock clock{kStart};
    SynchronousDemoBus bus{clock};
    strategy::StrategyEngine engine{bus, clock};
    engine.register_strategy(std::make_shared<strategy::MovingAverageCrossoverStrategy>(crossover_2_3()));
    bus.start();
    engine.start();

    std::thread other([&] { engine.stop(); });
    other.join();
    EXPECT_EQ(engine.stats().bus_errors, 1u);
    EXPECT_FALSE(engine.is_running());
    // The handler the bus still holds is inert once the engine stopped.
    EXPECT_TRUE(bus.publish(market("AAPL", 1.0)));
    EXPECT_EQ(engine.stats().events_routed, 0u);
}

}  // namespace
