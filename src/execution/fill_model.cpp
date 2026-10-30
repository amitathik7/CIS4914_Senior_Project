#include "trading_engine/execution/fill_model.hpp"

#include <cmath>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::execution {

IFillModel::~IFillModel() = default;

std::string_view NaiveFillModel::name() const {
    return "naive";
}

// Fees for one fill: per trade + per share * |qty| + bps of the gross notional.
common::Money fee_for(common::Quantity quantity, common::Price price,
                      const config::FeeModelConfig& fees) {
    const common::Quantity shares = quantity < 0 ? -quantity : quantity;
    common::Money total = fees.per_trade + fees.per_share * shares;
    if (fees.bps != 0.0) {
        total += (price * shares).scaled_by(fees.bps / 10'000.0);
    }
    return total;
}

std::vector<domain::Fill> NaiveFillModel::simulate(
    const domain::Order& order,
    const MarketContext& market,
    const config::SimulationConfig& config) const {
    const common::Quantity remaining = order.quantity - order.filled_quantity;
    if (remaining <= 0) {
        return {};
    }
    if (!market.last_price || *market.last_price <= common::Price{}) {
        throw common::ValidationError("NaiveFillModel::simulate: no reference price for " +
                                      order.symbol);
    }
    const common::Price reference = *market.last_price;
    const bool buy = order.side == domain::OrderSide::Buy;

    // Slippage always works against the order: buys pay up, sells receive less.
    common::Price slip = reference.scaled_by(config.slippage_bps / 10'000.0);
    if (slip < common::Price{}) {
        slip = -slip;
    }
    common::Price price = buy ? reference + slip : reference - slip;

    if (order.type == domain::OrderType::Limit && order.limit_price) {
        // A limit that the reference price does not reach does not fill on this bar.
        if (buy ? reference > *order.limit_price : reference < *order.limit_price) {
            return {};
        }
        price = buy ? std::min(price, *order.limit_price) : std::max(price, *order.limit_price);
    }
    if (price <= common::Price{}) {
        return {};
    }

    domain::Fill fill;
    fill.order_id        = order.id;
    fill.run_id          = order.run_id;
    fill.symbol          = order.symbol;
    fill.filled_quantity = buy ? remaining : -remaining;
    fill.fill_price      = price;
    fill.fees            = fee_for(remaining, price, config.fees);
    fill.slippage        = price - reference;
    fill.filled_at       = market.as_of;
    fill.status          = domain::FillStatus::Filled;
    return {fill};
}

}  // namespace trading_engine::execution
