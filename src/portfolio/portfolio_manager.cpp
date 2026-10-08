#include "trading_engine/portfolio/portfolio_manager.hpp"

#include <algorithm>
#include <cstdlib>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::portfolio {

IPortfolioView::~IPortfolioView() = default;
IReservationLedger::~IReservationLedger() = default;

PortfolioManager::PortfolioManager(common::RunId run_id,
                                   common::Money starting_cash,
                                   const common::IClock& clock,
                                   config::FeeModelConfig fees,
                                   double market_buy_buffer)
    : run_id_{run_id},
      clock_{clock},
      fees_{fees},
      market_buy_buffer_{market_buy_buffer},
      cash_{starting_cash} {}

PortfolioManager::~PortfolioManager() = default;

bool PortfolioManager::apply(const domain::Fill& fill) {
    const std::lock_guard lock{mutex_};

    if (fill.run_id != run_id_ || applied_fills_.contains(fill.id)) {
        return false;
    }
    if (fill.filled_quantity == 0) {
        throw common::ValidationError("fill has zero quantity");
    }
    if (fill.fill_price <= common::Price{}) {
        throw common::ValidationError("fill price must be positive");
    }
    if (fill.fees < common::Money{}) {
        throw common::ValidationError("fill fees must not be negative");
    }
    if (closed_orders_.contains(fill.order_id)) {
        throw common::ValidationError("fill for an order already closed");
    }
    const auto seq = last_sequence_.find(fill.order_id);
    const std::uint32_t expected = (seq == last_sequence_.end() ? 0 : seq->second) + 1;
    if (fill.sequence != expected) {
        throw common::ValidationError("fill sequence gap");
    }

    release_for_fill(fill);

    const common::Quantity qty = fill.filled_quantity;
    const common::Price price = fill.fill_price;

    cash_ -= price * qty + fill.fees;

    auto [it, inserted] = positions_.try_emplace(fill.symbol);
    Position& pos = it->second;
    if (inserted) {
        pos.symbol = fill.symbol;
    }

    // Fees are charged to realised P&L when paid, so average_cost stays a
    // pure trade price comparable with market prices.
    pos.realized_pnl -= fill.fees;

    const common::Quantity old_qty = pos.quantity;
    const common::Quantity new_qty = old_qty + qty;

    if (old_qty == 0 || (old_qty > 0) == (qty > 0)) {
        // Opening or adding: blend into the average cost.
        pos.average_cost =
            (pos.average_cost * std::abs(old_qty) + price * std::abs(qty)) /
            std::abs(new_qty);
    } else {
        // Reducing, closing or flipping: realise P&L on the closed shares.
        const common::Quantity closed = std::min(std::abs(qty), std::abs(old_qty));
        const common::Quantity direction = old_qty > 0 ? 1 : -1;
        pos.realized_pnl += (price - pos.average_cost) * (closed * direction);

        if (new_qty == 0) {
            pos.average_cost = common::Price{};
        } else if ((new_qty > 0) != (old_qty > 0)) {
            pos.average_cost = price;  // flipped: the remainder opened at this price
        }
    }
    pos.quantity = new_qty;

    marks_[fill.symbol] = price;
    applied_fills_.insert(fill.id);
    if (fill.status == domain::FillStatus::Filled) {
        last_sequence_.erase(fill.order_id);
        closed_orders_.insert(fill.order_id);  // even if its Working event never arrived
    } else {
        last_sequence_[fill.order_id] = fill.sequence;
    }
    return true;
}

bool PortfolioManager::apply_order_update(const domain::Order& order) {
    const std::lock_guard lock{mutex_};

    if (order.run_id != run_id_) {
        return false;
    }

    switch (order.status) {
    case domain::OrderStatus::Working: {
        if (open_orders_.contains(order.id) || closed_orders_.contains(order.id)) {
            return false;
        }
        PendingOrder pending{};
        pending.order_id = order.id;
        pending.origin_signal = order.origin_signal;
        pending.symbol = order.symbol;
        pending.side = order.side;
        pending.remaining_quantity = order.quantity - order.filled_quantity;
        if (pending.remaining_quantity <= 0) {
            return false;
        }

        const auto hold = holds_.find(order.origin_signal);
        if (hold != holds_.end()) {
            pending.reserved_cash = hold->second.amount;
            holds_.erase(hold);
        } else if (order.side == domain::OrderSide::Buy) {
            // No hold to take over (e.g. an order submitted directly). The
            // order is live regardless, so reserve what can be estimated.
            pending.reserved_cash =
                buy_cost(order.symbol, pending.remaining_quantity, order.limit_price)
                    .value_or(common::Money{});
        }
        open_orders_.emplace(order.id, std::move(pending));
        return true;
    }
    case domain::OrderStatus::Rejected:
    case domain::OrderStatus::Cancelled:
    case domain::OrderStatus::Expired: {
        // A rejection can arrive before the order was ever Working, while
        // its signal still holds the cash.
        bool changed = holds_.erase(order.origin_signal) > 0;
        changed = open_orders_.erase(order.id) > 0 || changed;
        closed_orders_.insert(order.id);
        return changed;
    }
    default:
        return false;
    }
}

void PortfolioManager::mark(const domain::MarketEvent& event) {
    std::optional<common::Price> price = event.price;
    if (!price && event.bid && event.ask) {
        price = (*event.bid + *event.ask) / 2;
    }
    if (!price || *price <= common::Price{} || event.symbol.empty()) {
        return;
    }

    const std::lock_guard lock{mutex_};
    marks_[event.symbol] = *price;
}

