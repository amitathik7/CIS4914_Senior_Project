#pragma once

// -----------------------------------------------------------------------------
//  IReservationLedger -- the ONE mutating call the RiskManager is allowed.
//
//  Responsibility: check buying power and place a hold against it in a single
//  atomic operation, so two signals cannot be approved against the same cash.
//
//  Why this exists, and why it is separate from IPortfolioView:
//    * Checking and then reserving as two steps leaves a window. An approved
//      signal has to reach the ExecutionSimulator, become an Order and come
//      back before the hold exists, so every signal already queued behind it
//      is evaluated against buying power that does not yet reflect it. The
//      window is measured in queue positions, not milliseconds -- in a
//      backtest at replay_speed = 0 it is the normal case, not a rare race.
//    * Keeping the mutating call on its own interface means IPortfolioView
//      stays read-only: the RiskManager still cannot change portfolio state
//      through the view it uses for everything else.
//
//  PROPOSED by docs/adr/0005-portfolio-risk-query-contract.md section 4, which
//  records the team decision (2026-09-20).
//
//  Ownership / lifecycle: implemented by PortfolioManager, which owns the
//  cash this holds against. The RiskManager holds a reference to it.
//
//  Thread-safety: hold_for_signal() MUST be atomic against itself and against
//  the write side -- the check and the hold happen under one lock, or the
//  window it exists to close reopens. This is only possible because the
//  RiskManager and the PortfolioManager live in one process (see that ADR's
//  section 5 on synchronous reads).
// -----------------------------------------------------------------------------

#include <optional>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/order.hpp"

namespace trading_engine::portfolio {

struct ReservationResult {
    bool          granted{false};
    common::Money held{};                // cash actually held; 0 when refused
    common::Money buying_power_after{};  // for the reject reason / metrics
};

class IReservationLedger {
public:
    virtual ~IReservationLedger();

    // Check buying power and hold against it, atomically.
    //
    // Keyed by SignalId because at approval time no Order exists yet. The hold
    // is attached to the order when that order's submission event arrives,
    // matched on domain::Order::origin_signal.
    //
    // The PORTFOLIO MANAGER sizes the hold, not the caller: a limit buy holds
    // quantity * limit_price plus estimated fees; a market buy holds the last
    // mark plus a buffer (5% by default) plus fees, and is refused if the
    // symbol has no mark yet.
    //
    // A sell holds no cash; it commits `quantity` against the position, and
    // is refused beyond the shares held and not already committed (no
    // short selling at approval). A non-positive quantity is refused.
    [[nodiscard]] virtual ReservationResult hold_for_signal(
        common::SignalId signal,
        const common::Symbol& symbol,
        domain::OrderSide side,
        common::Quantity quantity,
        std::optional<common::Price> limit_price) = 0;

    // Release a hold whose signal never became an order.
    //
    // The ordinary paths do not need this: a rejected order still carries
    // origin_signal, so apply_order_update() releases the hold even when the
    // order never reached Working. This is for the case where the
    // ExecutionSimulator drops an approved signal without emitting anything.
    virtual void release_signal_hold(common::SignalId signal) = 0;
};

}  // namespace trading_engine::portfolio
