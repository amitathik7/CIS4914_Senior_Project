#include "trading_engine/execution/execution_simulator.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::execution {

ExecutionSimulator::ExecutionSimulator(const IFillModel& fill_model,
                                       events::IEventBus& bus,
                                       const common::IClock& clock,
                                       config::SimulationConfig config)
    : fill_model_{fill_model},
      bus_{bus},
      clock_{clock},
      config_{std::move(config)} {}

ExecutionSimulator::~ExecutionSimulator() = default;

void ExecutionSimulator::start() {
    // TODO: subscribe to EventType::Signal and EventType::MarketData on bus_.
    throw common::NotImplemented("ExecutionSimulator::start");
}

void ExecutionSimulator::stop() {
    throw common::NotImplemented("ExecutionSimulator::stop");
}

void ExecutionSimulator::submit(const domain::TradeSignal& /*approved_signal*/) {
    // TODO: size the signal into an Order (order_ids_.next()), set status, run
    //       it through fill_model_ against the current MarketContext, publish
    //       the Order and each resulting Fill (fill_ids_.next()) on bus_, apply
    //       config_.latency before fills land.
    throw common::NotImplemented("ExecutionSimulator::submit(TradeSignal)");
}

void ExecutionSimulator::submit(const domain::Order& /*order*/) {
    throw common::NotImplemented("ExecutionSimulator::submit(Order)");
}

void ExecutionSimulator::on_market_event(const domain::MarketEvent& /*event*/) {
    // TODO: update the per-symbol MarketContext cache used by fill_model_.
    throw common::NotImplemented("ExecutionSimulator::on_market_event");
}

std::size_t ExecutionSimulator::open_order_count() const {
    throw common::NotImplemented("ExecutionSimulator::open_order_count");
}

}  // namespace trading_engine::execution
