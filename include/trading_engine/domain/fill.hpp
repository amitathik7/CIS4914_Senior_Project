#pragma once

// -----------------------------------------------------------------------------
//  Fill -- one (simulated) execution against an Order.
//
//  Responsibility: the event the PortfolioManager consumes to update cash,
//  positions and realised P&L. Produced only by the ExecutionSimulator via an
//  injected FillModel; the scaffold never fabricates fills.
//
//  Plain value type. Immutable once produced.
// -----------------------------------------------------------------------------

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::domain {

struct Fill {
    common::FillId    id{};
    common::OrderId   order_id{};
    common::Symbol    symbol{};
    common::Quantity  filled_quantity{0};   // signed: +buy / -sell
    common::Price     fill_price{0};        // price actually paid/received
    common::Money     fees{0};              // commissions + exchange fees, >= 0
    common::Price     slippage{0};          // fill_price - reference_price (modelled)
    common::Timestamp filled_at{};          // UTC

    // TODO: liquidity flag (maker/taker), reference price used for slippage,
    //       partial-fill sequence number, venue, and settlement date.
};

}  // namespace trading_engine::domain
