#pragma once

// -----------------------------------------------------------------------------
//  IStrategy -- interface for interchangeable trading strategies.
//
//  Responsibility: given a stream of market events, decide when to emit
//  TradeSignals. A strategy is pure decision logic: it does not size against
//  the portfolio, check risk, or place orders -- downstream stages do that.
//
//  Ownership / lifecycle: strategies are held by StrategyEngine as
//  shared_ptr<IStrategy>. Within each StrategyEngine::start() / stop() cycle,
//  on_start() is called once before the first event and on_stop() once after
//  the last. A restarted engine calls on_start() again on the same object (it
//  never recreates strategies), so on_start() is where a strategy resets its
//  per-run state. If on_start() throws, on_stop() is NOT called for that
//  strategy. Implementations must be cheap per event and must NOT block (no
//  I/O, no locks held across calls). A throwing callback is isolated and
//  counted by the engine, but that is a safety net, not a control-flow tool.
//
//  Thread-safety: callbacks for a given strategy never overlap and are ordered
//  (on_start, then events, then on_stop), so implementations need no internal
//  locking. They are NOT guaranteed to share a thread: on_start() / on_stop()
//  run on the thread that calls StrategyEngine::start() / stop(), and
//  on_market_event() on whichever thread delivers the event. If that changes
//  it will be documented here.
// -----------------------------------------------------------------------------

#include <string_view>

#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace trading_engine::strategy {

// Where a strategy sends the signals it decides to emit. StrategyEngine owns
// the implementation and hands every registered strategy its own sink, so the
// engine always knows which strategy is emitting.
//
// Contract:
//  * emit() copies `signal`; the caller's object is never modified. The engine
//    overwrites `id` (fresh, nonzero), `created_at` (injected clock) and
//    `strategy_id` (the emitting strategy's id()); whatever the strategy put in
//    those three fields is ignored. Every other field is forwarded unchanged.
//  * The sink is valid only inside the on_market_event() call that received
//    it. Do not keep the reference: while the engine exists, an emit() from
//    anywhere else is dropped and counted, never published, and once the engine
//    is destroyed the sink no longer exists at all.
//  * emit() does not report bus problems. A signal the bus rejects, or a
//    publish that fails, is counted by the engine and not signalled back, so
//    there is no return value and a strategy cannot learn the SignalId it was
//    assigned (see docs/adr/0002-strategy-risk-signal-contract.md, section 7).
class ISignalSink {
public:
    virtual ~ISignalSink();
    virtual void emit(const domain::TradeSignal& signal) = 0;
};

class IStrategy {
public:
    virtual ~IStrategy();

    // Stable, unique identifier used in TradeSignal::strategy_id and configs.
    // The engine reads it once, at registration, and rejects empty or
    // duplicate values; changing it afterwards has no effect.
    [[nodiscard]] virtual std::string_view id() const = 0;

    virtual void on_start() {}
    virtual void on_stop() {}

    // Called once per market event routed to this strategy. The engine does not
    // filter by symbol or event type: every strategy sees every event, so a
    // strategy ignores what it does not trade. Emit zero or more signals via
    // `out`, which is valid only for the duration of this call.
    virtual void on_market_event(const domain::MarketEvent& event, ISignalSink& out) = 0;

    // TODO: symbol-interest declaration so the engine can pre-filter routing;
    //       warm-up / history requirements; parameter injection; a hook to
    //       receive its own fills for state-machine strategies.
};

}  // namespace trading_engine::strategy
