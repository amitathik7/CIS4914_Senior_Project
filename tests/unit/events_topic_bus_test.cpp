// TopicEventBus over the in-memory broker: the Kafka code path minus Kafka.
// The same tests run against librdkafka's mock cluster in kafka_transport_test.cpp.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/events/event_codec.hpp"
#include "trading_engine/events/message_transport.hpp"
#include "trading_engine/events/topic_event_bus.hpp"

namespace {

namespace ev  = trading_engine::events;
namespace dom = trading_engine::domain;
namespace tec = trading_engine::common;
using namespace std::chrono_literals;

ev::Event bar(const std::string& symbol, std::uint64_t n = 0) {
    dom::MarketEvent e;
    e.symbol   = symbol;
    e.type     = dom::MarketEventType::Bar;
    e.sequence = n;
    ev::Event out;
    out.type    = ev::EventType::MarketData;
    out.payload = e;
    return out;
}

ev::Event signal(const std::string& symbol) {
    dom::TradeSignal s;
    s.symbol = symbol;
    ev::Event out;
    out.type    = ev::EventType::Signal;
    out.payload = s;
    return out;
}

ev::TopicEventBusOptions fast(std::string origin = {}) {
    ev::TopicEventBusOptions o;
    o.topic_prefix  = "test";
    o.origin        = std::move(origin);
    o.poll_interval = 5ms;
    o.drain_timeout = 5s;
    return o;
}

class TopicBusTest : public ::testing::Test {
protected:
    tec::ManualClock clock_{tec::Timestamp{std::chrono::seconds{1'700'000'000}}};
    std::shared_ptr<ev::InMemoryBroker> broker_ = std::make_shared<ev::InMemoryBroker>();
};

TEST_F(TopicBusTest, PublishesOneKeyedJsonMessagePerEventOnAPerTypeTopic) {
    ev::InMemoryTransport transport{broker_};
    ev::TopicEventBus bus{transport, clock_, fast("me")};
    EXPECT_EQ(bus.topic(ev::EventType::MarketData), "test.market_data");
    EXPECT_EQ(bus.topic(ev::EventType::Fill), "test.fill");
    bus.start();
    ASSERT_TRUE(bus.publish(bar("AAPL")));
    ASSERT_TRUE(bus.publish(signal("MSFT")));
    bus.request_shutdown();
    bus.wait_until_drained();

    const auto bars = broker_->messages("test.market_data");
    ASSERT_EQ(bars.size(), 1U);
    EXPECT_EQ(bars[0].key, "AAPL");
    const ev::WireEnvelope decoded = ev::decode(bars[0].payload);
    EXPECT_EQ(decoded.origin, "me");
    EXPECT_EQ(decoded.event.sequence, 1U);
    EXPECT_EQ(decoded.event.enqueued_at, clock_.now());
    ASSERT_EQ(broker_->messages("test.signal").size(), 1U);
    EXPECT_EQ(broker_->messages("test.signal")[0].key, "MSFT");
}

TEST_F(TopicBusTest, LocalSubscribersReceiveEventsBackThroughTheBroker) {
    ev::InMemoryTransport transport{broker_};
    ev::TopicEventBus bus{transport, clock_, fast()};
    std::vector<ev::Event> seen;   // dispatcher only
    std::thread::id handler_thread;
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        seen.push_back(e);
        handler_thread = std::this_thread::get_id();
    });
    bus.start();
    for (std::uint64_t i = 1; i <= 100; ++i) {
        ASSERT_TRUE(bus.publish(bar("AAPL", i)));
    }
    bus.request_shutdown();
    bus.wait_until_drained();

    ASSERT_EQ(seen.size(), 100U);
    for (std::uint64_t i = 0; i < 100; ++i) {
        EXPECT_EQ(seen[i].sequence, i + 1);
        EXPECT_EQ(std::get<dom::MarketEvent>(seen[i].payload).sequence, i + 1);
    }
    EXPECT_NE(handler_thread, std::this_thread::get_id());
    EXPECT_EQ(bus.stats().received_own, 100U);
    EXPECT_EQ(bus.stats().drain_timeouts, 0U);
    EXPECT_EQ(bus.depth(), 0U);
}

TEST_F(TopicBusTest, TwoProcessesSharingABrokerSeeEachOthersEvents) {
    // "strategy process" consumes bars and publishes signals; "risk process"
    // publishes bars and consumes signals. Neither knows the other exists.
    ev::InMemoryTransport risk_side{broker_};
    ev::InMemoryTransport strategy_side{broker_};
    ev::TopicEventBus risk_bus{risk_side, clock_, fast("risk")};
    ev::TopicEventBus strategy_bus{strategy_side, clock_, fast("strategy")};

    strategy_bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        strategy_bus.publish(signal(std::get<dom::MarketEvent>(e.payload).symbol));
    });
    std::mutex m;
    std::vector<std::string> signals;
    std::latch three{3};
    risk_bus.subscribe(ev::EventType::Signal, [&](const ev::Event& e) {
        const std::lock_guard lock{m};
        signals.push_back(std::get<dom::TradeSignal>(e.payload).symbol);
        three.count_down();
    });
    strategy_bus.start();
    risk_bus.start();
    risk_bus.publish(bar("AAPL"));
    risk_bus.publish(bar("MSFT"));
    risk_bus.publish(bar("AAPL"));
    three.wait();

