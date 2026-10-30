#pragma once

// -----------------------------------------------------------------------------
//  ExecutionSimulator -- turns approved signals/orders into simulated fills.
//
//  Responsibility: accept risk-approved intents, materialise Orders, maintain
//  per-symbol MarketContext from the market-data stream, ask the injected
//  IFillModel to produce Fills, update order status, and publish Orders and
//  Fills onto the event bus for the PortfolioManager and persistence.
//
//  Timing (no look-ahead): an order submitted while bar T is being handled is
//  filled on the FIRST later market event for its symbol (exchange_time > the
//  order's created_at), at that event's open when it has one, else its price.
//  Strategies decide on a bar's close, so they can never trade at the price
//  that produced the decision. An order still working when the data ends
//  stays open (open_order_count() reports it). LatencyConfig is not modelled
//  beyond this one-event delay.
//
//  Events published, in order, per order: Order(Working) at submission, then
//  for each execution Fill(...) followed by Order(PartiallyFilled|Filled).
//  A signal that cannot become an order (Flat, no quantity) publishes
//  Order(Rejected) with reject_reason, so a hold placed for it is released.
//
//  Ownership / lifecycle: owned by the composition root. Holds the IFillModel
//  by reference. start() subscribes to MarketData; signals arrive through
//  submit(), called by the RiskManager's approval path (not from the bus, so
//  an unapproved signal can never reach execution).
//
//  Thread-safety: every public method takes one mutex. Bus publications are
//  made outside it.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/fill.hpp"
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
                       config::SimulationConfig config,
                       common::RunId run_id = common::RunId{1});
    ~ExecutionSimulator() override;

    ExecutionSimulator(const ExecutionSimulator&)            = delete;
    ExecutionSimulator& operator=(const ExecutionSimulator&) = delete;

    // Subscribe to MarketData. Idempotent. stop() unsubscribes (a barrier).
    void start();
    void stop();

    // Build a market Order from an approved signal and route it. Returns the
    // order id (Rejected orders get one too).
    common::OrderId submit(const domain::TradeSignal& approved_signal);

    // Submit a pre-built order (e.g. from tests). An unset id, run_id or
    // created_at is filled in; the order is published as Working.
    common::OrderId submit(const domain::Order& order);

    // Fill eligible working orders for event.symbol against this event.
    void on_market_event(const domain::MarketEvent& event) override;

    [[nodiscard]] std::size_t open_order_count() const;

private:
    struct Publication {
        std::optional<domain::Fill> fill;
        domain::Order               order;
    };
    void publish(const std::vector<Publication>& out);

    const IFillModel&        fill_model_;
    events::IEventBus&       bus_;
    const common::IClock&    clock_;
    config::SimulationConfig config_;
    common::RunId            run_id_;

    mutable std::mutex mutex_;
    common::SequentialIdGenerator<common::OrderId> order_ids_{};
    common::SequentialIdGenerator<common::FillId>  fill_ids_{};
    std::map<common::OrderId, domain::Order>       working_{};   // id order == submission order
    std::map<common::OrderId, std::uint32_t>       fill_counts_{};
    std::optional<common::SubscriptionId>          subscription_{};

    // TODO: order_to_fill latency queue, cancel/replace handling, a
    //       volume-participation cap per event.
};

}  // namespace trading_engine::execution
