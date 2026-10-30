#pragma once

// -----------------------------------------------------------------------------
//  RiskDecisionReport -- what the RiskManager decided about one TradeSignal.
//
//  Responsibility: the published record of a risk decision, so consumers that
//  are not in the process (the web gateway, persistence) can show why a
//  signal did or did not become an order. Published by the RiskManager on
//  EventType::RiskDecision (topic <prefix>.risk_decision), one per signal it
//  evaluates. The field names follow the console's RiskDecision in
//  web/src/api/contract.ts, so the gateway can pass it through.
//
//  Plain value type. Immutable once produced.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace trading_engine::domain {

enum class RiskOutcome : std::uint8_t {
    Approved = 0,   // passed unchanged
    Resized,        // passed with a smaller quantity
    Rejected
};

[[nodiscard]] std::string_view to_string(RiskOutcome) noexcept;

// One policy's verdict. Policies run in order and the first rejection stops
// the run, so a rejected report's last check is the failing one.
struct RiskCheck {
    std::string                policy{};    // IRiskPolicy::name(), or "buying_power"
    bool                       passed{true};
    std::string                detail{};    // the policy's reason text
    std::optional<std::string> limit{};     // reserved: the configured limit, as text
    std::optional<std::string> observed{};  // reserved: the value that was compared
};

struct RiskDecisionReport {
    std::uint64_t     id{0};                // 1-based, per RiskManager
    common::RunId     run_id{};             // as on Order / Fill
    common::SignalId  signal_id{};
    common::Timestamp decided_at{};         // UTC
    common::Symbol    symbol{};
    SignalSide        side{SignalSide::Flat};
    std::string       strategy_id{};
    RiskOutcome       outcome{RiskOutcome::Rejected};
    common::Quantity  requested_quantity{0};   // as the strategy asked (0 if unset)
    common::Quantity  approved_quantity{0};    // 0 when rejected
    std::optional<std::string> reason{};       // the deciding policy; unset when approved unchanged
    std::vector<RiskCheck> checks{};
    // The order is created after the decision, by the ExecutionSimulator, so
    // this stays unset; join on Order::origin_signal == signal_id instead.
    std::optional<common::OrderId> order_id{};
};

}  // namespace trading_engine::domain
