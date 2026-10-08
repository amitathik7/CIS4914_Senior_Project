#include "trading_engine/strategy/strategy.hpp"
#include "trading_engine/strategy/strategy_diagnostics.hpp"

namespace trading_engine::strategy {

ISignalSink::~ISignalSink() = default;
IStrategy::~IStrategy() = default;
IStrategyObserver::~IStrategyObserver() = default;

}  // namespace trading_engine::strategy
