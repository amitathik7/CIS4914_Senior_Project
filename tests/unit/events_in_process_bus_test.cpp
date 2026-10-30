// InProcessEventBus against the IEventBus contract in events/event_bus.hpp.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/events/event_bus.hpp"

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
    out.type     = ev::EventType::MarketData;
    out.sequence = 999;   // the bus overwrites this
    out.payload  = e;
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

class InProcessBusTest : public ::testing::Test {
protected:
    tec::ManualClock clock_{tec::Timestamp{std::chrono::seconds{1'700'000'000}}};
};

TEST_F(InProcessBusTest, DeliversByTypeInSubscriptionOrderAndStampsEvents) {
    ev::InProcessEventBus bus{clock_};
    std::mutex m;
    std::vector<std::string> calls;
    std::vector<ev::Event> seen;
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        const std::lock_guard lock{m};
        calls.push_back("first");
        seen.push_back(e);
    });
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) {
        const std::lock_guard lock{m};
        calls.push_back("second");
    });
    bus.subscribe(ev::EventType::Fill, [&](const ev::Event&) {
        const std::lock_guard lock{m};
        calls.push_back("fill");
    });
    EXPECT_EQ(bus.subscription_count(ev::EventType::MarketData), 2U);

    bus.start();
    ASSERT_TRUE(bus.publish(bar("AAPL")));
    ASSERT_TRUE(bus.publish(bar("MSFT")));
    bus.request_shutdown();
    bus.wait_until_drained();

    EXPECT_EQ(calls, (std::vector<std::string>{"first", "second", "first", "second"}));
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0].sequence, 1U);
    EXPECT_EQ(seen[1].sequence, 2U);
    EXPECT_EQ(seen[0].enqueued_at, clock_.now());
    EXPECT_EQ(bus.stats().published, 2U);
    EXPECT_EQ(bus.stats().delivered, 2U);
    EXPECT_EQ(bus.depth(), 0U);
}

TEST_F(InProcessBusTest, RefusesEventsBeforeStartAndAfterShutdown) {
    ev::InProcessEventBus bus{clock_};
    EXPECT_FALSE(bus.publish(bar("AAPL")));
    bus.start();
    bus.start();   // no-op
    bus.request_shutdown();
    EXPECT_FALSE(bus.publish(bar("AAPL")));
    bus.wait_until_drained();
    EXPECT_FALSE(bus.publish(bar("AAPL")));
    EXPECT_EQ(bus.stats().rejected, 3U);
    EXPECT_THROW(bus.start(), std::logic_error);
}

TEST_F(InProcessBusTest, HandlersRunOnOneDispatcherThreadNotThePublisher) {
    ev::InProcessEventBus bus{clock_};
    std::mutex m;
    std::vector<std::thread::id> threads;
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) {
        const std::lock_guard lock{m};
        threads.push_back(std::this_thread::get_id());
    });
    bus.start();
    std::thread a{[&] { for (int i = 0; i < 50; ++i) bus.publish(bar("A")); }};
    std::thread b{[&] { for (int i = 0; i < 50; ++i) bus.publish(bar("B")); }};
    a.join();
    b.join();
    bus.request_shutdown();
    bus.wait_until_drained();

    ASSERT_EQ(threads.size(), 100U);
    for (const auto id : threads) {
        EXPECT_EQ(id, threads.front());
    }
    EXPECT_NE(threads.front(), std::this_thread::get_id());
}

TEST_F(InProcessBusTest, ConcurrentProducersLoseNothingAndSequencesFollowDeliveryOrder) {
    ev::InProcessEventBus bus{clock_, {.capacity = 64, .when_full = ev::BackpressurePolicy::Block}};
    std::vector<std::uint64_t> sequences;   // dispatcher only
    bus.subscribe(ev::EventType::MarketData,
                  [&](const ev::Event& e) { sequences.push_back(e.sequence); });
    bus.start();
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < 2'000; ++i) {
                EXPECT_TRUE(bus.publish(bar("X")));
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    bus.request_shutdown();
    bus.wait_until_drained();

    ASSERT_EQ(sequences.size(), 8'000U);
    for (std::size_t i = 0; i < sequences.size(); ++i) {
        EXPECT_EQ(sequences[i], i + 1);
    }
    EXPECT_LE(bus.stats().high_water_mark, 64U);
}

TEST_F(InProcessBusTest, UnsubscribeFromAnotherThreadWaitsForARunningHandler) {
    ev::InProcessEventBus bus{clock_};
    std::latch entered{1};
    std::atomic<bool> finished{false};
    std::atomic<int> calls{0};
    const auto id = bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) {
        ++calls;
        entered.count_down();
        std::this_thread::sleep_for(100ms);
        finished = true;
    });
    bus.start();
    bus.publish(bar("A"));
    entered.wait();
    bus.unsubscribe(id);   // the barrier: must not return while the handler sleeps
    EXPECT_TRUE(finished.load());
    bus.publish(bar("A"));
    bus.wait_until_drained();
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(bus.subscription_count(ev::EventType::MarketData), 0U);
}

TEST_F(InProcessBusTest, UnsubscribeInsideADeliverySkipsTheRestOfIt) {
    ev::InProcessEventBus bus{clock_};
    tec::SubscriptionId second{};
    int second_calls = 0;
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) { bus.unsubscribe(second); });
    second = bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) { ++second_calls; });
    bus.start();
    bus.publish(bar("A"));
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_EQ(second_calls, 0);
}

