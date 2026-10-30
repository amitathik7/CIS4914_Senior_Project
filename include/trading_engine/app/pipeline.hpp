#pragma once

// -----------------------------------------------------------------------------
//  Pipeline -- wires the components that exist today through one event bus.
//
//      source ──▶ MarketDataService ──MarketData──▶ StrategyEngine ──Signal──▶ (risk: TODO)
//                                       │
//                                       └─────────▶ PortfolioBusBridge ──▶ PortfolioManager
//                                                    ▲  (marks; Order / Fill events once
//                                                    │   the execution simulator publishes them)
//
//  The bus is injected, so the same wiring runs on an InProcessEventBus or on
//  a TopicEventBus over Kafka (docs/EVENT_BUS.md). Risk and execution are
//  still stubs: signals are published on the bus and whoever subscribes to
//  EventType::Signal (the CLI prints them) sees them; nothing turns them into
//  orders yet. When RiskManager / ExecutionSimulator land they subscribe and
//  publish here the same way, and the portfolio starts receiving Order and
//  Fill events with no change to this class's callers.
//
//  Lifecycle (one shot, like the buses):
//    add_strategy()...  start()  input().on_market_event()...  finish()
//  start() subscribes every consumer BEFORE starting the bus, so no early
//  event is missed. finish() follows the ordering StrategyEngine documents:
//  bus.request_shutdown(), bus.wait_until_drained(), then stop the engine and
//  detach the portfolio. The destructor runs finish() if needed.
//
//  Ownership: owns the portfolio, the market-data service, the strategy engine
//  and the bridge; borrows the bus and the clock, which must outlive it.
//  Threading: call the lifecycle methods and input() from one thread.
// -----------------------------------------------------------------------------

#include <memory>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/market_data/market_data_service.hpp"
#include "trading_engine/portfolio/portfolio_bus_bridge.hpp"
#include "trading_engine/portfolio/portfolio_manager.hpp"
#include "trading_engine/strategy/strategy.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

namespace trading_engine::events { class IEventBus; }

namespace trading_engine::app {

struct PipelineConfig {
    common::RunId                    run_id{1};
    common::Money                    starting_cash{common::Money::from_units(100'000)};
    config::FeeModelConfig           fees{};
    market_data::MarketDataPolicy    market_data{};
};

class Pipeline {
public:
    Pipeline(events::IEventBus& bus, const common::IClock& clock, PipelineConfig config = {});
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void add_strategy(std::shared_ptr<strategy::IStrategy> strategy);   // before start()
    void start();
    void finish() noexcept;

    // Where a market-data source pushes raw events.
    [[nodiscard]] market_data::IMarketEventSink& input() noexcept { return market_data_; }

    [[nodiscard]] const market_data::MarketDataService& market_data() const noexcept { return market_data_; }
    [[nodiscard]] const strategy::StrategyEngine& strategies() const noexcept { return strategies_; }
    [[nodiscard]] const portfolio::PortfolioManager& portfolio() const noexcept { return portfolio_; }
    [[nodiscard]] const portfolio::PortfolioBusBridge& portfolio_bridge() const noexcept { return bridge_; }

private:
    enum class State : std::uint8_t { Built, Running, Finished };

    events::IEventBus&            bus_;
    portfolio::PortfolioManager   portfolio_;
    market_data::MarketDataService market_data_;
    strategy::StrategyEngine      strategies_;
    portfolio::PortfolioBusBridge bridge_;
    State                         state_{State::Built};
};

}  // namespace trading_engine::app
