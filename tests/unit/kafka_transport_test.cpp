// RdKafkaTransport and TopicEventBus over a real Kafka protocol implementation:
// librdkafka's in-process mock cluster. No broker or Docker needed.
//
// Set TE_KAFKA_BOOTSTRAP=host:port (e.g. the docker-compose broker in
// docs/EVENT_BUS.md) to run the RealBroker tests too; they skip otherwise.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "trading_engine/app/pipeline.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/events/event_codec.hpp"
#include "trading_engine/events/topic_event_bus.hpp"
#include "trading_engine/kafka/rdkafka_transport.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

namespace {

namespace ev  = trading_engine::events;
namespace dom = trading_engine::domain;
namespace tec = trading_engine::common;
namespace kf  = trading_engine::kafka;
namespace st  = trading_engine::strategy;
using namespace std::chrono_literals;

// std::getenv without MSVC's C4996 deprecation warning.
std::string env_or_empty(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out{value};
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::string{};
#endif
}

ev::Event bar(const std::string& symbol, std::uint64_t n, tec::Price price = tec::Price::from_units(1)) {
    dom::MarketEvent e;
    e.symbol        = symbol;
    e.type          = dom::MarketEventType::Bar;
    e.sequence      = n;
    e.price         = price;
    e.exchange_time = tec::Timestamp{std::chrono::minutes{static_cast<long>(n)}};
    ev::Event out;
    out.type    = ev::EventType::MarketData;
    out.payload = e;
    return out;
}

// Topic names unique per test, so tests sharing a real broker never interfere.
std::string unique_prefix(const std::string& stem) {
    std::random_device rd;
    return "te-test." + stem + "." + std::to_string(rd());
}

ev::TopicEventBusOptions bus_options(const std::string& prefix, std::string origin = {}) {
    ev::TopicEventBusOptions o;
    o.topic_prefix  = prefix;
    o.origin        = std::move(origin);
    o.poll_interval = 20ms;
    o.drain_timeout = 20s;
    return o;
}

kf::KafkaTransportConfig transport_config(const std::string& bootstrap, const std::string& client) {
    kf::KafkaTransportConfig c;
    c.bootstrap_servers = bootstrap;
    c.client_id         = client;
    c.partitions        = 3;   // several partitions: per-symbol ordering must still hold
    c.timeout           = 20s;
    return c;
}

class KafkaMockTest : public ::testing::Test {
protected:
    kf::KafkaMockCluster cluster_{1};
    tec::ManualClock clock_{tec::Timestamp{std::chrono::seconds{1'700'000'000}}};

    // The mock cluster does not implement the CreateTopics admin request, so
    // topics are made on the mock directly and the transport is told not to.
    kf::KafkaTransportConfig mock_config(const std::string& client) {
        kf::KafkaTransportConfig c = transport_config(cluster_.bootstrap_servers(), client);
        c.create_topics            = false;
        return c;
    }
    std::string mock_prefix(const std::string& stem) {
        const std::string prefix = unique_prefix(stem);
        for (const auto type : {ev::EventType::MarketData, ev::EventType::Signal,
                                ev::EventType::Order, ev::EventType::Fill}) {
            cluster_.create_topic(prefix + "." + std::string(ev::to_string(type)), 3);
        }
        return prefix;
    }
};

TEST_F(KafkaMockTest, TransportRoundTripsKeyedMessages) {
    const std::string topic = mock_prefix("raw") + ".market_data";
    kf::RdKafkaTransport transport{mock_config("raw")};
    transport.subscribe({topic});
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(transport.send({topic, "AAPL", "payload-" + std::to_string(i)}));
    }
    ASSERT_TRUE(transport.flush(10s));

    std::vector<std::string> got;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (got.size() < 10 && std::chrono::steady_clock::now() < deadline) {
        if (auto m = transport.poll(100ms)) {
            EXPECT_EQ(m->topic, topic);
            EXPECT_EQ(m->key, "AAPL");
            got.push_back(m->payload);
        }
    }
    ASSERT_EQ(got.size(), 10U);
    for (std::size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(got[i], "payload-" + std::to_string(i)) << "one key stays in order";
    }
    EXPECT_EQ(transport.stats().delivery_failures, 0U);
}

TEST_F(KafkaMockTest, TopicBusDeliversThroughKafkaWithPerSymbolOrdering) {
    const std::string prefix = mock_prefix("bus");
    kf::RdKafkaTransport transport{mock_config("bus")};
    ev::TopicEventBus bus{transport, clock_, bus_options(prefix)};

    std::map<std::string, std::vector<std::uint64_t>> per_symbol;   // dispatcher only
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        const auto& m = std::get<dom::MarketEvent>(e.payload);
        per_symbol[m.symbol].push_back(m.sequence);
    });
    bus.start();
    for (std::uint64_t i = 1; i <= 50; ++i) {
        for (const char* symbol : {"AAPL", "MSFT", "NVDA", "TSLA"}) {
            ASSERT_TRUE(bus.publish(bar(symbol, i)));
        }
    }
    bus.request_shutdown();
    bus.wait_until_drained();

