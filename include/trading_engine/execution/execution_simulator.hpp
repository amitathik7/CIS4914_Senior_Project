#pragma once

// -----------------------------------------------------------------------------
//  ExecutionSimulator -- turns approved signals/orders into simulated fills.
//
//  Responsibility: accept risk-approved intents, materialise Orders, maintain
//  per-symbol MarketContext from the market-data stream, ask the injected
//  IFillModel to produce Fills, update order status, and publish Orders and
//  Fills onto the event bus for the PortfolioManager and persistence.
//
//  Ownership / lifecycle: owned by TradingEngine. Holds the IFillModel by
//  reference (composition root owns it). Subscribes to Signal and MarketData
//  events in start().
//
//  Thread-safety: single bus-worker thread in the scaffold. A future
//  multi-threaded design must protect the open-order table and market-context
//  map.
// -----------------------------------------------------------------------------

#include <cstddef>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/execution/fill_model.hpp"
#include "trading_engine/market_data/market_data_source.hpp"

namespace trading_engine::events { class IEventBus; }

namespace trading_engine::execution {

class ExecutionSimulator final : public market_data::IMarketEventSink {
public:
    ExecutionSimulator(const IFillModel& fill_model,
                       events::IEventBus& bus,
                       const common::IClock& clock,
                       config::SimulationConfig config);
    ~ExecutionSimulator() override;

    void start();   // NOT IMPLEMENTED: subscribe to Signal + MarketData
    void stop();    // NOT IMPLEMENTED

    // Build an Order from an approved signal and route it. NOT IMPLEMENTED.
    void submit(const domain::TradeSignal& approved_signal);

    // Directly submit a pre-built order (e.g. from tests). NOT IMPLEMENTED.
    void submit(const domain::Order& order);

    // Keep MarketContext current. NOT IMPLEMENTED.
    void on_market_event(const domain::MarketEvent& event) override;

    [[nodiscard]] std::size_t open_order_count() const;   // NOT IMPLEMENTED

private:
    [[maybe_unused]] const IFillModel&        fill_model_;
    [[maybe_unused]] events::IEventBus&       bus_;
    [[maybe_unused]] const common::IClock&    clock_;
    [[maybe_unused]] config::SimulationConfig config_;
    [[maybe_unused]] common::SequentialIdGenerator<common::OrderId> order_ids_{};
    [[maybe_unused]] common::SequentialIdGenerator<common::FillId>  fill_ids_{};

    // TODO: open-order table keyed by OrderId, per-symbol MarketContext cache,
    //       a latency/timer queue so fills land after order_to_fill delay,
    //       cancel/replace handling, partial-fill continuation.
};

}  // namespace trading_engine::execution
