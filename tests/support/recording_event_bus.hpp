#pragma once

// -----------------------------------------------------------------------------
//  RecordingEventBus -- a synchronous IEventBus test double.
//
//  publish() delivers the event to every matching subscriber on the calling
//  thread, before it returns, and records it for assertions. There is no
//  queue, no worker thread and no backpressure: the point is to drive a
//  component under test deterministically and observe exactly what it did. It
//  is NOT the production bus (events::InProcessEventBus) and models none of its
//  timing; anything that depends on asynchronous delivery needs the real bus.
//
//  Delivery rules:
//   * Subscribers of an event's type run in subscription order.
//   * Handlers may publish, subscribe or unsubscribe from inside a handler. A
//     delivery works from a snapshot of the subscribers, and skips one that was
//     unsubscribed earlier in the same delivery.
//   * An exception from a handler propagates out of publish() to the publisher.
//   * publish() assigns Event::sequence (1, 2, 3, ...) and leaves enqueued_at
//     alone: the double has no clock.
//   * The lifecycle calls (start / request_shutdown / wait_until_drained) are
//     only counted, so a test can assert that a component leaves the bus
//     lifecycle to its owner. depth() is always 0 because nothing is queued.
//
//  Faults are switched on through the public `faults` member; see Faults.
//
//  Thread-safety: none. Single-threaded tests only.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::test_support {

class RecordingEventBus final : public events::IEventBus {
public:
    // Switches that make the double misbehave the way a real bus can.
    struct Faults {
        bool reject_publish{false};       // publish() returns false (full, or shut down)
        bool throw_on_publish{false};     // publish() throws
        bool throw_on_subscribe{false};   // subscribe() throws
        bool throw_on_unsubscribe{false}; // unsubscribe() throws
        bool ignore_unsubscribe{false};   // unsubscribe() returns normally but the handler
                                          // stays registered and keeps being invoked
    };
    Faults faults{};

    // --- events::IEventBus ---------------------------------------------------
    bool publish(events::Event event) override {
        if (faults.throw_on_publish) {
            throw std::runtime_error("RecordingEventBus: injected publish() failure");
        }
        if (faults.reject_publish) {
            rejected_.push_back(std::move(event));
            return false;
        }

        event.sequence = ++sequence_;
        published_.push_back(event);
        deliver(event);   // a local copy: a handler that publishes may reallocate published_
        return true;
    }

    common::SubscriptionId subscribe(events::EventType type, events::EventHandler handler) override {
        ++subscribe_calls_;
        if (faults.throw_on_subscribe) {
            throw std::runtime_error("RecordingEventBus: injected subscribe() failure");
        }
        const common::SubscriptionId id{++last_subscription_};
        subscriptions_.push_back(Subscription{id, type, std::move(handler)});
        return id;
    }

    void unsubscribe(common::SubscriptionId id) override {
        ++unsubscribe_calls_;
        if (faults.throw_on_unsubscribe) {
            throw std::runtime_error("RecordingEventBus: injected unsubscribe() failure");
        }
        if (faults.ignore_unsubscribe) {
            return;
        }
        std::erase_if(subscriptions_, [id](const Subscription& s) { return s.id == id; });
    }

    void start() override { ++start_calls_; }
    void request_shutdown() override { ++shutdown_calls_; }
    void wait_until_drained() override { ++drain_calls_; }
    [[nodiscard]] std::size_t depth() const override { return 0; }

    // --- Observation ---------------------------------------------------------
    // Events publish() accepted / refused, in the order they were offered.
    [[nodiscard]] const std::vector<events::Event>& published() const noexcept { return published_; }
    [[nodiscard]] const std::vector<events::Event>& rejected() const noexcept { return rejected_; }

    // The TradeSignal payloads of the accepted EventType::Signal events.
    [[nodiscard]] std::vector<domain::TradeSignal> signals() const {
        std::vector<domain::TradeSignal> out;
        for (const events::Event& event : published_) {
            if (event.type != events::EventType::Signal) {
                continue;
            }
            if (const auto* signal = std::get_if<domain::TradeSignal>(&event.payload)) {
                out.push_back(*signal);
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t subscription_count(events::EventType type) const noexcept {
        std::size_t n = 0;
        for (const Subscription& s : subscriptions_) {
            n += (s.type == type) ? 1U : 0U;
        }
        return n;
    }

    [[nodiscard]] std::size_t subscribe_calls() const noexcept { return subscribe_calls_; }
    [[nodiscard]] std::size_t unsubscribe_calls() const noexcept { return unsubscribe_calls_; }
    [[nodiscard]] std::size_t lifecycle_calls() const noexcept {
        return start_calls_ + shutdown_calls_ + drain_calls_;
    }

private:
    struct Subscription {
        common::SubscriptionId id{};
        events::EventType      type{events::EventType::MarketData};
        events::EventHandler   handler{};
    };

    void deliver(const events::Event& event) {
        std::vector<Subscription> targets;
        for (const Subscription& s : subscriptions_) {
            if (s.type == event.type) {
                targets.push_back(s);
            }
        }
        for (const Subscription& target : targets) {
            if (still_subscribed(target.id)) {
                target.handler(event);
            }
        }
    }

    [[nodiscard]] bool still_subscribed(common::SubscriptionId id) const noexcept {
        for (const Subscription& s : subscriptions_) {
            if (s.id == id) {
                return true;
            }
        }
        return false;
    }

    std::vector<Subscription>  subscriptions_{};
    std::vector<events::Event> published_{};
    std::vector<events::Event> rejected_{};

    std::uint64_t sequence_{0};
    std::uint64_t last_subscription_{0};
    std::size_t   subscribe_calls_{0};
    std::size_t   unsubscribe_calls_{0};
    std::size_t   start_calls_{0};
    std::size_t   shutdown_calls_{0};
    std::size_t   drain_calls_{0};
};

}  // namespace trading_engine::test_support
