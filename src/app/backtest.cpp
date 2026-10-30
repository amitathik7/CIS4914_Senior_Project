#include "trading_engine/app/backtest.hpp"

#include <algorithm>
#include <map>
#include <utility>

#include "trading_engine/app/pipeline.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/execution/execution_simulator.hpp"
#include "trading_engine/execution/fill_model.hpp"

namespace trading_engine::app {

Backtest::Backtest(BacktestConfig config) : config_{std::move(config)} {}

Backtest::~Backtest() = default;

void Backtest::add_strategy(std::shared_ptr<strategy::IStrategy> strategy) {
    if (strategy == nullptr) {
        throw common::ValidationError("Backtest::add_strategy: strategy must not be null");
    }
    strategies_.push_back(std::move(strategy));
}

BacktestResult Backtest::run(std::vector<domain::MarketEvent> bars) {
    if (used_) {
        throw common::ValidationError("Backtest::run: a backtest runs once");
    }
    if (bars.empty()) {
        throw common::ValidationError("Backtest::run: no bars to replay");
    }
    used_ = true;
    std::stable_sort(bars.begin(), bars.end(), [](const auto& a, const auto& b) {
        return a.exchange_time < b.exchange_time;
    });

    common::ManualClock clock{bars.front().exchange_time};
    events::InProcessEventBus bus{clock};

    PipelineConfig pipeline_config;
    pipeline_config.run_id        = config_.run_id;
    pipeline_config.starting_cash = config_.simulation.starting_cash;
    pipeline_config.fees          = config_.simulation.fees;
    pipeline_config.market_data   = config_.market_data;
    Pipeline pipeline{bus, clock, pipeline_config};
    for (const auto& s : strategies_) {
        pipeline.add_strategy(s);
    }

    const execution::NaiveFillModel fill_model;
    execution::ExecutionSimulator execution{fill_model, bus, clock, config_.simulation, config_.run_id};

    risk::RiskManager risk{pipeline.portfolio(), bus, clock, config_.risk,
                           config_.risk_policies ? risk::starter_policies()
                                                 : std::vector<std::shared_ptr<risk::IRiskPolicy>>{}};
    risk.set_reservation_ledger(&pipeline.reservations());
    risk.set_approval_sink([&execution](const domain::TradeSignal& s) { (void)execution.submit(s); });

    // Recorders. They run on the dispatcher and are read only after draining.
    BacktestResult result;
    std::map<common::OrderId, domain::Order> orders;
    bus.subscribe(events::EventType::Signal, [&](const events::Event& e) {
        result.signals.push_back(std::get<domain::TradeSignal>(e.payload));
    });
    bus.subscribe(events::EventType::Order, [&](const events::Event& e) {
        const auto& o = std::get<domain::Order>(e.payload);
        orders[o.id]  = o;
    });
    bus.subscribe(events::EventType::Fill, [&](const events::Event& e) {
        result.fills.push_back(std::get<domain::Fill>(e.payload));
    });

    risk.start();   // before the pipeline: prices are current when signals arrive
    pipeline.start();

    std::vector<domain::PortfolioSnapshot> curve;
    curve.push_back(pipeline.portfolio().snapshot());   // the starting point, for analysis
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const domain::MarketEvent& bar = bars[i];
        clock.set(bar.exchange_time);
        execution.on_market_event(bar);
        bus.wait_until_drained();
        pipeline.input().on_market_event(bar);
        bus.wait_until_drained();
        const bool last_at_time = i + 1 == bars.size() || bars[i + 1].exchange_time != bar.exchange_time;
        if (last_at_time) {
            result.equity_curve.push_back(pipeline.portfolio().snapshot());
        }
    }

    pipeline.finish();
    risk.stop();

    curve.insert(curve.end(), result.equity_curve.begin(), result.equity_curve.end());
    result.final_snapshot     = pipeline.portfolio().snapshot();
    result.report             = analytics::PerformanceAnalyzer{config_.analysis}.analyze(curve, result.fills);
    result.decisions          = risk.decisions();
    result.risk               = risk.stats();
    result.open_orders_at_end = execution.open_order_count();
    result.market_data        = pipeline.market_data().stats();
    result.strategies         = pipeline.strategies().stats();
    result.portfolio          = pipeline.portfolio_bridge().stats();
    result.orders.reserve(orders.size());
    for (auto& [id, order] : orders) {
        result.orders.push_back(std::move(order));
    }
    return result;
}

}  // namespace trading_engine::app
