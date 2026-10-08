#pragma once

// -----------------------------------------------------------------------------
//  SignalCancelRequest -- PROPOSED by docs/adr/0002-strategy-risk-signal-
//  contract.md (status: Proposed, not Accepted).
//
//  Responsibility: a strategy's request to cancel something it emitted
//  earlier, identified by that earlier TradeSignal's id. Deliberately NOT a
//  TradeSignal -- it has no side/quantity/price, because "cancel" isn't a
//  sizing intent, it's a request to stop one.
//
//  NOT WIRED IN YET -- this type cannot currently reach any running
//  component:
//    * strategy::ISignalSink only declares emit(const domain::TradeSignal&)
//      (see strategy.hpp); there is no way for an IStrategy to hand one of
//      these to the engine.
//    * events::EventPayload (event_bus.hpp) is a closed std::variant that
//      does not include this type; the bus cannot carry it either.
//  Wiring either of those in is future integration work with its own review,
//  not part of the ADR 0002 draft. See that ADR's "cancellation" section for
//  the open questions this type intentionally does NOT answer (what cancelling
//  a pending / already-filled / already-cancelled / unknown target means).
//
//  Plain value type, matching the other domain/ structs. Not thread-safe;
//  immutable once emitted.
// -----------------------------------------------------------------------------

#include <string>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::domain {

struct SignalCancelRequest {
    // This request's own id. PROPOSED: drawn from the same SignalId space as
    // TradeSignal::id (minted by the same StrategyEngine sequence) rather
    // than a new id type -- see the ADR's "ID encoding" / alternatives
    // section for why, and for the distinct-CancelRequestId alternative a
    // reviewer may prefer instead.
    common::SignalId id{};

    // The id of the earlier TradeSignal this request wants to cancel. Named
    // with an explicit "_id" suffix (unlike Order::origin_signal, which omits
    // it) to keep this new type's two ids visually distinct from each other.
    // Must not equal `id` above in a well-formed request.
    common::SignalId target_signal_id{};

    std::string       strategy_id{};   // should match the target signal's strategy_id
    common::Symbol    symbol{};        // informational only; target_signal_id is authoritative
    common::Timestamp created_at{};    // UTC

    // Deliberately absent: quantity, order type, price -- a cancel request
    // does not size or price anything. What "cancel" means for a target that
    // is still pending / already an order / already filled / already
    // cancelled / unknown is NOT decided by this type -- see the ADR.
};

}  // namespace trading_engine::domain
