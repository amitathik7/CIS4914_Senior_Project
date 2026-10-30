#include "trading_engine/execution/execution_simulator.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"
#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::execution {

ExecutionSimulator::ExecutionSimulator(const IFillModel& fill_model,
                                       events::IEventBus& bus,
                                       const common::IClock& clock,
                                       config::SimulationConfig config,
                                       common::RunId run_id)
    : fill_model_{fill_model},
      bus_{bus},
      clock_{clock},
      config_{std::move(config)},
      run_id_{run_id} {}

ExecutionSimulator::~ExecutionSimulator() {
    stop();
}

void ExecutionSimulator::start() {
    const std::lock_guard lock{mutex_};
    if (subscription_) {
        return;
    }
    subscription_ = bus_.subscribe(events::EventType::MarketData, [this](const events::Event& e) {
        if (const auto* event = std::get_if<domain::MarketEvent>(&e.payload)) {
            on_market_event(*event);
        }
    });
}

void ExecutionSimulator::stop() {
    std::optional<common::SubscriptionId> id;
    {
        const std::lock_guard lock{mutex_};
        id = std::exchange(subscription_, std::nullopt);
    }
    if (id) {
        bus_.unsubscribe(*id);
    }
}

common::OrderId ExecutionSimulator::submit(const domain::TradeSignal& signal) {
    domain::Order order;
    order.run_id        = run_id_;
    order.origin_signal = signal.id;
    order.strategy_id   = signal.strategy_id;
    order.symbol        = signal.symbol;
    order.side          = signal.side == domain::SignalSide::Sell ? domain::OrderSide::Sell
                                                                  : domain::OrderSide::Buy;
    order.type          = signal.order_type.value_or(domain::OrderType::Market);
    order.limit_price   = order.type == domain::OrderType::Limit ? signal.limit_price : std::nullopt;
    order.quantity      = signal.requested_quantity.value_or(0);

    std::string reject;
    if (signal.side == domain::SignalSide::Flat) {
        reject = "flat signals are not supported by the simulator";
    } else if (order.quantity <= 0) {
        reject = "the signal has no positive requested_quantity";
    } else if (order.type == domain::OrderType::Limit && !order.limit_price) {
        reject = "a limit signal without a limit_price";
    }
    if (reject.empty()) {
        return submit(order);
    }

    {
        const std::lock_guard lock{mutex_};
        order.id = order_ids_.next();
    }
    order.created_at    = clock_.now();
    order.updated_at    = order.created_at;
    order.status        = domain::OrderStatus::Rejected;
    order.reject_reason = std::move(reject);
    publish({Publication{std::nullopt, order}});
    return order.id;
}

common::OrderId ExecutionSimulator::submit(const domain::Order& in) {
    if (in.quantity <= 0) {
        throw common::ValidationError("ExecutionSimulator::submit: order quantity must be > 0");
    }
    domain::Order order = in;
    {
        const std::lock_guard lock{mutex_};
        if (!order.id.valid()) {
            order.id = order_ids_.next();
        }
        if (!order.run_id.valid()) {
            order.run_id = run_id_;
        }
        if (order.created_at == common::Timestamp{}) {
            order.created_at = clock_.now();
        }
        order.updated_at      = order.created_at;
        order.status          = domain::OrderStatus::Working;
        order.filled_quantity = 0;
        order.average_fill_price.reset();
        working_[order.id] = order;
    }
    publish({Publication{std::nullopt, order}});
    return order.id;
}

void ExecutionSimulator::on_market_event(const domain::MarketEvent& event) {
    const std::optional<common::Price> reference = event.open ? event.open : event.price;
    if (!reference || *reference <= common::Price{}) {
        return;
    }
    MarketContext market;
    market.symbol        = event.symbol;
    market.as_of         = event.exchange_time;
    market.last_price    = reference;
    market.bid           = event.bid;
    market.ask           = event.ask;
    market.recent_volume = event.volume;

    std::vector<Publication> out;
    {
        const std::lock_guard lock{mutex_};
        for (auto it = working_.begin(); it != working_.end();) {
            domain::Order& order = it->second;
            if (order.symbol != event.symbol || !(order.created_at < event.exchange_time)) {
                ++it;
                continue;
            }
            for (domain::Fill fill : fill_model_.simulate(order, market, config_)) {
                const common::Quantity shares =
                    fill.filled_quantity < 0 ? -fill.filled_quantity : fill.filled_quantity;
                if (shares == 0) {
                    continue;
                }
                const common::Quantity before = order.filled_quantity;
                order.filled_quantity += shares;
                // Volume-weighted average over every execution of this order.
                const common::Price prior = order.average_fill_price.value_or(common::Price{});
                order.average_fill_price =
                    (prior * before + fill.fill_price * shares) / order.filled_quantity;
                order.updated_at = event.exchange_time;
                order.status     = order.filled_quantity >= order.quantity
                                       ? domain::OrderStatus::Filled
                                       : domain::OrderStatus::PartiallyFilled;

                fill.id       = fill_ids_.next();
                fill.run_id   = order.run_id;
                fill.sequence = ++fill_counts_[order.id];
                fill.status   = order.status == domain::OrderStatus::Filled
                                    ? domain::FillStatus::Filled
                                    : domain::FillStatus::PartiallyFilled;
                out.push_back(Publication{fill, order});
            }
            if (order.status == domain::OrderStatus::Filled) {
                fill_counts_.erase(order.id);
                it = working_.erase(it);
            } else {
                ++it;
            }
        }
    }
    publish(out);
}

std::size_t ExecutionSimulator::open_order_count() const {
    const std::lock_guard lock{mutex_};
    return working_.size();
}

void ExecutionSimulator::publish(const std::vector<Publication>& out) {
    for (const auto& p : out) {
        if (p.fill) {
            bus_.publish(events::Event{events::EventType::Fill, {}, 0, *p.fill});
        }
        bus_.publish(events::Event{events::EventType::Order, {}, 0, p.order});
    }
}

}  // namespace trading_engine::execution
