#pragma once

// Shared scaffolding for the StrategyEngine tests: a REAL engine wired to a
// RecordingEventBus and a ManualClock, plus builders for the events and signals
// the tests feed it. The strategies are test_support::FakeStrategy.

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "support/fake_strategy.hpp"
#include "support/price_literals.hpp"
#include "support/recording_event_bus.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

namespace common   = trading_engine::common;
namespace domain   = trading_engine::domain;
namespace events   = trading_engine::events;
namespace strategy = trading_engine::strategy;
namespace support  = trading_engine::test_support;

// A clock reading that is neither the epoch nor any default-constructed
// timestamp, so "stamped from the clock" cannot pass by accident.
inline const common::Timestamp kT0 = common::Timestamp{} + std::chrono::hours{5};

inline domain::MarketEvent market_event(
    std::string symbol,
    domain::MarketEventType type = domain::MarketEventType::Trade,
    common::Price price = common::Price::from_units(100)) {
    domain::MarketEvent event;
    event.symbol = std::move(symbol);
    event.type   = type;
    event.price  = price;
    return event;
}

inline domain::TradeSignal signal_for(
    std::string symbol, domain::SignalSide side = domain::SignalSide::Buy) {
    domain::TradeSignal signal;
    signal.symbol = std::move(symbol);
    signal.side   = side;
    return signal;
}

// The message of the std::exception that `action` throws; "" if it throws nothing.
template <class Action>
std::string thrown_what(Action&& action) {
    try {
        action();
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "<non-std exception>";
    }
    return {};
}

class StrategyEngineTest : public ::testing::Test {
protected:
    // Order matters: members are destroyed in reverse, and the engine's
    // destructor still calls into the bus and into strategies that append to
    // `calls`, so all three must outlive it.
    support::RecordingEventBus bus;
    common::ManualClock        clock{kT0};
    support::CallLog           calls;
    strategy::StrategyEngine   engine{bus, clock};

    // Registers a FakeStrategy that logs into `calls`, and returns it.
    std::shared_ptr<support::FakeStrategy> add(std::string id) {
        auto fake = std::make_shared<support::FakeStrategy>(std::move(id), &calls);
        engine.register_strategy(fake);
        return fake;
    }

    // Publishes a MarketData event on the bus, as MarketDataService will.
    bool publish_market(domain::MarketEvent event) {
        events::Event envelope;
        envelope.type    = events::EventType::MarketData;
        envelope.payload = std::move(event);
        return bus.publish(std::move(envelope));
    }

    // Registered strategy ids, in the engine's order.
    [[nodiscard]] std::vector<std::string> ids() const {
        std::vector<std::string> out;
        for (const auto& each : engine.strategy_stats()) {
            out.push_back(each.id);
        }
        return out;
    }

    // The envelopes of every Signal event the bus accepted.
    [[nodiscard]] std::vector<events::Event> signal_events() const {
        std::vector<events::Event> out;
        for (const events::Event& event : bus.published()) {
            if (event.type == events::EventType::Signal) {
                out.push_back(event);
            }
        }
        return out;
    }

    // Live MarketData subscriptions on the bus (the engine should hold 0 or 1).
    [[nodiscard]] std::size_t market_subscriptions() const {
        return bus.subscription_count(events::EventType::MarketData);
    }
};
