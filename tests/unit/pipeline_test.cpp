// The existing components wired through each bus produce the same signals.
//
// The reference is the synchronous RecordingEventBus the strategy tests already
// trust. The pipeline then runs on the InProcessEventBus and on the TopicEventBus
// (the Kafka path, here over the in-memory broker) and must publish exactly the
// same TradeSignals, field for field.

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include "support/recording_event_bus.hpp"
#include "trading_engine/app/pipeline.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/events/message_transport.hpp"
#include "trading_engine/events/topic_event_bus.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

namespace {

namespace ev  = trading_engine::events;
namespace dom = trading_engine::domain;
namespace tec = trading_engine::common;
namespace app = trading_engine::app;
namespace st  = trading_engine::strategy;
using namespace std::chrono;

// Two symbols, interleaved, oscillating with a drift so both strategies fire
// repeatedly; plus a few rows MarketDataService must reject.
std::vector<dom::MarketEvent> bars() {
    std::vector<dom::MarketEvent> out;
    const tec::Timestamp start{sys_days{2026y / January / 5} + 14h + 30min};
    for (int i = 0; i < 120; ++i) {
        for (const char* symbol : {"AAPL", "MSFT"}) {
            const double base  = symbol[0] == 'A' ? 100.0 : 300.0;
            const double price = base + 8.0 * std::sin(i / 4.0) + 0.05 * i + (i % 7 == 0 ? 6.0 : 0.0);
            dom::MarketEvent e;
            e.symbol        = symbol;
            e.type          = dom::MarketEventType::Bar;
            e.exchange_time = start + minutes{i};
            e.price         = tec::Price::from_double(std::round(price * 100.0) / 100.0);
            out.push_back(e);
        }
    }
    dom::MarketEvent no_symbol;
    no_symbol.type  = dom::MarketEventType::Bar;
    no_symbol.price = tec::Price::from_units(1);
    out.insert(out.begin() + 10, no_symbol);
    dom::MarketEvent negative = out[3];
    negative.price            = -tec::Price::from_units(1);
    out.insert(out.begin() + 20, negative);
    return out;
}

struct Result {
    std::vector<dom::TradeSignal> signals;
    tec::Money cash{};
    std::size_t positions{0};
    tec::Money equity{};
    std::uint64_t accepted{0};
    std::uint64_t rejected{0};
    std::uint64_t marks{0};
};

Result run_on(ev::IEventBus& bus, const tec::IClock& clock, bool collect_from_bus) {
    Result r;
    std::mutex m;
    if (collect_from_bus) {
        bus.subscribe(ev::EventType::Signal, [&](const ev::Event& e) {
            const std::lock_guard lock{m};
            r.signals.push_back(std::get<dom::TradeSignal>(e.payload));
        });
    }
    {
        app::Pipeline pipeline{bus, clock};
        st::MovingAverageCrossoverConfig sma;
        sma.short_window = 3;
        sma.long_window  = 8;
        sma.symbols      = {"AAPL", "MSFT"};
        pipeline.add_strategy(std::make_shared<st::MovingAverageCrossoverStrategy>(sma));
        st::MeanReversionConfig mr;
        mr.lookback        = 10;
        mr.entry_threshold = 1.5;
        mr.symbols         = {"AAPL", "MSFT"};
        pipeline.add_strategy(std::make_shared<st::MeanReversionStrategy>(mr));

        pipeline.start();
        for (const auto& bar : bars()) {
            pipeline.input().on_market_event(bar);
        }
        pipeline.finish();

        r.accepted  = pipeline.market_data().stats().accepted;
        r.rejected  = pipeline.market_data().stats().rejected;
        r.marks     = pipeline.portfolio_bridge().stats().marks;
        const auto snapshot = pipeline.portfolio().snapshot();
        r.cash      = snapshot.cash;
        r.positions = snapshot.positions.size();
        r.equity    = snapshot.total_equity;
    }
    return r;
}

void expect_same_signals(const std::vector<dom::TradeSignal>& expected,
                         const std::vector<dom::TradeSignal>& actual) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual[i].id, expected[i].id);
        EXPECT_EQ(actual[i].strategy_id, expected[i].strategy_id);
        EXPECT_EQ(actual[i].symbol, expected[i].symbol);
        EXPECT_EQ(actual[i].side, expected[i].side);
        EXPECT_EQ(actual[i].created_at, expected[i].created_at);
        EXPECT_EQ(actual[i].requested_quantity, expected[i].requested_quantity);
        EXPECT_EQ(actual[i].order_type, expected[i].order_type);
        EXPECT_EQ(actual[i].metadata, expected[i].metadata);
    }
}

class PipelineTest : public ::testing::Test {
protected:
    // A fixed clock: created_at is then identical on every bus.
    tec::ManualClock clock_{tec::Timestamp{sys_days{2026y / January / 5}}};

    Result reference() {
        trading_engine::test_support::RecordingEventBus recording;
        Result r  = run_on(recording, clock_, /*collect_from_bus=*/false);
        r.signals = recording.signals();
        return r;
    }
};

TEST_F(PipelineTest, TheReferenceRunActuallyProducesSignals) {
    const Result r = reference();
    EXPECT_GT(r.signals.size(), 10U);
    EXPECT_EQ(r.accepted, 240U);
    EXPECT_EQ(r.rejected, 2U);
    EXPECT_EQ(r.marks, 240U);
    bool sma = false;
    bool mr  = false;
    for (const auto& s : r.signals) {
        sma = sma || s.strategy_id == "sma_crossover";
        mr  = mr || s.strategy_id == "mean_reversion";
    }
    EXPECT_TRUE(sma && mr) << "both strategies should fire on this data";
}

TEST_F(PipelineTest, InProcessBusMatchesTheReference) {
    const Result expected = reference();
    ev::InProcessEventBus bus{clock_};
    const Result actual = run_on(bus, clock_, true);
    expect_same_signals(expected.signals, actual.signals);
    EXPECT_EQ(actual.marks, expected.marks);
    EXPECT_EQ(actual.cash, expected.cash);
    EXPECT_EQ(actual.equity, expected.equity);
    EXPECT_EQ(bus.stats().handler_errors, 0U);
}

TEST_F(PipelineTest, TopicBusMatchesTheReference) {
    const Result expected = reference();
    auto broker = std::make_shared<ev::InMemoryBroker>();
    ev::InMemoryTransport transport{broker};
    ev::TopicEventBus bus{transport, clock_, {.topic_prefix = "pipeline", .poll_interval = milliseconds{5}}};
    const Result actual = run_on(bus, clock_, true);
    expect_same_signals(expected.signals, actual.signals);
    EXPECT_EQ(actual.marks, expected.marks);
    EXPECT_EQ(bus.stats().decode_errors, 0U);
    EXPECT_EQ(bus.stats().drain_timeouts, 0U);
    // Everything crossed the "broker": 240 bars plus every signal.
    EXPECT_EQ(broker->messages("pipeline.market_data").size(), 240U);
    EXPECT_EQ(broker->messages("pipeline.signal").size(), expected.signals.size());
}

}  // namespace