TEST_F(InProcessBusTest, AHandlerMayPublishAndItsEventsSurviveTheDrain) {
    ev::InProcessEventBus bus{clock_, {.capacity = 1}};
    std::atomic<int> signals{0};
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        // Nested publish never blocks, even at capacity, and is accepted while draining.
        EXPECT_TRUE(bus.publish(signal(std::get<dom::MarketEvent>(e.payload).symbol)));
        EXPECT_TRUE(bus.publish(signal("again")));
    });
    bus.subscribe(ev::EventType::Signal, [&](const ev::Event&) { ++signals; });
    bus.start();
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(bus.publish(bar("A")));
    }
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_EQ(signals.load(), 40);
}

TEST_F(InProcessBusTest, RejectPolicyRefusesWhenFull) {
    ev::InProcessEventBus bus{clock_, {.capacity = 1, .when_full = ev::BackpressurePolicy::Reject}};
    std::latch entered{1};
    std::promise<void> release;
    auto released = release.get_future().share();
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        if (std::get<dom::MarketEvent>(e.payload).sequence == 1) {
            entered.count_down();
            released.wait();
        }
    });
    bus.start();
    ASSERT_TRUE(bus.publish(bar("A", 1)));   // taken by the dispatcher, which blocks
    entered.wait();
    EXPECT_TRUE(bus.publish(bar("A", 2)));   // fills the queue
    EXPECT_FALSE(bus.publish(bar("A", 3)));  // full: refused at once
    EXPECT_EQ(bus.depth(), 1U);
    release.set_value();
    bus.wait_until_drained();
    EXPECT_EQ(bus.stats().rejected, 1U);
    EXPECT_EQ(bus.stats().delivered, 2U);
}

TEST_F(InProcessBusTest, BlockPolicyWaitsForRoomAndShutdownReleasesWaiters) {
    ev::InProcessEventBus bus{clock_, {.capacity = 1, .when_full = ev::BackpressurePolicy::Block}};
    std::latch entered{1};
    std::promise<void> release;
    auto released = release.get_future().share();
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event& e) {
        if (std::get<dom::MarketEvent>(e.payload).sequence == 1) {
            entered.count_down();
            released.wait();
        }
    });
    bus.start();
    ASSERT_TRUE(bus.publish(bar("A", 1)));
    entered.wait();
    ASSERT_TRUE(bus.publish(bar("A", 2)));
    auto blocked = std::async(std::launch::async, [&] { return bus.publish(bar("A", 3)); });
    EXPECT_EQ(blocked.wait_for(100ms), std::future_status::timeout) << "should be waiting for room";
    release.set_value();
    EXPECT_TRUE(blocked.get());
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_EQ(bus.stats().delivered, 3U);
}

TEST_F(InProcessBusTest, HandlerExceptionsAreCountedAndDoNotStopDelivery) {
    ev::InProcessEventBus bus{clock_};
    std::atomic<int> after{0};
    bus.subscribe(ev::EventType::MarketData, [](const ev::Event&) { throw std::runtime_error("boom"); });
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) { ++after; });
    bus.start();
    EXPECT_TRUE(bus.publish(bar("A")));
    EXPECT_TRUE(bus.publish(bar("A")));
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_EQ(after.load(), 2);
    EXPECT_EQ(bus.stats().handler_errors, 2U);
}

TEST_F(InProcessBusTest, MisuseIsLoud) {
    EXPECT_THROW((ev::InProcessEventBus{clock_, {.capacity = 0}}), std::invalid_argument);

    ev::InProcessEventBus bus{clock_};
    EXPECT_THROW(bus.subscribe(ev::EventType::Fill, ev::EventHandler{}), std::invalid_argument);
    EXPECT_EQ(bus.subscription_count(ev::EventType::Fill), 0U);

    std::atomic<bool> threw{false};
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) {
        try {
            bus.wait_until_drained();
        } catch (const std::logic_error&) {
            threw = true;
        }
    });
    bus.start();
    bus.publish(bar("A"));
    bus.request_shutdown();
    bus.wait_until_drained();
    EXPECT_TRUE(threw.load());
}

TEST_F(InProcessBusTest, WaitUntilDrainedWhileRunningKeepsTheBusUsable) {
    ev::InProcessEventBus bus{clock_};
    std::atomic<int> calls{0};
    bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) { ++calls; });
    bus.start();
    bus.publish(bar("A"));
    bus.wait_until_drained();
    EXPECT_EQ(calls.load(), 1);
    EXPECT_TRUE(bus.publish(bar("A")));
    bus.wait_until_drained();
    EXPECT_EQ(calls.load(), 2);
}

TEST_F(InProcessBusTest, DestructorDrainsWhatWasAccepted) {
    std::atomic<int> calls{0};
    {
        ev::InProcessEventBus bus{clock_};
        bus.subscribe(ev::EventType::MarketData, [&](const ev::Event&) {
            std::this_thread::sleep_for(1ms);
            ++calls;
        });
        bus.start();
        for (int i = 0; i < 20; ++i) {
            bus.publish(bar("A"));
        }
    }
    EXPECT_EQ(calls.load(), 20);
}

}  // namespace
