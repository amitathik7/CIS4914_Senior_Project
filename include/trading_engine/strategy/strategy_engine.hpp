#pragma once

// -----------------------------------------------------------------------------
//  StrategyEngine -- routes market events to strategies and collects signals.
//
//  Responsibility: own the set of registered strategies, deliver each inbound
//  MarketEvent to the interested ones, provide the ISignalSink they emit
//  through, stamp each emitted TradeSignal with an id and timestamp, and
//  publish it to the event bus for the RiskManager.
//
//  Ownership / lifecycle: owned by TradingEngine. Holds strategies via
//  shared_ptr. Subscribes to EventType::MarketData on the bus in start() and
//  unsubscribes in stop().
//
//  Thread-safety: consumes market events on a single bus-worker thread; the
//  scaffold does not add its own threads. register_strategy() is expected to
//  be called only during setup, before start().
// -----------------------------------------------------------------------------

#include <cstddef>
#include <memory>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/market_data/market_data_source.hpp"
#include "trading_engine/strategy/strategy.hpp"

namespace trading_engine::events { class IEventBus; }

namespace trading_engine::strategy {

class StrategyEngine final : public market_data::IMarketEventSink {
public:
    StrategyEngine(events::IEventBus& bus, const common::IClock& clock);
    ~StrategyEngine() override;

    // Setup-time only. NOT IMPLEMENTED (throws) for now.
    void register_strategy(std::shared_ptr<IStrategy> strategy);

    void start();   // NOT IMPLEMENTED: subscribe to the bus
    void stop();    // NOT IMPLEMENTED: unsubscribe, on_stop() each strategy

    // Fan-out entry point. NOT IMPLEMENTED.
    void on_market_event(const domain::MarketEvent& event) override;

    [[nodiscard]] std::size_t strategy_count() const noexcept { return strategies_.size(); }

private:
    [[maybe_unused]] events::IEventBus&       bus_;
    [[maybe_unused]] const common::IClock&    clock_;
    std::vector<std::shared_ptr<IStrategy>>   strategies_{};
    [[maybe_unused]] common::SequentialIdGenerator<common::SignalId> signal_ids_{};

    // TODO: symbol -> [strategy] routing index; the concrete ISignalSink
    //       implementation; per-strategy error isolation so one throwing
    //       strategy cannot stall the pipeline; latency instrumentation.
};

}  // namespace trading_engine::strategy
