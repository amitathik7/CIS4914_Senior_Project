#pragma once

// -----------------------------------------------------------------------------
//  TradeSignal -- a strategy's intent, before any risk check or sizing policy.
//
//  Responsibility: carry "strategy X wants exposure Y in symbol Z" from the
//  StrategyEngine to the RiskManager. A signal is a request, not an order: it
//  may be rejected, resized, or split downstream.
//
//  Plain value type. Not thread-safe; immutable once emitted.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/order.hpp"

namespace trading_engine::domain {

enum class SignalSide : std::uint8_t {
    Buy = 0,
    Sell,
    Flat   // close / go to zero
};

[[nodiscard]] std::string_view to_string(SignalSide) noexcept;

struct TradeSignal {
    common::SignalId  id{};
    std::string       strategy_id{};   // stable name of the emitting strategy
    common::Symbol    symbol{};
    SignalSide        side{SignalSide::Flat};
    common::Timestamp created_at{};     // UTC

    // Exactly one sizing intent is expected to be set; leaving both empty means
    // "let the sizing policy decide". Enforced later, not here.
    std::optional<common::Quantity> requested_quantity{};
    std::optional<double>           target_exposure{};   // fraction of equity, e.g. 0.05

    std::optional<double> confidence{};   // model-defined score in [0, 1]

    // Free-form, string-keyed metadata. Deliberately NOT a JSON type -- the
    // core domain must not depend on a particular JSON library.
    // TODO: consider a small variant value type if typed metadata is needed.
    std::map<std::string, std::string> metadata{};

    // --- Order-shape hint --------------------------------------------------
    // PROPOSED by docs/adr/0002-strategy-risk-signal-contract.md (status:
    // Proposed, not Accepted). Only meaningful alongside `requested_quantity`
    // -- an exposure-based signal (target_exposure set instead) has no order
    // shape and should leave both fields below empty, exactly like every
    // signal built before this pair existed.
    //
    // No component reads these two fields yet (StrategyEngine, RiskManager,
    // and ExecutionSimulator are all still common::NotImplemented), so right
    // now "absent" and "ignored" are the same outcome.
    std::optional<OrderType>     order_type{};
    std::optional<common::Price> limit_price{};  // set iff order_type == Limit; enforced later, not here

    // TODO: time-in-force / expiry, target price band, urgency, parent
    //       portfolio-construction batch id, per-signal risk overrides.
};

}  // namespace trading_engine::domain
