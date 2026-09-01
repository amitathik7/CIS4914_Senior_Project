#pragma once

// -----------------------------------------------------------------------------
//  PortfolioManager -- single source of truth for cash, positions and P&L.
//
//  Responsibility: consume Fills to update cash, position quantity and cost
//  basis, and realised P&L; consume MarketEvents to re-mark open positions for
//  unrealised P&L; expose an immutable PortfolioSnapshot for the RiskManager,
//  analytics, and persistence.
//
//  IPortfolioView is the read side, deliberately narrow, so consumers (notably
//  RiskManager) depend only on "give me the current snapshot", not on the
//  mutating API.
//
//  Ownership / lifecycle: owned by TradingEngine; one of the first components
//  created and the last destroyed, since others hold IPortfolioView& to it.
//
//  Thread-safety: writes (apply/mark) arrive on the execution/bus thread;
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
    [[nodiscard]] virtual common::Money cash() const = 0;
};

class PortfolioManager final : public IPortfolioView {
public:
    PortfolioManager(common::RunId run_id,
                     common::Money starting_cash,
                     const common::IClock& clock);
    ~PortfolioManager() override;

    // --- write side (mutating) ---------------------------------------
    // Apply a simulated execution. NOT IMPLEMENTED (throws).
    void apply(const domain::Fill& fill);
    // Re-mark open positions from a market event. NOT IMPLEMENTED.
    void mark(const domain::MarketEvent& event);

    // --- read side (IPortfolioView) --------------------------------
    [[nodiscard]] domain::PortfolioSnapshot snapshot() const override;   // NOT IMPLEMENTED
    [[nodiscard]] std::optional<Position> position(
        const common::Symbol& symbol) const override;                    // NOT IMPLEMENTED
    [[nodiscard]] common::Money cash() const override;                   // NOT IMPLEMENTED

private:
    [[maybe_unused]] common::RunId         run_id_{};
    [[maybe_unused]] common::Money         starting_cash_{0};
    [[maybe_unused]] const common::IClock& clock_;

    // TODO: symbol -> Position map, running cash balance, realised-P&L ledger,
    //       last mark price per symbol, exposure aggregates, and the
    //       synchronisation primitive that makes the read side thread-safe.
};

}  // namespace trading_engine::portfolio
