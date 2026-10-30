#include "trading_engine/app/pipeline.hpp"

#include <stdexcept>
#include <utility>

#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::app {

Pipeline::Pipeline(events::IEventBus& bus, const common::IClock& clock, PipelineConfig config)
    : bus_{bus},
      portfolio_{config.run_id, config.starting_cash, clock, config.fees},
      market_data_{bus, clock, config.market_data},
      strategies_{bus, clock},
      bridge_{bus, portfolio_} {}

Pipeline::~Pipeline() { finish(); }

void Pipeline::add_strategy(std::shared_ptr<strategy::IStrategy> strategy) {
    if (state_ != State::Built) {
        throw std::logic_error("Pipeline: add_strategy() after start()");
    }
    strategies_.register_strategy(std::move(strategy));
}

void Pipeline::start() {
    if (state_ != State::Built) {
        throw std::logic_error("Pipeline: start() may be called once");
    }
    bridge_.attach();
    try {
        strategies_.start();
        bus_.start();
    } catch (...) {
        strategies_.stop();
        bridge_.detach();
        throw;
    }
    state_ = State::Running;
}

void Pipeline::finish() noexcept {
    if (state_ != State::Running) {
        return;
    }
    state_ = State::Finished;
    try {
        bus_.request_shutdown();
        bus_.wait_until_drained();
    } catch (...) {
        // Draining failed; still withdraw every subscription below.
    }
    strategies_.stop();
    bridge_.detach();
}

}  // namespace trading_engine::app