domain::PortfolioSnapshot PortfolioManager::snapshot() const {
    const std::lock_guard lock{mutex_};

    domain::PortfolioSnapshot snap{};
    snap.run_id = run_id_;
    snap.as_of = clock_.now();
    snap.cash = cash_;
    snap.reserved_cash = reserved_total();
    snap.buying_power = cash_ - snap.reserved_cash;

    snap.pending_orders.reserve(open_orders_.size());
    for (const auto& [id, order] : open_orders_) {
        snap.pending_orders.push_back(order);
    }

    snap.positions.reserve(positions_.size());
    for (const auto& [symbol, pos] : positions_) {
        const common::Money value = mark_price(pos) * pos.quantity;
        snap.net_exposure += value;
        snap.gross_exposure += value < common::Money{} ? -value : value;
        snap.positions.push_back(valued(pos));
    }
    snap.total_equity = cash_ + snap.net_exposure;
    return snap;
}

std::optional<Position> PortfolioManager::position(
    const common::Symbol& symbol) const {
    const std::lock_guard lock{mutex_};
    const auto it = positions_.find(symbol);
    if (it == positions_.end()) {
        return std::nullopt;
    }
    return valued(it->second);
}

common::Money PortfolioManager::cash() const {
    const std::lock_guard lock{mutex_};
    return cash_;
}

common::Money PortfolioManager::buying_power() const {
    const std::lock_guard lock{mutex_};
    return cash_ - reserved_total();
}

ReservationResult PortfolioManager::hold_for_signal(
    common::SignalId signal,
    const common::Symbol& symbol,
    domain::OrderSide side,
    common::Quantity quantity,
    std::optional<common::Price> limit_price) {
    const std::lock_guard lock{mutex_};

    const common::Money power = cash_ - reserved_total();

    // A retried signal must not hold twice.
    if (const auto existing = holds_.find(signal); existing != holds_.end()) {
        return {true, existing->second.amount, power};
    }
    if (!signal.valid() || quantity <= 0) {
        return {false, common::Money{}, power};
    }

    if (side == domain::OrderSide::Sell) {
        // Cash-account rule: only shares held and not already promised to
        // another sell can be sold. Sells hold no cash.
        const auto pos = positions_.find(symbol);
        const common::Quantity held = pos == positions_.end() ? 0 : pos->second.quantity;
        if (quantity > held - committed_to_sell(symbol)) {
            return {false, common::Money{}, power};
        }
        holds_.emplace(signal, Hold{symbol, side, quantity, common::Money{}});
        return {true, common::Money{}, power};
    }

    const auto cost = buy_cost(symbol, quantity, limit_price);
    if (!cost || *cost > power) {
        return {false, common::Money{}, power};
    }
    holds_.emplace(signal, Hold{symbol, side, quantity, *cost});
    return {true, *cost, power - *cost};
}

void PortfolioManager::release_signal_hold(common::SignalId signal) {
    const std::lock_guard lock{mutex_};
    holds_.erase(signal);
}

common::Price PortfolioManager::mark_price(const Position& pos) const {
    const auto it = marks_.find(pos.symbol);
    return it != marks_.end() ? it->second : pos.average_cost;
}

Position PortfolioManager::valued(const Position& pos) const {
    Position out = pos;
    out.unrealized_pnl = (mark_price(pos) - pos.average_cost) * pos.quantity;
    return out;
}

std::optional<common::Money> PortfolioManager::buy_cost(
    const common::Symbol& symbol, common::Quantity quantity,
    std::optional<common::Price> limit_price) const {
    common::Money notional{};
    if (limit_price) {
        notional = *limit_price * quantity;
    } else {
        const auto mark = marks_.find(symbol);
        if (mark == marks_.end()) {
            return std::nullopt;
        }
        notional = (mark->second * quantity).scaled_by(1.0 + market_buy_buffer_);
    }
    const common::Money fees = fees_.per_share * quantity + fees_.per_trade +
                               notional.scaled_by(common::from_basis_points(fees_.bps));
    return notional + fees;
}

common::Money PortfolioManager::reserved_total() const {
    common::Money total{};
    for (const auto& [signal, hold] : holds_) {
        total += hold.amount;
    }
    for (const auto& [id, order] : open_orders_) {
        total += order.reserved_cash;
    }
    return total;
}

common::Quantity PortfolioManager::committed_to_sell(const common::Symbol& symbol) const {
    common::Quantity total = 0;
    for (const auto& [signal, hold] : holds_) {
        if (hold.side == domain::OrderSide::Sell && hold.symbol == symbol) {
            total += hold.quantity;
        }
    }
    for (const auto& [id, order] : open_orders_) {
        if (order.side == domain::OrderSide::Sell && order.symbol == symbol) {
            total += order.remaining_quantity;
        }
    }
    return total;
}

void PortfolioManager::release_for_fill(const domain::Fill& fill) {
    const auto it = open_orders_.find(fill.order_id);
    if (it == open_orders_.end()) {
        return;  // not tracked (e.g. its Working event was lost); nothing held
    }
    PendingOrder& order = it->second;
    const common::Quantity filled = std::abs(fill.filled_quantity);

    if (fill.status == domain::FillStatus::Filled || filled >= order.remaining_quantity) {
        open_orders_.erase(it);
        closed_orders_.insert(fill.order_id);
        return;
    }
    // Release the filled share of what is held, pro rata.
    order.reserved_cash -= (order.reserved_cash * filled) / order.remaining_quantity;
    order.remaining_quantity -= filled;
}

}  // namespace trading_engine::portfolio
