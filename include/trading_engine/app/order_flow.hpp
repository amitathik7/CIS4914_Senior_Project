#pragma once

// -----------------------------------------------------------------------------
//  OrderFlow -- the risk and execution stage of a Pipeline, on the bus.
//
//      ... StrategyEngine ──Signal──▶ RiskManager ──approved──▶ ExecutionSimulator
//                                        │                        │
//                                        └─RiskDecision─▶ bus     └─Order / Fill─▶ bus
//                                                                   ──▶ PortfolioBusBridge
//
//  Adds what app::Pipeline leaves out, so a run goes all the way from bars to
//  fills and a moving portfolio: the RiskManager (starter policies plus the
//  portfolio's reservation ledger) checks every Signal, publishes a
//  RiskDecisionReport, and hands approved signals to the ExecutionSimulator,
//  which publishes Order and Fill events that the portfolio already consumes.
//  Market orders fill at the open of the next market event for their symbol
//  (NaiveFillModel, SimulationConfig slippage and fees).
//
//  Works on any IEventBus: in-process, or the topic bus over Kafka. The
//  risk -> execution hand-off is a direct call, not a bus hop, so an
//  unapproved signal can never reach execution.
//
//  app::Backtest composes the same components itself (it drives execution
//  directly per bar); this class is for runs where everything goes through
//  the bus, such as `trading_engine run`.
//
//  Lifecycle:
//    Pipeline p{...};  OrderFlow f{p, bus, clock, ...};
//    f.start();  p.start();  ...feed bars...  p.finish();  f.stop();
//  start() must come BEFORE Pipeline::start(): subscriptions run in the order
//  they were made, so execution sees each market event (and fills against
//  it) before the portfolio marks it. The destructor calls stop().
//
//  Ownership: owns the fill model, the simulator and the risk manager;
//  borrows the pipeline, the bus and the clock, which must outlive it.
// -----------------------------------------------------------------------------

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/execution/execution_simulator.hpp"
#include "trading_engine/execution/fill_model.hpp"
#include "trading_engine/risk/risk_manager.hpp"

namespace trading_engine::events { class IEventBus; }

namespace trading_engine::app {

class Pipeline;

struct OrderFlowConfig {
    common::RunId            run_id{1};          // match PipelineConfig::run_id
    config::RiskLimits       risk{};
    config::SimulationConfig simulation{};       // slippage_bps and fees are used
    bool                     risk_policies{true};   // false: buying power / no-short only
};

class OrderFlow {
public:
    OrderFlow(Pipeline& pipeline, events::IEventBus& bus, const common::IClock& clock,
              OrderFlowConfig config = {});
    ~OrderFlow();

    OrderFlow(const OrderFlow&)            = delete;
    OrderFlow& operator=(const OrderFlow&) = delete;

    void start();          // before Pipeline::start(); idempotent
    void stop() noexcept;  // after Pipeline::finish(); idempotent

    [[nodiscard]] const risk::RiskManager& risk() const noexcept { return risk_; }
    // For the kill switch (RiskManager::set_kill_switch), from any thread.
    [[nodiscard]] risk::RiskManager& risk() noexcept { return risk_; }
    [[nodiscard]] const execution::ExecutionSimulator& execution() const noexcept { return execution_; }

private:
    execution::NaiveFillModel     fill_model_{};
    execution::ExecutionSimulator execution_;
    risk::RiskManager             risk_;
};

}  // namespace trading_engine::app