    EXPECT_EQ(bus.stats().drain_timeouts, 0U);
    EXPECT_EQ(bus.stats().received_own, 200U);
    ASSERT_EQ(per_symbol.size(), 4U);
    for (const auto& [symbol, sequences] : per_symbol) {
        ASSERT_EQ(sequences.size(), 50U) << symbol;
        EXPECT_TRUE(std::is_sorted(sequences.begin(), sequences.end())) << symbol;
    }
}

TEST_F(KafkaMockTest, TwoProcessesTalkThroughTheCluster) {
    const std::string prefix = mock_prefix("two");
    kf::RdKafkaTransport strategy_side{mock_config("strategy")};
    kf::RdKafkaTransport risk_side{mock_config("risk")};
    ev::TopicEventBus strategy_bus{strategy_side, clock_, bus_options(prefix, "strategy")};
    ev::TopicEventBus risk_bus{risk_side, clock_, bus_options(prefix, "risk")};

    strategy_bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        dom::TradeSignal s;
        s.symbol = std::get<dom::MarketEvent>(e.payload).symbol;
        s.side   = dom::SignalSide::Buy;
        strategy_bus.publish({ev::EventType::Signal, {}, 0, s});
    });
    std::latch all{20};
    std::atomic<int> signals{0};
    risk_bus.subscribe(ev::EventType::Signal, [&](const ev::Event&) {
        ++signals;
        all.count_down();
    });
    strategy_bus.start();
    risk_bus.start();
    for (std::uint64_t i = 1; i <= 20; ++i) {
        ASSERT_TRUE(risk_bus.publish(bar(i % 2 ? "AAPL" : "MSFT", i)));
    }
    all.wait();
    risk_bus.request_shutdown();
    risk_bus.wait_until_drained();
    strategy_bus.request_shutdown();
    strategy_bus.wait_until_drained();
    EXPECT_EQ(signals.load(), 20);
}

TEST_F(KafkaMockTest, ThePipelineRunsEndToEndThroughKafka) {
    const std::string prefix = mock_prefix("pipeline");
    kf::RdKafkaTransport transport{mock_config("pipeline")};
    ev::TopicEventBus bus{transport, clock_, bus_options(prefix)};
    std::vector<dom::TradeSignal> signals;   // dispatcher only
    bus.subscribe(ev::EventType::Signal,
                  [&](const ev::Event& e) { signals.push_back(std::get<dom::TradeSignal>(e.payload)); });

    trading_engine::app::Pipeline pipeline{bus, clock_};
    st::MovingAverageCrossoverConfig sma;
    sma.short_window = 2;
    sma.long_window  = 4;
    sma.symbols      = {"AAPL"};
    pipeline.add_strategy(std::make_shared<st::MovingAverageCrossoverStrategy>(sma));
    pipeline.start();
    // Down, then up, then down: at least one crossover each way.
    const int closes[] = {10, 9, 8, 7, 6, 7, 8, 9, 10, 11, 12, 11, 10, 9, 8, 7};
    std::uint64_t n = 0;
    for (const int close : closes) {
        const ev::Event e = bar("AAPL", ++n, tec::Price::from_units(close));
        pipeline.input().on_market_event(std::get<dom::MarketEvent>(e.payload));
    }
    pipeline.finish();

    constexpr std::uint64_t kBars = std::size(closes);
    EXPECT_EQ(pipeline.market_data().stats().accepted, kBars);
    EXPECT_EQ(pipeline.portfolio_bridge().stats().marks, kBars);
    ASSERT_GE(signals.size(), 2U);
    EXPECT_EQ(signals.size(), pipeline.strategies().stats().signals_published);
    EXPECT_EQ(signals[0].side, dom::SignalSide::Buy);
    EXPECT_EQ(signals[1].side, dom::SignalSide::Sell);
    EXPECT_EQ(bus.stats().decode_errors, 0U);
}

TEST(KafkaConfig, BadPropertiesFailLoudly) {
    kf::KafkaTransportConfig c;
    c.properties = {{"no.such.property", "1"}};
    EXPECT_THROW(kf::RdKafkaTransport{c}, kf::KafkaError);
}

// --- optional: a real broker ---------------------------------------------------

TEST(RealBroker, TopicBusRoundTrip) {
    const std::string bootstrap = env_or_empty("TE_KAFKA_BOOTSTRAP");
    if (bootstrap.empty()) {
        GTEST_SKIP() << "set TE_KAFKA_BOOTSTRAP=host:port to run against a real Kafka broker";
    }
    const tec::SystemClock clock;
    kf::RdKafkaTransport transport{transport_config(bootstrap, "real")};
    ev::TopicEventBus bus{transport, clock, bus_options(unique_prefix("real"))};
    std::atomic<int> seen{0};
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) { ++seen; });
    bus.start();
    for (std::uint64_t i = 1; i <= 100; ++i) {
        ASSERT_TRUE(bus.publish(bar("AAPL", i)));
    }
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_EQ(seen.load(), 100);
    EXPECT_EQ(bus.stats().drain_timeouts, 0U);
}

}  // namespace
