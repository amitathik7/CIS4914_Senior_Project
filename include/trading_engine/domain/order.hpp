#pragma once

// -----------------------------------------------------------------------------
//  Order -- a risk-approved instruction to trade, tracked through its lifecycle.
//
//  Responsibility: the unit the ExecutionSimulator works on. Produced from an
//  approved TradeSignal by a sizing/routing step; updated as (simulated) fills
//  arrive.
//
//  Plain value type. The owning component (ExecutionSimulator) is responsible
//  for all state transitions; nothing here enforces the state machine yet.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::domain {

enum class OrderSide : std::uint8_t { Buy = 0, Sell };

enum class OrderType : std::uint8_t {
    Market = 0,
    Limit
    // TODO: Stop, StopLimit, MarketOnClose, Pegged ...
};

// PROVISIONAL lifecycle. The exact state machine (and who may drive each
// transition) is an open question -- see docs/OPEN_QUESTIONS.md.
enum class OrderStatus : std::uint8_t {
    New = 0,          // created, not yet risk-checked
    PendingRisk,      // handed to RiskManager
    Rejected,         // risk or validation rejected it
    Approved,         // cleared risk, not yet "working"
    Working,          // live in the (simulated) market
    PartiallyFilled,
    Filled,
    Cancelled,
    Expired
};

[[nodiscard]] std::string_view to_string(OrderSide) noexcept;
[[nodiscard]] std::string_view to_string(OrderType) noexcept;
[[nodiscard]] std::string_view to_string(OrderStatus) noexcept;

struct Order {
    common::OrderId  id{};
    common::SignalId origin_signal{};   // signal this order was derived from
    std::string      strategy_id{};
    common::Symbol   symbol{};
    OrderSide        side{OrderSide::Buy};
    OrderType        type{OrderType::Market};
    common::Quantity quantity{0};                    // ordered amount, > 0
    std::optional<common::Price> limit_price{};      // set iff type == Limit
    OrderStatus      status{OrderStatus::New};

    common::Timestamp created_at{};   // UTC
    common::Timestamp updated_at{};   // UTC, last status change

    // Running fill state, maintained by the ExecutionSimulator.
    common::Quantity filled_quantity{0};
    std::optional<common::Price> average_fill_price{};

    // TODO: client_order_id for idempotency, time_in_force, venue/route,
    //       parent_id for child slices, cancel/replace history, reject reason.
};

}  // namespace trading_engine::domain
