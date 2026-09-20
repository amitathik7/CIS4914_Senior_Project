#pragma once

// -----------------------------------------------------------------------------
//  PortfolioManager -- single source of truth for cash, positions and P&L.
//
//  Responsibility: consume Fills to update cash, position quantity and cost
//  basis, and realised P&L; consume Order lifecycle events to reserve cash at
//  submission and release it when an order is rejected, cancelled or expires;
//  consume MarketEvents to re-mark open positions for unrealised P&L; expose an
//  immutable PortfolioSnapshot for the RiskManager, analytics, and persistence.
//
//  Functions as a traditional portfolio manager: it owns open-order state and
//  buying power (cash minus reserved), per the team decision recorded in
//  docs/adr/0004-execution-portfolio-fill-contract.md sections 7-8.
//
//  IPortfolioView is the read side, deliberately narrow, so consumers (notably
//  RiskManager) depend only on "give me the current snapshot", not on the
//  mutating API.
//
//  Ownership / lifecycle: owned by TradingEngine; one of the first components
//  created and the last destroyed, since others hold IPortfolioView& to it.
//
//  Thread-safety: writes (apply/apply_order_update/mark) arrive on the
//  execution/bus thread;
//  reads (snapshot/position) come from the RiskManager thread. The future
//  implementation MUST make reads safe against concurrent writes -- e.g. a
//  seqlock or a mutex-guarded copy. The scaffold does neither yet.
// -----------------------------------------------------------------------------

#include <optional>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/portfolio_snapshot.hpp"
#include "trading_engine/portfolio/position.hpp"

namespace trading_engine::portfolio {

// Read-only view of portfolio state. Implemented by PortfolioManager.
class IPortfolioView {
public:
    virtual ~IPortfolioView();

    [[nodiscard]] virtual domain::PortfolioSnapshot snapshot() const = 0;
    [[nodiscard]] virtual std::optional<Position> position(
        const common::Symbol& symbol) const = 0;

    // Settled cash. Moves only when fills settle.
    [[nodiscard]] virtual common::Money cash() const = 0;

    // cash() minus cash reserved against open buy orders: what is actually
    // free to commit to a new buy. PROPOSED by
    // docs/adr/0005-portfolio-risk-query-contract.md sections 4 and 6.
    [[nodiscard]] virtual common::Money buying_power() const = 0;
};

class PortfolioManager final : public IPortfolioView {
public:
    PortfolioManager(common::RunId run_id,
                     common::Money starting_cash,
                     const common::IClock& clock);
    ~PortfolioManager() override;

    // --- write side (mutating) ---------------------------------------
    // Two entry points for execution results, per
    // docs/adr/0004-execution-portfolio-fill-contract.md section 13.

    // Apply a simulated execution. NOT IMPLEMENTED (throws).
    //
    // Updates cash and positions, and releases the reservation held for the
    // filled quantity. A fill whose status is Filled releases whatever
    // reservation remains on its order.
    //
    // Idempotent on domain::Fill::id. Returns true if the fill was applied,
    // false if it was recognised as an already-applied duplicate and ignored.
    // A duplicate is an EXPECTED condition under an at-least-once transport,
    // not an error, which is why this returns bool rather than throwing.
    // Applying the same FillId twice would silently corrupt cash and cost
    // basis, so the check is part of the contract, not an optimisation.
    bool apply(const domain::Fill& fill);

    // Apply an order lifecycle event. NOT IMPLEMENTED (throws).
    //
    // Working (the submission event): add to open orders and reserve cash for
    // a buy, or commit shares for a sell. Rejected: release the whole
    // reservation. Cancelled / Expired: release the reservation on the
    // unfilled remainder. PartiallyFilled / Filled: ignored, because fills
    // drive those and acting on both would release the same cash twice.
    //
    // Idempotent by state, keyed on Order::id: a repeated submission, a
    // release for an order already released, and a late submission for an
    // order already complete are all ignored. Returns true if state changed,
    // false otherwise.
    bool apply_order_update(const domain::Order& order);
    // Re-mark open positions from a market event. NOT IMPLEMENTED.
    void mark(const domain::MarketEvent& event);

    // --- read side (IPortfolioView) --------------------------------
    [[nodiscard]] domain::PortfolioSnapshot snapshot() const override;   // NOT IMPLEMENTED
    [[nodiscard]] std::optional<Position> position(
        const common::Symbol& symbol) const override;                    // NOT IMPLEMENTED
    [[nodiscard]] common::Money cash() const override;                   // NOT IMPLEMENTED
    [[nodiscard]] common::Money buying_power() const override;           // NOT IMPLEMENTED

private:
    [[maybe_unused]] common::RunId         run_id_{};
    [[maybe_unused]] common::Money         starting_cash_{0};
    [[maybe_unused]] const common::IClock& clock_;

    // TODO: symbol -> Position map, running cash balance, realised-P&L ledger,
    //       last mark price per symbol, exposure aggregates, open-order table
    //       keyed by OrderId with its reserved cash, the set of completed
    //       order ids (so late events cannot re-reserve), and the
    //       synchronisation primitive that makes the read side thread-safe.
};

}  // namespace trading_engine::portfolio
