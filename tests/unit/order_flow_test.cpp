// app::OrderFlow: risk and execution on the bus, so a Pipeline run goes from
// bars to risk decisions, orders, fills and a moving portfolio.
//
// The bars are replayed in market time (clock set to each timestamp, bus
// drained before the next), the same way `trading_engine run` does it. The
// run must then give the same fills on the InProcessEventBus and on the
// TopicEventBus, and the same fills and final portfolio as app::Backtest,
// which drives the very same components directly.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "trading_engine/app/backtest.hpp"
#include "trading_engine/app/order_flow.hpp"
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

// Two symbols, interleaved, oscillating with a drift so both strategies buy
// and sell repeatedly.
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
    return out;
}

std::vector<std::shared_ptr<st::IStrategy>> strategies(tec::Quantity quantity) {
    st::MovingAverageCrossoverConfig sma;
    sma.short_window       = 3;
    sma.long_window        = 8;
    sma.requested_quantity = quantity;
    sma.symbols            = {"AAPL", "MSFT"};
    st::MeanReversionConfig mr;
    mr.lookback           = 10;
    mr.entry_threshold    = 1.5;
    mr.requested_quantity = quantity;
    mr.symbols            = {"AAPL", "MSFT"};
    return {std::make_shared<st::MovingAverageCrossoverStrategy>(sma),
            std::make_shared<st::MeanReversionStrategy>(mr)};
}

struct FlowRun {
    std::vector<dom::TradeSignal>         signals;
    std::vector<dom::RiskDecisionReport>  decisions;
    std::vector<dom::Order>               orders;   // every Order event, in bus order
    std::vector<dom::Fill>                fills;
    dom::PortfolioSnapshot                final{};
    trading_engine::portfolio::PortfolioBusBridge::Stats bridge{};
    std::size_t                           open_at_end{0};
};

FlowRun run_on(ev::IEventBus& bus, tec::ManualClock& clock, app::OrderFlowConfig config = {},
           tec::Quantity quantity = 1) {
    FlowRun r;
    std::mutex m;
    bus.subscribe(ev::EventType::Signal, [&](const ev::Event& e) {
        const std::lock_guard lock{m};
        r.signals.push_back(std::get<dom::TradeSignal>(e.payload));
    });
    bus.subscribe(ev::EventType::RiskDecision, [&](const ev::Event& e) {
        const std::lock_guard lock{m};
        r.decisions.push_back(std::get<dom::RiskDecisionReport>(e.payload));
    });
    bus.subscribe(ev::EventType::Order, [&](const ev::Event& e) {
        const std::lock_guard lock{m};
        r.orders.push_back(std::get<dom::Order>(e.payload));
    });
    bus.subscribe(ev::EventType::Fill, [&](const ev::Event& e) {
        const std::lock_guard lock{m};
        r.fills.push_back(std::get<dom::Fill>(e.payload));
    });

    app::PipelineConfig pipeline_config;
    pipeline_config.starting_cash = config.simulation.starting_cash;
    pipeline_config.fees          = config.simulation.fees;
    app::Pipeline pipeline{bus, clock, pipeline_config};
    for (const auto& s : strategies(quantity)) {
        pipeline.add_strategy(s);
    }
    app::OrderFlow flow{pipeline, bus, clock, config};

    flow.start();
    pipeline.start();
    const auto input = bars();
    for (std::size_t i = 0; i < input.size(); ++i) {
        clock.set(input[i].exchange_time);
        pipeline.input().on_market_event(input[i]);
        if (i + 1 == input.size() || input[i + 1].exchange_time != input[i].exchange_time) {
            bus.wait_until_drained();
        }
    }
    pipeline.finish();
    flow.stop();

    r.final       = pipeline.portfolio().snapshot();
    r.bridge      = pipeline.portfolio_bridge().stats();
    r.open_at_end = flow.execution().open_order_count();
    return r;
}

void expect_same_fills(const std::vector<dom::Fill>& expected, const std::vector<dom::Fill>& actual) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual[i].id, expected[i].id);
        EXPECT_EQ(actual[i].order_id, expected[i].order_id);
        EXPECT_EQ(actual[i].symbol, expected[i].symbol);
        EXPECT_EQ(actual[i].filled_quantity, expected[i].filled_quantity);
        EXPECT_EQ(actual[i].fill_price, expected[i].fill_price);
        EXPECT_EQ(actual[i].fees, expected[i].fees);
        EXPECT_EQ(actual[i].filled_at, expected[i].filled_at);
    }
}

class OrderFlowTest : public ::testing::Test {
protected:
    tec::ManualClock clock_{bars().front().exchange_time};
};

