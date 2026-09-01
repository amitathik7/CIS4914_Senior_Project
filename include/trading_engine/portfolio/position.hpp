#pragma once

// -----------------------------------------------------------------------------
//  Position -- the engine's holding in a single instrument.
//
//  Responsibility: a snapshot of quantity and cost basis for one symbol, plus
//  realised and (mark-dependent) unrealised P&L. Owned and mutated only by the
//  PortfolioManager; every other component receives it by value inside a
//  PortfolioSnapshot.
//
//  Plain value type. No P&L math lives here yet -- the scaffold does not
//  compute or fabricate P&L (see PortfolioManager).
// -----------------------------------------------------------------------------

#include "trading_engine/common/types.hpp"

namespace trading_engine::portfolio {

struct Position {
    common::Symbol   symbol{};
    common::Quantity quantity{0};       // signed: > 0 long, < 0 short, 0 flat
    common::Price    average_cost{0};   // cost basis per unit for the open qty
    common::Money    realized_pnl{0};   // locked in by closing trades
    common::Money    unrealized_pnl{0}; // mark-to-market on the open qty

    [[nodiscard]] bool is_flat() const noexcept { return quantity == 0; }

    // TODO: cost-basis method (average / FIFO / LIFO), last mark price and
    //       mark time, accumulated fees, opened_at, and per-lot detail for
    //       tax-style reporting.
};

}  // namespace trading_engine::portfolio
