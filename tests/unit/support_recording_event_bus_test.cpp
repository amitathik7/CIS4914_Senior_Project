// The RecordingEventBus double is test infrastructure, but the StrategyEngine
// tests are only as trustworthy as it is: a double that quietly delivered twice
// or skipped a subscriber would make those tests pass or fail for the wrong
// reason. So its own contract is pinned here.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "support/recording_event_bus.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/events/event_bus.hpp"

namespace common  = trading_engine::common;
namespace domain  = trading_engine::domain;
namespace events  = trading_engine::events;
namespace support = trading_engine::test_support;

namespace {

using Trace = std::vector<std::string>;

events::Event market(const std::string& symbol) {
    domain::MarketEvent payload;
    payload.symbol = symbol;
    events::Event event;
    event.type    = events::EventType::MarketData;
    event.payload = payload;
    return event;
}

events::Event signal_event(const std::string& symbol) {
    domain::TradeSignal payload;
    payload.symbol = symbol;
    events::Event event;
    event.type    = events::EventType::Signal;
    event.payload = payload;
    return event;
}

const std::string& symbol_of(const events::Event& event) {
    return std::get<domain::MarketEvent>(event.payload).symbol;
}

}  // namespace

TEST(RecordingEventBus, DeliversSynchronouslyToMatchingSubscribersInSubscriptionOrder) {
    support::RecordingEventBus bus;
    Trace trace;
    bus.subscribe(events::EventType::MarketData,
                  [&](const events::Event& e) { trace.push_back("one:" + symbol_of(e)); });
    bus.subscribe(events::EventType::MarketData,
                  [&](const events::Event& e) { trace.push_back("two:" + symbol_of(e)); });

    EXPECT_TRUE(bus.publish(market("AAPL")));

    // Already delivered when publish() returned: there is no queue to drain.
    EXPECT_EQ(trace, (Trace{"one:AAPL", "two:AAPL"}));
    EXPECT_EQ(bus.depth(), 0u);
}

TEST(RecordingEventBus, DoesNotDeliverToSubscribersOfOtherTypes) {
    support::RecordingEventBus bus;
    int signal_deliveries = 0;
    bus.subscribe(events::EventType::Signal, [&](const events::Event&) { ++signal_deliveries; });

    bus.publish(market("AAPL"));
    EXPECT_EQ(signal_deliveries, 0);

    bus.publish(signal_event("AAPL"));
    EXPECT_EQ(signal_deliveries, 1);
}

TEST(RecordingEventBus, AssignsIncreasingSequenceNumbersAndLeavesEnqueuedAtAlone) {
    support::RecordingEventBus bus;

    bus.publish(market("A"));
    bus.publish(market("B"));

    ASSERT_EQ(bus.published().size(), 2u);
    EXPECT_EQ(bus.published()[0].sequence, 1u);
    EXPECT_EQ(bus.published()[1].sequence, 2u);
    EXPECT_EQ(bus.published()[0].enqueued_at, common::Timestamp{}) << "the double has no clock";
}

TEST(RecordingEventBus, SubscriptionIdsAreNonzeroAndDistinct) {
    support::RecordingEventBus bus;
    const auto first  = bus.subscribe(events::EventType::MarketData, [](const events::Event&) {});
    const auto second = bus.subscribe(events::EventType::MarketData, [](const events::Event&) {});

    EXPECT_TRUE(first.valid());
    EXPECT_TRUE(second.valid());
    EXPECT_NE(first, second);
}

TEST(RecordingEventBus, UnsubscribeStopsDeliveryAndToleratesUnknownIds) {
    support::RecordingEventBus bus;
    int deliveries = 0;
    const auto id = bus.subscribe(events::EventType::MarketData,
                                  [&](const events::Event&) { ++deliveries; });
    bus.publish(market("AAPL"));
    ASSERT_EQ(deliveries, 1);

    bus.unsubscribe(id);
    bus.unsubscribe(id);                          // already gone
    bus.unsubscribe(common::SubscriptionId{99});  // never existed
    bus.publish(market("AAPL"));

    EXPECT_EQ(deliveries, 1);
    EXPECT_EQ(bus.subscription_count(events::EventType::MarketData), 0u);
    EXPECT_EQ(bus.unsubscribe_calls(), 3u);
}

