#pragma once

// -----------------------------------------------------------------------------
//  IStrategy -- interface for interchangeable trading strategies.
//
//  Responsibility: given a stream of market events, decide when to emit
//  TradeSignals. A strategy is pure decision logic: it does not size against
//  the portfolio, check risk, or place orders -- downstream stages do that.
//
//  Ownership / lifecycle: strategies are held by StrategyEngine as
//  shared_ptr<IStrategy>. on_start() is called once before the first event,
//  on_stop() once after the last. Implementations must be cheap per event and
//  must NOT block (no I/O, no locks held across calls).
//
//  Thread-safety: for now, all callbacks for a given strategy happen on one
//  StrategyEngine thread, so implementations need no internal locking. If that
//  changes it will be documented here.
// -----------------------------------------------------------------------------

#include <string_view>

#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace trading_engine::strategy {

// Where a strategy sends the signals it decides to emit. The StrategyEngine
// provides the implementation (it forwards to the event bus and stamps ids).
class ISignalSink {
public:
    virtual ~ISignalSink();
    virtual void emit(const domain::TradeSignal& signal) = 0;
};

class IStrategy {
public:
    virtual ~IStrategy();

    // Stable, unique identifier used in TradeSignal::strategy_id and configs.
    [[nodiscard]] virtual std::string_view id() const = 0;

    virtual void on_start() {}
    virtual void on_stop() {}

    // Called once per market event routed to this strategy. Emit zero or more
    // signals via `out`.
    virtual void on_market_event(const domain::MarketEvent& event, ISignalSink& out) = 0;

    // TODO: symbol-interest declaration so the engine can pre-filter routing;
    //       warm-up / history requirements; parameter injection; a hook to
    //       receive its own fills for state-machine strategies.
};

}  // namespace trading_engine::strategy
