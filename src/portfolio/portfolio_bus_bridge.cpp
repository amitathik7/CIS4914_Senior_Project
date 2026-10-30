#include "trading_engine/portfolio/portfolio_bus_bridge.hpp"

#include <exception>
#include <variant>

#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/portfolio/portfolio_manager.hpp"

namespace trading_engine::portfolio {

PortfolioBusBridge::PortfolioBusBridge(events::IEventBus& bus, PortfolioManager& portfolio)
    : bus_{bus}, portfolio_{portfolio} {}

PortfolioBusBridge::~PortfolioBusBridge() { detach(); }

void PortfolioBusBridge::attach() {
    if (!subscriptions_.empty()) {
        return;
    }
    try {
        subscriptions_.push_back(
            bus_.subscribe(events::EventType::MarketData, [this](const events::Event& e) {
                if (const auto* market = std::get_if<domain::MarketEvent>(&e.payload)) {
                    portfolio_.mark(*market);
                    last_marks_[market->symbol] = *market;
                    marks_.fetch_add(1, std::memory_order_relaxed);
                }
            }));
        subscriptions_.push_back(
            bus_.subscribe(events::EventType::Order, [this](const events::Event& e) {
                if (const auto* order = std::get_if<domain::Order>(&e.payload)) {
                    try {
                        portfolio_.apply_order_update(*order);
                        order_updates_.fetch_add(1, std::memory_order_relaxed);
                    } catch (const std::exception& ex) {
                        record_error(ex.what());
                    }
                }
            }));
        subscriptions_.push_back(
            bus_.subscribe(events::EventType::Fill, [this](const events::Event& e) {
                if (const auto* fill = std::get_if<domain::Fill>(&e.payload)) {
                    try {
                        if (portfolio_.apply(*fill)) {
                            fills_applied_.fetch_add(1, std::memory_order_relaxed);
                            // apply() marks the position at the fill price. On a bus the
                            // fill is delivered after the market event that caused it, so
                            // put back a mark that is at least as new as the fill.
                            const auto last = last_marks_.find(fill->symbol);
                            if (last != last_marks_.end() &&
                                !(last->second.exchange_time < fill->filled_at)) {
                                portfolio_.mark(last->second);
                            }
                        } else {
                            fills_ignored_.fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (const std::exception& ex) {
                        record_error(ex.what());
                    }
                }
            }));
    } catch (...) {
        detach();   // all or nothing
        throw;
    }
}

void PortfolioBusBridge::detach() noexcept {
    for (const common::SubscriptionId id : subscriptions_) {
        try {
            bus_.unsubscribe(id);
        } catch (...) {
            // Best effort: the handlers only touch members, which outlive this call.
        }
    }
    subscriptions_.clear();
}

PortfolioBusBridge::Stats PortfolioBusBridge::stats() const noexcept {
    Stats s;
    s.marks         = marks_.load(std::memory_order_relaxed);
    s.order_updates = order_updates_.load(std::memory_order_relaxed);
    s.fills_applied = fills_applied_.load(std::memory_order_relaxed);
    s.fills_ignored = fills_ignored_.load(std::memory_order_relaxed);
    s.errors        = errors_.load(std::memory_order_relaxed);
    return s;
}

std::string PortfolioBusBridge::last_error() const {
    const std::lock_guard lock{error_mutex_};
    return last_error_;
}

void PortfolioBusBridge::record_error(const char* what) {
    errors_.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard lock{error_mutex_};
    last_error_ = what;
}

}  // namespace trading_engine::portfolio
