#include "trading_engine/risk/risk_policy.hpp"

#include <algorithm>
#include <string>

#include "trading_engine/common/decimal_text.hpp"

namespace trading_engine::risk {

IRiskPolicy::~IRiskPolicy() = default;

namespace {

bool is_buy(const domain::TradeSignal& s) { return s.side == domain::SignalSide::Buy; }

common::Quantity requested(const domain::TradeSignal& s) { return s.requested_quantity.value_or(0); }

common::Quantity held(const domain::PortfolioSnapshot& p, const common::Symbol& symbol) {
    for (const auto& pos : p.positions) {
        if (pos.symbol == symbol) {
            return pos.quantity;
        }
    }
    return 0;
}

std::string money(common::Money m) { return common::format_decimal(m); }

}  // namespace

RiskDecision MaxOrderQuantityPolicy::evaluate(const domain::TradeSignal& signal,
                                              const domain::PortfolioSnapshot&,
                                              const RiskContext& context) const {
    const common::Quantity limit = context.limits.max_order_quantity;
    if (requested(signal) <= limit) {
        return RiskDecision::allow();
    }
    if (limit <= 0) {
        return RiskDecision::deny("max_order_quantity is 0");
    }
    return {true,
            "resized from " + std::to_string(requested(signal)) + " to max_order_quantity " +
                std::to_string(limit),
            limit};
}

RiskDecision MaxPositionNotionalPolicy::evaluate(const domain::TradeSignal& signal,
                                                 const domain::PortfolioSnapshot& portfolio,
                                                 const RiskContext& context) const {
    if (!is_buy(signal)) {
        return RiskDecision::allow();
    }
    const auto price = context.reference_prices.find(signal.symbol);
    if (price == context.reference_prices.end()) {
        return RiskDecision::deny("no reference price for " + signal.symbol);
    }
    const common::Money after = price->second * (held(portfolio, signal.symbol) + requested(signal));
    if (after > context.limits.max_position_notional) {
        return RiskDecision::deny("position would be " + money(after) +
                                  ", above max_position_notional " +
                                  money(context.limits.max_position_notional));
    }
    return RiskDecision::allow();
}

RiskDecision MaxGrossExposurePolicy::evaluate(const domain::TradeSignal& signal,
                                              const domain::PortfolioSnapshot& portfolio,
                                              const RiskContext& context) const {
    if (!is_buy(signal)) {
        return RiskDecision::allow();
    }
    const auto price = context.reference_prices.find(signal.symbol);
    if (price == context.reference_prices.end()) {
        return RiskDecision::deny("no reference price for " + signal.symbol);
    }
    const common::Money after = portfolio.gross_exposure + price->second * requested(signal);
    if (after > context.limits.max_gross_exposure) {
        return RiskDecision::deny("gross exposure would be " + money(after) +
                                  ", above max_gross_exposure " +
                                  money(context.limits.max_gross_exposure));
    }
    return RiskDecision::allow();
}

RiskDecision MaxOpenOrdersPolicy::evaluate(const domain::TradeSignal&,
                                           const domain::PortfolioSnapshot&,
                                           const RiskContext& context) const {
    if (context.open_order_count >= context.limits.max_open_orders) {
        return RiskDecision::deny(std::to_string(context.open_order_count) +
                                  " orders already open (max_open_orders " +
                                  std::to_string(context.limits.max_open_orders) + ")");
    }
    return RiskDecision::allow();
}

RiskDecision DailyLossPolicy::evaluate(const domain::TradeSignal& signal,
                                       const domain::PortfolioSnapshot&,
                                       const RiskContext& context) const {
    if (is_buy(signal) && -context.pnl_today >= context.limits.max_daily_loss) {
        return RiskDecision::deny("session loss " + money(-context.pnl_today) +
                                  " has reached max_daily_loss " +
                                  money(context.limits.max_daily_loss));
    }
    return RiskDecision::allow();
}

std::vector<std::shared_ptr<IRiskPolicy>> starter_policies() {
    return {std::make_shared<MaxOrderQuantityPolicy>(), std::make_shared<MaxPositionNotionalPolicy>(),
            std::make_shared<MaxGrossExposurePolicy>(), std::make_shared<MaxOpenOrdersPolicy>(),
            std::make_shared<DailyLossPolicy>()};
}

}  // namespace trading_engine::risk
