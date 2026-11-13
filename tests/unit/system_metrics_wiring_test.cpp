#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "trading_engine/analytics/system_metrics.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/events/message_transport.hpp"
#include "trading_engine/events/topic_event_bus.hpp"
#include "trading_engine/market_data/market_data_service.hpp"

namespace {

using namespace std;
using namespace std::chrono_literals;

namespace an = trading_engine::analytics;
namespace dom = trading_engine::domain;
namespace ev = trading_engine::events;
namespace md = trading_engine::market_data;
namespace tec = trading_engine::common;

ev::Event bar(const string& symbol) {
    dom::MarketEvent market_event;

    market_event.symbol = symbol;
    market_event.type =
        dom::MarketEventType::Bar;

    ev::Event event;

    event.type =
        ev::EventType::MarketData;

    event.payload =
        market_event;

    return event;
}

optional<size_t> queue_depth(
    const an::SystemMetrics::Snapshot& snapshot,
    string_view name) {

    const auto found =
        find_if(
            snapshot.queue_depths.begin(),
            snapshot.queue_depths.end(),
            [name](
                const an::SystemMetrics::QueueDepth& queue) {
                return string_view{queue.name} == name;
            });

    if (found ==
        snapshot.queue_depths.end()) {

        return nullopt;
    }

    return found->depth;
}

TEST(
    SystemMetricsWiring,
    MarketDataServiceRecordsIngestMetrics) {

    tec::ManualClock clock;
    an::SystemMetrics metrics;

    ev::InProcessEventBus bus{
        clock};

    md::MarketDataService service{
        bus,
        clock,
        md::MarketDataPolicy{},
        nullptr,
        &metrics};

    bus.start();

    dom::MarketEvent valid;

    valid.symbol = "AAPL";

    valid.type =
        dom::MarketEventType::Trade;

    valid.price =
        tec::Price::from_double(
            100.0);

    service.on_market_event(
        valid);

    dom::MarketEvent invalid =
        valid;

    invalid.price =
        tec::Price::from_double(
            -1.0);

    service.on_market_event(
        invalid);

    bus.request_shutdown();
    bus.wait_until_drained();

    const auto snapshot =
        metrics.snapshot();

    const auto ingest =
        static_cast<size_t>(
            an::Stage::Ingest);

    EXPECT_EQ(
        snapshot.stages[ingest].events,
        1u);

    EXPECT_EQ(
        snapshot.stages[ingest].errors,
        1u);

    EXPECT_EQ(
        snapshot.stages[ingest].latency.count,
        2u);
}

TEST(
    SystemMetricsWiring,
    InProcessBusReportsQueueDepth) {

    tec::ManualClock clock;
    an::SystemMetrics metrics;

    ev::InProcessEventBus bus{
        clock,
        {.capacity = 2},
        &metrics};

    latch entered{1};

    promise<void> release;

    auto released =
        release.get_future().share();

    bus.subscribe(
        ev::EventType::MarketData,
        [&](const ev::Event&) {
            entered.count_down();
            released.wait();
        });

    bus.start();

    ASSERT_TRUE(
        bus.publish(
            bar("AAPL")));

    entered.wait();

    ASSERT_TRUE(
        bus.publish(
            bar("MSFT")));

    const auto queued =
        metrics.snapshot();

    const auto queued_depth =
        queue_depth(
            queued,
            "event_bus");

    ASSERT_TRUE(
        queued_depth.has_value());

    EXPECT_EQ(
        *queued_depth,
        1u);

    release.set_value();

    bus.request_shutdown();
    bus.wait_until_drained();

    const auto drained =
        metrics.snapshot();

    const auto drained_depth =
        queue_depth(
            drained,
            "event_bus");

    ASSERT_TRUE(
        drained_depth.has_value());

    EXPECT_EQ(
        *drained_depth,
        0u);
}

TEST(
    SystemMetricsWiring,
    TopicBusReportsQueueDepth) {

    tec::ManualClock clock;
    an::SystemMetrics metrics;

    auto broker =
        make_shared<
            ev::InMemoryBroker>();

    ev::InMemoryTransport transport{
        broker};

    ev::TopicEventBusOptions options;

    options.topic_prefix =
        "metrics_test";

    options.origin =
        "metrics";

    options.poll_interval =
        5ms;

    options.drain_timeout =
        5s;

    ev::TopicEventBus bus{
        transport,
        clock,
        options,
        &metrics};

    latch entered{1};

    promise<void> release;

    auto released =
        release.get_future().share();

    bus.subscribe(
        ev::EventType::MarketData,
        [&](const ev::Event&) {
            entered.count_down();
            released.wait();
        });

    bus.start();

    ASSERT_TRUE(
        bus.publish(
            bar("AAPL")));

    entered.wait();

    const auto queued =
        metrics.snapshot();

    const auto queued_depth =
        queue_depth(
            queued,
            "kafka");

    ASSERT_TRUE(
        queued_depth.has_value());

    EXPECT_EQ(
        *queued_depth,
        1u);

    release.set_value();

    bus.request_shutdown();
    bus.wait_until_drained();

    const auto drained =
        metrics.snapshot();

    const auto drained_depth =
        queue_depth(
            drained,
            "kafka");

    ASSERT_TRUE(
        drained_depth.has_value());

    EXPECT_EQ(
        *drained_depth,
        0u);
}

} // namespace