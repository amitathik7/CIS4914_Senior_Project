#include "trading_engine/risk/risk_manager.hpp"

#include <chrono>
#include <utility>

#include "trading_engine/common/errors.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/portfolio/portfolio_manager.hpp"
#include "trading_engine/portfolio/reservation_ledger.hpp"

namespace trading_engine::risk {

RiskManager::RiskManager(const portfolio::IPortfolioView& portfolio,
                         events::IEventBus& bus,
                         const common::IClock& clock,
                         config::RiskLimits limits,
                         std::vector<std::shared_ptr<IRiskPolicy>> policies)
    : portfolio_{portfolio},
      bus_{bus},
      clock_{clock},
      limits_{limits},
      policies_{std::move(policies)} {
    for (const auto& policy : policies_) {
        if (policy == nullptr) {
            throw common::ValidationError("RiskManager: policy must not be null");
        }
    }
}

RiskManager::~RiskManager() {
    stop();
}

void RiskManager::start() {
    {
        const std::lock_guard lock{mutex_};
        if (!subscriptions_.empty()) {
            return;
        }
    }
    // Subscribed outside the lock: a handler may already run on the dispatcher.
    const auto prices = bus_.subscribe(events::EventType::MarketData, [this](const events::Event& e) {
        if (const auto* event = std::get_if<domain::MarketEvent>(&e.payload)) {
            on_market_event(*event);
        }
    });
    const auto signals = bus_.subscribe(events::EventType::Signal, [this](const events::Event& e) {
        const auto* signal = std::get_if<domain::TradeSignal>(&e.payload);
        if (signal == nullptr) {
            return;
        }
        const RiskDecision decision = check(*signal);
        if (decision.approved && sink_) {
            domain::TradeSignal approved = *signal;
            if (decision.adjusted_quantity) {
                approved.requested_quantity = decision.adjusted_quantity;
            }
            sink_(approved);
        }
    });
    const std::lock_guard lock{mutex_};
    subscriptions_ = {prices, signals};
}

void RiskManager::stop() {
    std::vector<common::SubscriptionId> ids;
    {
        const std::lock_guard lock{mutex_};
        ids.swap(subscriptions_);
    }
    for (const auto id : ids) {
        bus_.unsubscribe(id);
    }
}

void RiskManager::on_market_event(const domain::MarketEvent& event) {
    const auto price = event.price ? event.price : event.open;
    const std::lock_guard lock{mutex_};
    if (price && *price > common::Price{}) {
        prices_[event.symbol] = *price;
    }
    // Sessions are UTC calendar days of the data's exchange_time.
    const auto day = std::chrono::floor<std::chrono::days>(event.exchange_time)
                         .time_since_epoch()
                         .count();
    if (day != session_day_) {
        session_day_          = day;
        session_start_equity_ = portfolio_.snapshot().total_equity;
    }
}

RiskDecision RiskManager::check(const domain::TradeSignal& signal) {
    const domain::PortfolioSnapshot snapshot = portfolio_.snapshot();

    RiskContext context;
    context.limits           = limits_;
    context.open_order_count = static_cast<std::uint32_t>(snapshot.pending_orders.size());
    context.as_of            = clock_.now();
    {
        const std::lock_guard lock{mutex_};
        context.reference_prices = prices_;
        context.pnl_today        = snapshot.total_equity - session_start_equity_.value_or(snapshot.total_equity);
    }
    context.realized_pnl_today = context.pnl_today;

    RiskRecord record{signal, RiskDecision::allow(), {}};
    domain::TradeSignal working = signal;
    const common::Quantity asked = signal.requested_quantity.value_or(0);

    for (const auto& policy : policies_) {
        RiskDecision d = policy->evaluate(working, snapshot, context);
        if (!d.approved) {
            record.decision = std::move(d);
            record.policy   = std::string(policy->name());
            break;
        }
        if (d.adjusted_quantity && *d.adjusted_quantity < working.requested_quantity.value_or(0)) {
            working.requested_quantity = d.adjusted_quantity;
            record.decision = std::move(d);
            record.policy   = std::string(policy->name());
        }
    }

    if (record.decision.approved && ledger_ != nullptr) {
        const common::Quantity quantity = working.requested_quantity.value_or(0);
        const auto side = working.side == domain::SignalSide::Sell ? domain::OrderSide::Sell
                                                                   : domain::OrderSide::Buy;
        const auto hold = ledger_->hold_for_signal(working.id, working.symbol, side, quantity,
                                                   working.limit_price);
        if (!hold.granted) {
            record.decision = RiskDecision::deny(
                side == domain::OrderSide::Sell
                    ? "not enough shares held to sell " + std::to_string(quantity) + " (no short selling)"
                    : "not enough buying power for " + std::to_string(quantity) + " shares");
            record.policy = "buying_power";
        }
    }

    const common::Quantity final_quantity = working.requested_quantity.value_or(0);
    if (record.decision.approved && final_quantity != asked) {
        record.decision.adjusted_quantity = final_quantity;
    } else {
        record.decision.adjusted_quantity.reset();
    }

    const std::lock_guard lock{mutex_};
    ++stats_.evaluated;
    if (record.decision.approved) {
        ++stats_.approved;
        if (record.decision.adjusted_quantity) {
            ++stats_.resized;
        }
    } else {
        ++stats_.rejected;
    }
    decisions_.push_back(record);
    return record.decision;
}

void RiskManager::add_policy(std::shared_ptr<IRiskPolicy> policy) {
    if (policy == nullptr) {
        throw common::ValidationError(
            "RiskManager::add_policy: policy must not be null");
    }
    policies_.push_back(std::move(policy));
}

std::vector<RiskRecord> RiskManager::decisions() const {
    const std::lock_guard lock{mutex_};
    return decisions_;
}

RiskManager::Stats RiskManager::stats() const {
    const std::lock_guard lock{mutex_};
    return stats_;
}

}  // namespace trading_engine::risk
