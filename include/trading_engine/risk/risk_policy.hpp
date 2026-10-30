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

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/portfolio_snapshot.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace trading_engine::risk {

// Extra inputs a policy might need beyond the signal and portfolio snapshot.
struct RiskContext {
    config::RiskLimits limits{};
    std::uint32_t      open_order_count{0};
    common::Money       realized_pnl_today{};
    common::Timestamp   as_of{};
    // Equity now minus equity at the start of the current session (UTC
    // calendar day of the market data), realised and unrealised together.
    common::Money       pnl_today{};
    // Last known price per symbol, for notional math. A symbol missing here
    // has never traded in this run.
    std::map<common::Symbol, common::Price> reference_prices{};
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

// --- Starter policies (IMPLEMENTATION_PLAN M6) ----------------------------
// Each reads only its inputs. Sells that reduce a long position always pass
// the exposure policies: they lower exposure.

// Resizes a signal above limits.max_order_quantity down to it.
class MaxOrderQuantityPolicy final : public IRiskPolicy {
public:
    [[nodiscard]] std::string_view name() const override { return "max_order_quantity"; }
    [[nodiscard]] RiskDecision evaluate(const domain::TradeSignal& signal,
                                        const domain::PortfolioSnapshot& portfolio,
                                        const RiskContext& context) const override;
};

// Rejects a buy whose resulting position would be worth more than
// limits.max_position_notional at the reference price.
class MaxPositionNotionalPolicy final : public IRiskPolicy {
public:
    [[nodiscard]] std::string_view name() const override { return "max_position_notional"; }
    [[nodiscard]] RiskDecision evaluate(const domain::TradeSignal& signal,
                                        const domain::PortfolioSnapshot& portfolio,
                                        const RiskContext& context) const override;
};

// Rejects a buy that would take gross exposure above limits.max_gross_exposure.
class MaxGrossExposurePolicy final : public IRiskPolicy {
public:
    [[nodiscard]] std::string_view name() const override { return "max_gross_exposure"; }
    [[nodiscard]] RiskDecision evaluate(const domain::TradeSignal& signal,
                                        const domain::PortfolioSnapshot& portfolio,
                                        const RiskContext& context) const override;
};

// Rejects any signal while limits.max_open_orders orders are already open.
class MaxOpenOrdersPolicy final : public IRiskPolicy {
public:
    [[nodiscard]] std::string_view name() const override { return "max_open_orders"; }
    [[nodiscard]] RiskDecision evaluate(const domain::TradeSignal& signal,
                                        const domain::PortfolioSnapshot& portfolio,
                                        const RiskContext& context) const override;
};

// Rejects buys once the session's loss reaches limits.max_daily_loss; sells
// (which can only reduce a long here) stay allowed. Resets each session.
class DailyLossPolicy final : public IRiskPolicy {
public:
    [[nodiscard]] std::string_view name() const override { return "daily_loss"; }
    [[nodiscard]] RiskDecision evaluate(const domain::TradeSignal& signal,
                                        const domain::PortfolioSnapshot& portfolio,
                                        const RiskContext& context) const override;
};

// The five policies above, in that order.
[[nodiscard]] std::vector<std::shared_ptr<IRiskPolicy>> starter_policies();

}  // namespace trading_engine::risk
