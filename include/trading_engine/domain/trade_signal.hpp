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

    // TODO: time-in-force / expiry, target price band, urgency, parent
    //       portfolio-construction batch id, per-signal risk overrides.
};

}  // namespace trading_engine::domain