TEST(RecordingEventBus, HandlersMayPublishAndUnsubscribeDuringDelivery) {
    support::RecordingEventBus bus;
    Trace trace;
    common::SubscriptionId second{};

    bus.subscribe(events::EventType::MarketData, [&](const events::Event& e) {
        trace.push_back("first:" + symbol_of(e));
        bus.unsubscribe(second);   // drops a later subscriber mid-delivery
        if (symbol_of(e) == "OUTER") {
            bus.publish(market("INNER"));   // and publishes again, re-entrantly
        }
    });
    second = bus.subscribe(events::EventType::MarketData, [&](const events::Event& e) {
        trace.push_back("second:" + symbol_of(e));
    });

    bus.publish(market("OUTER"));

    // `second` was unsubscribed before its turn, so it never ran, even though it
    // was subscribed when the OUTER delivery began.
    EXPECT_EQ(trace, (Trace{"first:OUTER", "first:INNER"}));
    ASSERT_EQ(bus.published().size(), 2u);
    EXPECT_EQ(symbol_of(bus.published()[0]), "OUTER");
    EXPECT_EQ(symbol_of(bus.published()[1]), "INNER");
}

TEST(RecordingEventBus, RejectFaultRefusesRecordsAndDoesNotDeliver) {
    support::RecordingEventBus bus;
    int deliveries = 0;
    bus.subscribe(events::EventType::MarketData, [&](const events::Event&) { ++deliveries; });
    bus.faults.reject_publish = true;

    EXPECT_FALSE(bus.publish(market("AAPL")));

    EXPECT_EQ(deliveries, 0);
    EXPECT_TRUE(bus.published().empty());
    ASSERT_EQ(bus.rejected().size(), 1u);
    EXPECT_EQ(symbol_of(bus.rejected()[0]), "AAPL");

    bus.faults.reject_publish = false;
    EXPECT_TRUE(bus.publish(market("AAPL")));
    EXPECT_EQ(deliveries, 1);
}

TEST(RecordingEventBus, ThrowFaultsThrowFromTheMatchingCallAndChangeNothing) {
    support::RecordingEventBus bus;

    bus.faults.throw_on_publish = true;
    EXPECT_THROW(bus.publish(market("AAPL")), std::runtime_error);
    EXPECT_TRUE(bus.published().empty());

    bus.faults = {};
    bus.faults.throw_on_subscribe = true;
    EXPECT_THROW(bus.subscribe(events::EventType::MarketData, [](const events::Event&) {}),
                 std::runtime_error);
    EXPECT_EQ(bus.subscription_count(events::EventType::MarketData), 0u);

    bus.faults = {};
    const auto id = bus.subscribe(events::EventType::MarketData, [](const events::Event&) {});
    bus.faults.throw_on_unsubscribe = true;
    EXPECT_THROW(bus.unsubscribe(id), std::runtime_error);
    EXPECT_EQ(bus.subscription_count(events::EventType::MarketData), 1u);
}

TEST(RecordingEventBus, IgnoreUnsubscribeFaultLeavesTheHandlerLive) {
    support::RecordingEventBus bus;
    int deliveries = 0;
    const auto id = bus.subscribe(events::EventType::MarketData,
                                  [&](const events::Event&) { ++deliveries; });
    bus.faults.ignore_unsubscribe = true;

    EXPECT_NO_THROW(bus.unsubscribe(id));
    bus.publish(market("AAPL"));

    EXPECT_EQ(deliveries, 1) << "a bus that fails to drop a handler keeps calling it";
    EXPECT_EQ(bus.subscription_count(events::EventType::MarketData), 1u);
}

TEST(RecordingEventBus, LifecycleCallsAreOnlyCounted) {
    support::RecordingEventBus bus;
    EXPECT_EQ(bus.lifecycle_calls(), 0u);

    bus.start();
    bus.request_shutdown();
    bus.wait_until_drained();

    EXPECT_EQ(bus.lifecycle_calls(), 3u);
    EXPECT_TRUE(bus.publish(market("AAPL"))) << "shutdown is not modelled; use faults.reject_publish";
}

TEST(RecordingEventBus, SignalsReturnsTheAcceptedSignalPayloadsInOrder) {
    support::RecordingEventBus bus;

    bus.publish(market("IGNORED"));
    bus.publish(signal_event("FIRST"));
    bus.faults.reject_publish = true;
    bus.publish(signal_event("REFUSED"));
    bus.faults.reject_publish = false;
    bus.publish(signal_event("SECOND"));

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 2u);
    EXPECT_EQ(signals[0].symbol, "FIRST");
    EXPECT_EQ(signals[1].symbol, "SECOND");
}
