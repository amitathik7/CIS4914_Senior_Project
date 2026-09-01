#pragma once

// -----------------------------------------------------------------------------
//  TradingEngine -- the composition root and lifecycle coordinator.
//
//  Responsibility: own every component, wire them together via dependency
//  injection (interfaces, not globals), and run one of two modes:
//    * run_live()     -- AlpacaLiveSource -> ... -> analytics, real clock;
//    * run_backtest() -- AlpacaHistoricalSource / repository via
//                        HistoricalReplay, ManualClock, deterministic.
//  Coordinates ordered start-up (portfolio & bus first, sources last) and
//  ordered shutdown (stop sources, drain the bus, flush persistence, emit the
//  performance report).
//
//  STATUS: constructor stores config; run_live(), run_backtest(), and
//  request_shutdown() throw common::NotImplemented. main() only reports that
//  the scaffold built -- it does not construct or start this class.
//
//  Ownership / lifecycle: the engine owns all components (unique_ptr members in
//  the future). Destruction order is the reverse of construction so that
//  IPortfolioView& / IClock& references stay valid until their users are gone.
//
//  Thread-safety: control methods are called from one thread (the caller).
//  Components run on the event bus's worker threads once that exists.
// -----------------------------------------------------------------------------

#include <atomic>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/configuration/engine_config.hpp"

namespace trading_engine::app {

class TradingEngine {
public:
    explicit TradingEngine(config::EngineConfig config);
    ~TradingEngine();

    TradingEngine(const TradingEngine&) = delete;
    TradingEngine& operator=(const TradingEngine&) = delete;

    // Blocking. Runs until the feed ends (backtest) or shutdown is requested
    // (live). NOT IMPLEMENTED.
    void run_live();
    void run_backtest();

    // Async, signal-safe-ish request to stop; run_*() returns soon after.
    // NOT IMPLEMENTED.
    void request_shutdown() noexcept;

    [[nodiscard]] const config::EngineConfig& config() const noexcept { return config_; }
    [[nodiscard]] common::RunId run_id() const noexcept { return run_id_; }

private:
    config::EngineConfig config_;
    common::RunId        run_id_{};
    std::atomic<bool>    shutdown_requested_{false};

    // TODO: unique_ptr members for SystemMetrics, IClock, IEventBus,
    //       PortfolioManager, MarketDataService, StrategyEngine, RiskManager,
    //       ExecutionSimulator, PerformanceAnalyzer, repositories, and the
    //       active IMarketDataSource; build_* helpers per mode; a wait loop.
};

}  // namespace trading_engine::app
