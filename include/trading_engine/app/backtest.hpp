#pragma once

// -----------------------------------------------------------------------------
//  Backtest -- one deterministic simulated run over recorded bars, end to end.
//
//      bars -> ExecutionSimulator (fills orders from earlier bars at this open)
//           -> MarketDataService -> bus -> StrategyEngine -> Signal
//           -> RiskManager (policies + buying-power hold) -> ExecutionSimulator
//           -> Order / Fill -> PortfolioManager -> snapshot per timestamp
//           -> PerformanceAnalyzer
//
//  Every stage is the real component. Determinism: a ManualClock is set to
//  each bar's exchange_time before it is delivered, and the bus is drained
//  after every step, so the order of events never depends on thread timing:
//  the same bars and configuration give the same result every time.
//
//  Per bar, in order:
//    1. clock = bar.exchange_time;
//    2. orders submitted on EARLIER bars for this symbol fill at this bar's
//       open (else its close) moved by slippage; their Order/Fill events are
//       applied to the portfolio (drained);
//    3. the bar goes through MarketDataService; the portfolio marks the
//       close, strategies decide, risk checks each signal against the
//       portfolio as it is now, approved signals become Working orders;
//    4. after the last bar at this timestamp, a PortfolioSnapshot is kept.
//  An order still working after the last bar stays unfilled (reported).
//
//  Long only: the portfolio refuses a hold for a sell beyond the shares held,
//  so a Sell signal with no position is rejected, never a short.
//
//  Single use: construct, add_strategy() any number of times, run() once.
//  Not thread-safe; run() blocks the calling thread.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <memory>
#include <vector>

#include "trading_engine/analytics/performance_analyzer.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/performance_report.hpp"
#include "trading_engine/domain/portfolio_snapshot.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/market_data/market_data_service.hpp"
#include "trading_engine/portfolio/portfolio_bus_bridge.hpp"
#include "trading_engine/risk/risk_manager.hpp"
#include "trading_engine/strategy/strategy.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

namespace trading_engine::app {

struct BacktestConfig {
    common::RunId                 run_id{1};
    config::SimulationConfig      simulation{};   // starting cash, fees, slippage
    config::RiskLimits            risk{};
    // false: only the buying-power / shares-held check runs (no policies).
    bool                          risk_policies{true};
    analytics::AnalysisOptions    analysis{};
    market_data::MarketDataPolicy market_data{};
};

struct BacktestResult {
    std::vector<domain::TradeSignal>       signals{};     // as published, id order
    std::vector<risk::RiskRecord>          decisions{};   // one per signal
    std::vector<domain::Order>             orders{};      // latest state of each order, id order
    std::vector<domain::Fill>              fills{};       // execution order
    // One snapshot per distinct bar timestamp, after that timestamp's bars.
    std::vector<domain::PortfolioSnapshot> equity_curve{};
    domain::PortfolioSnapshot              final_snapshot{};
    domain::PerformanceReport              report{};
    std::size_t                            open_orders_at_end{0};

    market_data::MarketDataService::Stats  market_data{};
    strategy::StrategyEngine::Stats        strategies{};
    risk::RiskManager::Stats               risk{};
    portfolio::PortfolioBusBridge::Stats   portfolio{};
};

class Backtest {
public:
    explicit Backtest(BacktestConfig config = {});
    ~Backtest();

    Backtest(const Backtest&)            = delete;
    Backtest& operator=(const Backtest&) = delete;

    void add_strategy(std::shared_ptr<strategy::IStrategy> strategy);

    // Replays `bars` (stably sorted by exchange_time, so same-time bars keep
    // their given order) and returns everything the run produced. Throws
    // common::ValidationError for no bars or a second call.
    [[nodiscard]] BacktestResult run(std::vector<domain::MarketEvent> bars);

    [[nodiscard]] const BacktestConfig& config() const noexcept { return config_; }

private:
    BacktestConfig                                   config_;
    std::vector<std::shared_ptr<strategy::IStrategy>> strategies_{};
    bool                                             used_{false};
};

}  // namespace trading_engine::app
