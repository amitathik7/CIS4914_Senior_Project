// Out-of-line definitions for the domain model: currently just the enum
// name helpers. These are real, total functions -- safe to call, no fake data.

#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace trading_engine::domain {

std::string_view to_string(MarketEventType type) noexcept {
    switch (type) {
        case MarketEventType::Unknown: return "unknown";
        case MarketEventType::Trade:   return "trade";
        case MarketEventType::Quote:   return "quote";
        case MarketEventType::Bar:     return "bar";
        case MarketEventType::Status:  return "status";
    }
    return "invalid";
}

std::string_view to_string(SignalSide side) noexcept {
    switch (side) {
        case SignalSide::Buy:  return "buy";
        case SignalSide::Sell: return "sell";
        case SignalSide::Flat: return "flat";
    }
    return "invalid";
}

std::string_view to_string(OrderSide side) noexcept {
    switch (side) {
        case OrderSide::Buy:  return "buy";
        case OrderSide::Sell: return "sell";
    }
    return "invalid";
}

std::string_view to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::Market: return "market";
        case OrderType::Limit:  return "limit";
    }
    return "invalid";
}

std::string_view to_string(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::New:             return "new";
        case OrderStatus::PendingRisk:     return "pending_risk";
        case OrderStatus::Rejected:        return "rejected";
        case OrderStatus::Approved:        return "approved";
        case OrderStatus::Working:         return "working";
        case OrderStatus::PartiallyFilled: return "partially_filled";
        case OrderStatus::Filled:          return "filled";
        case OrderStatus::Cancelled:       return "cancelled";
        case OrderStatus::Expired:         return "expired";
    }
    return "invalid";
}

// These two strings are the serialized `fill_status` values in
// docs/adr/0004-execution-portfolio-fill-contract.md, and match the
// OrderStatus spellings above deliberately -- the same word means the same
// thing on both structs.
std::string_view to_string(FillStatus status) noexcept {
    switch (status) {
        case FillStatus::Filled:          return "filled";
        case FillStatus::PartiallyFilled: return "partially_filled";
    }
    return "invalid";
}

}  // namespace trading_engine::domain
