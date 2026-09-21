#pragma once

// -----------------------------------------------------------------------------
//  PendingOrder -- one unresolved order, as the Portfolio Manager sees it.
//
//  Responsibility: the Portfolio Manager's view of an order it has reserved
//  against but that has not yet fully filled, been rejected, cancelled or
//  expired. Carries exactly what issue #5 asks the Risk Manager to see
//  (symbol, side, quantity) plus the cash held against it.
//
//  PROPOSED by docs/adr/0005-portfolio-risk-query-contract.md section 4, which
//  records the team decision (2026-09-20) that the Portfolio Manager reserves
//  cash at order submission.
//
//  This is a PROJECTION, not the order itself. The Execution Simulator owns
//  the authoritative order table; this copy is built only from its order
//  events and fills (ADR 0004 sections 7-8), so it is only as right as those
//  events are complete and ordered.
//
//  Plain value type. Owned and mutated only by the PortfolioManager; every
//  other component receives it by value inside a PortfolioSnapshot.
// -----------------------------------------------------------------------------

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/order.hpp"

namespace trading_engine::portfolio {

struct PendingOrder {
    common::OrderId   order_id{};

    // The signal whose hold this order inherited. The hold is placed at
    // approval keyed by SignalId, then attached here when the submission
    // event arrives (ADR 0005 section 4).
    common::SignalId  origin_signal{};

    common::Symbol    symbol{};
    domain::OrderSide side{domain::OrderSide::Buy};

    // Unfilled part of the order, > 0. Falls as fills arrive; the entry is
    // removed when it reaches zero or the order ends without filling.
    common::Quantity  remaining_quantity{0};

    // Cash held against the remaining quantity. Buys only: a pending sell
    // reserves no cash, it commits remaining_quantity against the position
    // instead (ADR 0005 section 4). How much a market buy reserves is still
    // open.
    common::Money     reserved_cash{0};
};

}  // namespace trading_engine::portfolio