    strategy_bus.request_shutdown();
    strategy_bus.wait_until_drained();
    risk_bus.request_shutdown();
    risk_bus.wait_until_drained();
    EXPECT_EQ(signals, (std::vector<std::string>{"AAPL", "MSFT", "AAPL"}));
    EXPECT_EQ(risk_bus.stats().received, 6U);   // its 3 bars + 3 signals from the other side
    EXPECT_EQ(risk_bus.stats().received_own, 3U);
}

TEST_F(TopicBusTest, NestedPublishDuringDrainIsAcceptedAndDelivered) {
    ev::InMemoryTransport transport{broker_};
    ev::TopicEventBus bus{transport, clock_, fast()};
    std::atomic<int> signals{0};
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        EXPECT_TRUE(bus.publish(signal(std::get<dom::MarketEvent>(e.payload).symbol)));
    });
    bus.subscribe(ev::EventType::Signal, [&](const ev::Event&) { ++signals; });
    bus.start();
    for (int i = 0; i < 25; ++i) {
        bus.publish(bar("A"));
    }
    bus.request_shutdown();
    EXPECT_FALSE(bus.publish(bar("late")));   // outside the dispatcher: refused
    bus.wait_until_drained();
    EXPECT_EQ(signals.load(), 25);
    EXPECT_EQ(bus.stats().rejected, 1U);
}

TEST_F(TopicBusTest, ATransportRefusalIsAFalsePublishAndConsumesNoSequence) {
    ev::InMemoryTransport transport{broker_};
    ev::TopicEventBus bus{transport, clock_, fast()};
    std::vector<std::uint64_t> sequences;
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) { sequences.push_back(e.sequence); });
    bus.start();
    EXPECT_TRUE(bus.publish(bar("A")));
    transport.refuse_sends(true);
    EXPECT_FALSE(bus.publish(bar("A")));
    transport.refuse_sends(false);
    EXPECT_TRUE(bus.publish(bar("A")));
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_EQ(sequences, (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(bus.stats().rejected, 1U);
}

TEST_F(TopicBusTest, UnreadableMessagesAreCountedAndSkipped) {
    ev::InMemoryTransport transport{broker_};
    ev::InMemoryTransport vandal{broker_};
    ev::TopicEventBus bus{transport, clock_, fast()};
    std::atomic<int> delivered{0};
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) { ++delivered; });
    bus.start();
    vandal.send({"test.market_data", "AAPL", "{not json"});
    vandal.send({"test.market_data", "AAPL", "{\"schema\":99}"});
    bus.publish(bar("AAPL"));
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_EQ(delivered.load(), 1);
    EXPECT_EQ(bus.stats().decode_errors, 2U);
}

TEST_F(TopicBusTest, ASubscriberStartsAtTheEndOfTheTopicUnlessAskedToReplay) {
    ev::InMemoryTransport early{broker_};
    early.send({"test.market_data", "A", ev::encode({"old", bar("A")})});

    ev::InMemoryTransport latest{broker_};
    ev::InMemoryTransport replay{broker_, /*from_beginning=*/true};
    ev::TopicEventBus latest_bus{latest, clock_, fast()};
    ev::TopicEventBus replay_bus{replay, clock_, fast()};
    std::atomic<int> latest_seen{0};
    std::atomic<int> replay_seen{0};
    latest_bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) { ++latest_seen; });
    replay_bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) { ++replay_seen; });
    latest_bus.start();
    replay_bus.start();
    latest_bus.publish(bar("B"));
    latest_bus.request_shutdown();
    latest_bus.wait_until_drained();
    replay_bus.request_shutdown();
    // replay_bus has nothing of its own to wait for; give it a moment to read both.
    for (int i = 0; i < 200 && replay_seen.load() < 2; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    replay_bus.wait_until_drained();
    EXPECT_EQ(latest_seen.load(), 1);
    EXPECT_EQ(replay_seen.load(), 2);
}

TEST_F(TopicBusTest, UnsubscribeIsABarrierHereToo) {
    ev::InMemoryTransport transport{broker_};
    ev::TopicEventBus bus{transport, clock_, fast()};
    std::latch entered{1};
    std::atomic<bool> finished{false};
    const auto id = bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) {
        entered.count_down();
        std::this_thread::sleep_for(100ms);
        finished = true;
    });
    bus.start();
    bus.publish(bar("A"));
    entered.wait();
    bus.unsubscribe(id);
    EXPECT_TRUE(finished.load());
}

TEST_F(TopicBusTest, LifecycleAndConfigurationMisuseIsLoud) {
    ev::InMemoryTransport transport{broker_};
    EXPECT_THROW((ev::TopicEventBus{transport, clock_, {.topic_prefix = "has space"}}),
                 std::invalid_argument);
    EXPECT_THROW((ev::TopicEventBus{transport, clock_, {.topic_prefix = ""}}), std::invalid_argument);

    ev::TopicEventBus bus{transport, clock_, fast()};
    EXPECT_FALSE(bus.publish(bar("A")));   // not started
    EXPECT_FALSE(bus.origin().empty());    // a random origin was generated
    bus.start();
    ev::Event mismatched = bar("A");
    mismatched.type      = ev::EventType::Fill;
    EXPECT_THROW(bus.publish(mismatched), std::invalid_argument);
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_THROW(bus.start(), std::logic_error);
}

TEST_F(TopicBusTest, DistinctBusesGetDistinctOrigins) {
    ev::InMemoryTransport a{broker_};
    ev::InMemoryTransport b{broker_};
    const ev::TopicEventBus first{a, clock_};
    const ev::TopicEventBus second{b, clock_};
    EXPECT_NE(first.origin(), second.origin());
}

}  // namespace
