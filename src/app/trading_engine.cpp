#include "trading_engine/app/trading_engine.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::app {

TradingEngine::TradingEngine(config::EngineConfig config)
    : config_{std::move(config)}, run_id_{common::RunId{1}} {
    // TODO: derive a genuinely unique run id (wall-clock epoch / UUID / DB
    //       sequence) and persist run metadata (mode, config hash, git commit)
    //       via ITradeRepository once persistence exists.
}

TradingEngine::~TradingEngine() = default;

void TradingEngine::run_live() {
    // TODO: build components in dependency order (SystemMetrics, IClock=System,
    //       EventBus, PortfolioManager, MarketDataService, StrategyEngine,
    //       RiskManager, ExecutionSimulator, PerformanceAnalyzer, repositories,
    //       AlpacaLiveSource), start them, then block until request_shutdown().
    throw common::NotImplemented("TradingEngine::run_live");
}

void TradingEngine::run_backtest() {
    // TODO: same wiring but IClock=ManualClock, source=AlpacaHistoricalSource
    //       (or repository), driven by HistoricalReplay; on completion, run
    //       PerformanceAnalyzer and persist the PerformanceReport.
    throw common::NotImplemented("TradingEngine::run_backtest");
}

void TradingEngine::request_shutdown() noexcept {
    // Real: records the request. The (not-yet-written) run loops will observe
    // this flag and unwind in reverse construction order.
    shutdown_requested_.store(true, std::memory_order_relaxed);
}

}  // namespace trading_engine::app
