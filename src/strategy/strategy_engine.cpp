#include "trading_engine/strategy/strategy_engine.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::strategy {

StrategyEngine::StrategyEngine(events::IEventBus& bus, const common::IClock& clock)
    : bus_{bus}, clock_{clock} {}

StrategyEngine::~StrategyEngine() = default;

void StrategyEngine::register_strategy(std::shared_ptr<IStrategy> strategy) {
    // Setup-time wiring. Real (not a stub): the registry has to work before any
    // events flow. Input is validated at this boundary.
    if (strategy == nullptr) {
        throw common::ValidationError(
            "StrategyEngine::register_strategy: strategy must not be null");
    }
    strategies_.push_back(std::move(strategy));
}

void StrategyEngine::start() {
    // TODO: subscribe to EventType::MarketData on bus_; call on_start() for
    //       every registered strategy.
    throw common::NotImplemented("StrategyEngine::start");
}

void StrategyEngine::stop() {
    throw common::NotImplemented("StrategyEngine::stop");
}

void StrategyEngine::on_market_event(const domain::MarketEvent& /*event*/) {
    // TODO: route to interested strategies, provide the ISignalSink, stamp each
    //       emitted TradeSignal with signal_ids_.next() and clock_->now(), then
    //       publish EventType::Signal on bus_. Isolate per-strategy exceptions.
    throw common::NotImplemented("StrategyEngine::on_market_event");
}

}  // namespace trading_engine::strategy
