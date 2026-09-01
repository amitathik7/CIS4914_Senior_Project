#pragma once

// -----------------------------------------------------------------------------
//  IRiskPolicy -- interface for a single, composable risk rule.
//
//  Responsibility: answer one question about one signal -- "does allowing this
//  breach my limit?" -- given the signal and a snapshot of current portfolio
//  state. Examples: max position notional, max order size, max gross exposure,
//  daily loss stop, max open orders.
//
//  Contract: evaluate() must be a pure function of its inputs, side-effect
//  free, and safe to call concurrently. Policies are owned by the RiskManager
//  as shared_ptr and live for the whole run.
// -----------------------------------------------------------------------------

#include <optional>
#include <string>
#include <string_view>

#include "trading_engine/common/types.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/portfolio_snapshot.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace trading_engine::risk {

// Extra inputs a policy might need beyond the signal and portfolio snapshot.
struct RiskContext {
    config::RiskLimits limits{};
    std::uint32_t      open_order_count{0};
    common::Money       realized_pnl_today{0};
    common::Timestamp   as_of{};
    // TODO: reference prices for notional math, per-symbol/strategy overrides,
    //       pending (not-yet-filled) exposure.
};

struct RiskDecision {
    bool        approved{false};
    std::string reason{};                         // human-readable, always set
    std::optional<common::Quantity> adjusted_quantity{};  // set => resize, don't reject

    static RiskDecision allow() { return {true, "ok", std::nullopt}; }
    static RiskDecision deny(std::string why) { return {false, std::move(why), std::nullopt}; }
};

class IRiskPolicy {
public:
    virtual ~IRiskPolicy();

    [[nodiscard]] virtual std::string_view name() const = 0;

    [[nodiscard]] virtual RiskDecision evaluate(
        const domain::TradeSignal& signal,
        const domain::PortfolioSnapshot& portfolio,
        const RiskContext& context) const = 0;
};

}  // namespace trading_engine::risk
