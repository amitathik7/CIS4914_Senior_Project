#pragma once

// -----------------------------------------------------------------------------
//  Fill -- one (simulated) execution against an Order.
//
//  Responsibility: the event the PortfolioManager consumes to update cash,
//  positions and realised P&L. Produced only by the ExecutionSimulator via an
//  injected FillModel; the scaffold never fabricates fills.
//
//  A Fill reports quantity that ACTUALLY EXECUTED. An order that never
//  executed produces no Fill at all -- rejections, cancellations and
//  expiries are Order lifecycle events, not fills. See
//  docs/adr/0004-execution-portfolio-fill-contract.md.
//
//  Plain value type. Immutable once produced.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string_view>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::domain {

// PROPOSED by docs/adr/0004-execution-portfolio-fill-contract.md (status:
// Proposed, not Accepted).
//
// The state of the ORDER as of this fill -- not the state of the fill itself.
// There is deliberately no "failed"/"rejected" member: see the file header.
// The two spellings match OrderStatus's own, because the same word means the
// same thing on both structs.
enum class FillStatus : std::uint8_t {
    Filled = 0,       // this fill completed the order's remaining quantity
    PartiallyFilled   // quantity remains on the order after this fill
};

[[nodiscard]] std::string_view to_string(FillStatus) noexcept;

struct Fill {
    // Identity, and the idempotency key. A consumer that has already applied
    // this id must not apply it again -- under an at-least-once transport the
    // same Fill can legitimately arrive more than once. Unique per run.
    common::FillId    id{};

    common::OrderId   order_id{};

    // PROPOSED by ADR 0004. In-process the consumer already knows which run it
    // belongs to, so this is redundant; across a shared topic it is the only
    // way to tell two concurrent runs apart. Already required by the draft
    // `fills` table.
    common::RunId     run_id{};

    common::Symbol    symbol{};

    // Signed: +buy / -sell. This sign is the SINGLE source of truth for
    // direction in C++ -- there is deliberately no separate `side` member to
    // contradict it. (The serialized form in ADR 0004 does carry an explicit
    // side plus a positive magnitude; the mapping is documented there.)
    common::Quantity  filled_quantity{0};

    // GROSS execution price per unit -- fees are NOT folded in; see `fees`.
    // Kept gross so it stays comparable with market prices for slippage
    // analysis. Cash impact is computed as:
    //     cash_delta = -(filled_quantity * fill_price) - fees
    common::Price     fill_price{0};

    // Commissions + exchange fees for THIS fill, >= 0. Always reduces cash,
    // on buys and sells alike. Omitted from the issue #4 field list, but the
    // cash math above is wrong without it.
    common::Money     fees{0};

    // fill_price - reference_price (modelled). INFORMATIONAL / derived: it is
    // already embedded in fill_price and must NOT be applied to cash again.
    common::Price     slippage{0};

    common::Timestamp filled_at{};          // UTC

    // PROPOSED by ADR 0004. Stamped by the ExecutionSimulator, the only
    // component that knows the originating order's remaining quantity. This
    // is DERIVED data -- the authoritative running total lives on
    // Order::filled_quantity -- so the two can disagree if a fill is replayed
    // or applied out of order. Nothing enforces agreement yet.
    FillStatus        status{FillStatus::Filled};

    // PROPOSED by ADR 0004. 1-based position of this fill within its order's
    // fill series, so a consumer can detect a MISSING fill rather than
    // silently mis-compute a cost basis. Redundant when the transport already
    // guarantees ordered, exactly-once delivery -- flagged in the ADR as the
    // one field a reviewer may reasonably cut.
    std::uint32_t     sequence{0};

    // TODO: liquidity flag (maker/taker), reference price used for slippage,
    //       venue, and settlement date.
};

}  // namespace trading_engine::domain
