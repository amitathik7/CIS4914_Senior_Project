#include "trading_engine/app/order_flow.hpp"

#include <memory>
#include <vector>

#include "trading_engine/app/pipeline.hpp"
#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::app {

OrderFlow::OrderFlow(Pipeline& pipeline, events::IEventBus& bus, const common::IClock& clock,
                     OrderFlowConfig config)
    : execution_{fill_model_, bus, clock, config.simulation, config.run_id},
      risk_{pipeline.portfolio(), bus, clock, config.risk,
            config.risk_policies ? risk::starter_policies()
                                 : std::vector<std::shared_ptr<risk::IRiskPolicy>>{}} {
    risk_.set_run_id(config.run_id);
    risk_.set_reservation_ledger(&pipeline.reservations());
    risk_.set_approval_sink(
        [this](const domain::TradeSignal& signal) { (void)execution_.submit(signal); });
}

OrderFlow::~OrderFlow() { stop(); }

void OrderFlow::start() {
    execution_.start();   // first: fills land before anything else sees the bar
    risk_.start();
}

void OrderFlow::stop() noexcept {
    try {
        risk_.stop();
        execution_.stop();
    } catch (...) {
        // unsubscribe() does not throw for a known id; nothing to recover here.
    }
}

}  // namespace trading_engine::app