TEST_F(OrderFlowTest, SignalsBecomeRiskDecisionsOrdersAndFills) {
    ev::InProcessEventBus bus{clock_};
    const FlowRun r = run_on(bus, clock_);

    ASSERT_GT(r.signals.size(), 10U);
    ASSERT_EQ(r.decisions.size(), r.signals.size()) << "one decision per signal";
    ASSERT_FALSE(r.fills.empty());
    EXPECT_EQ(bus.stats().handler_errors, 0U);
    EXPECT_EQ(r.bridge.errors, 0U);
    EXPECT_EQ(r.bridge.fills_applied, r.fills.size());

    std::map<std::uint64_t, dom::TradeSignal> signal_by_id;
    for (const auto& s : r.signals) {
        signal_by_id[s.id.value] = s;
    }
    std::map<std::uint64_t, dom::Order> first_event;   // by order id
    for (const auto& o : r.orders) {
        first_event.emplace(o.id.value, o);
    }

    for (const auto& d : r.decisions) {
        const auto& s = signal_by_id.at(d.signal_id.value);
        EXPECT_EQ(d.symbol, s.symbol);
        EXPECT_EQ(d.strategy_id, s.strategy_id);
        EXPECT_EQ(d.requested_quantity, 1);
        EXPECT_FALSE(d.checks.empty()) << "starter policies plus buying power ran";
        if (d.outcome == dom::RiskOutcome::Rejected) {
            ASSERT_TRUE(d.reason.has_value());
            EXPECT_FALSE(d.checks.back().passed);
            EXPECT_EQ(d.checks.back().policy, *d.reason);
            EXPECT_EQ(d.approved_quantity, 0);
        } else {
            EXPECT_EQ(d.approved_quantity, 1);
        }
    }

    // Every fill belongs to an order that was published Working first, at the
    // signal's bar, and the fill lands on a LATER bar (the next one's open).
    tec::Quantity net_aapl = 0;
    for (const auto& f : r.fills) {
        const auto& order = first_event.at(f.order_id.value);
        EXPECT_EQ(order.status, dom::OrderStatus::Working);
        EXPECT_LT(order.created_at, f.filled_at);
        EXPECT_EQ(f.filled_at - order.created_at, minutes{1});
        if (f.symbol == "AAPL") {
            net_aapl += f.filled_quantity;
        }
    }
    tec::Quantity held_aapl = 0;
    for (const auto& p : r.final.positions) {
        if (p.symbol == "AAPL") {
            held_aapl = p.quantity;
        }
    }
    EXPECT_EQ(held_aapl, net_aapl) << "the portfolio applied every fill";
    EXPECT_EQ(r.open_at_end, 0U) << "orders from the last bar may stay open, but this data ends flat";
    EXPECT_NE(r.final.cash, tec::Money::from_units(100'000)) << "the portfolio actually moved";
}

TEST_F(OrderFlowTest, TopicBusGivesTheSameFillsAsTheInProcessBus) {
    ev::InProcessEventBus local{clock_};
    const FlowRun expected = run_on(local, clock_);

    tec::ManualClock clock{bars().front().exchange_time};
    auto broker = std::make_shared<ev::InMemoryBroker>();
    ev::InMemoryTransport transport{broker};
    ev::TopicEventBus bus{transport, clock, {.topic_prefix = "flow", .poll_interval = milliseconds{2}}};
    const FlowRun actual = run_on(bus, clock);

    expect_same_fills(expected.fills, actual.fills);
    EXPECT_EQ(actual.decisions.size(), expected.decisions.size());
    EXPECT_EQ(actual.final.cash, expected.final.cash);
    EXPECT_EQ(actual.final.total_equity, expected.final.total_equity);
    EXPECT_EQ(bus.stats().decode_errors, 0U);
    EXPECT_EQ(bus.stats().drain_timeouts, 0U);
    EXPECT_EQ(broker->messages("flow.risk_decision").size(), expected.decisions.size());
    EXPECT_EQ(broker->messages("flow.fill").size(), expected.fills.size());
}

TEST_F(OrderFlowTest, MatchesAppBacktestFillForFillAndInTheFinalPortfolio) {
    ev::InProcessEventBus bus{clock_};
    const FlowRun flow = run_on(bus, clock_);

    app::Backtest backtest{app::BacktestConfig{}};
    for (const auto& s : strategies(1)) {
        backtest.add_strategy(s);
    }
    const app::BacktestResult reference = backtest.run(bars());

    expect_same_fills(reference.fills, flow.fills);
    EXPECT_EQ(flow.final.cash, reference.final_snapshot.cash);
    // Equal equity also shows the portfolio is marked at the bar's price, not
    // left at the fill price, after a fill that arrives over the bus.
    EXPECT_EQ(flow.final.total_equity, reference.final_snapshot.total_equity);
}

TEST_F(OrderFlowTest, RejectedSignalsNeverReachExecution) {
    app::OrderFlowConfig config;
    config.simulation.starting_cash = tec::Money::from_units(150);   // about one AAPL share
    ev::InProcessEventBus bus{clock_};
    const FlowRun r = run_on(bus, clock_, config);

    std::size_t rejected = 0;
    for (const auto& d : r.decisions) {
        if (d.outcome != dom::RiskOutcome::Rejected) {
            continue;
        }
        ++rejected;
        for (const auto& o : r.orders) {
            EXPECT_NE(o.origin_signal, d.signal_id) << "a rejected signal produced an order";
        }
    }
    EXPECT_GT(rejected, 0U);
    EXPECT_GE(r.final.cash, tec::Money{}) << "buying power held";
}

TEST_F(OrderFlowTest, OversizedSignalsAreResizedToTheOrderLimit) {
    app::OrderFlowConfig config;
    config.simulation.starting_cash        = tec::Money::from_units(10'000'000);
    config.risk.max_order_quantity         = 50;
    config.risk.max_position_notional      = tec::Money::from_units(1'000'000);
    config.risk.max_gross_exposure         = tec::Money::from_units(5'000'000);
    ev::InProcessEventBus bus{clock_};
    const FlowRun r = run_on(bus, clock_, config, /*quantity=*/80);

    bool saw_resize = false;
    for (const auto& d : r.decisions) {
        if (d.outcome == dom::RiskOutcome::Resized) {
            saw_resize = true;
            EXPECT_EQ(d.requested_quantity, 80);
            EXPECT_EQ(d.approved_quantity, 50);
            EXPECT_EQ(d.reason, "max_order_quantity");
        }
    }
    ASSERT_TRUE(saw_resize);
    for (const auto& f : r.fills) {
        EXPECT_LE(f.filled_quantity < 0 ? -f.filled_quantity : f.filled_quantity, 50);
    }
}

}  // namespace
