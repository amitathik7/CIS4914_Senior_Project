#pragma once

// -----------------------------------------------------------------------------
//  SynchronousDemoBus -- the Strategy Lab's event bus.
//
//  It is NOT the production queue. There is no bounded queue, no worker thread, no
//  backpressure and no asynchronous delivery here: publish() runs the subscribers on
//  the calling thread before it returns. events::InProcessEventBus (the production
//  class) is a stub that throws, and tests/support's RecordingEventBus is a test double
//  the runnable tool must not depend on. This class exists so the REAL StrategyEngine
//  can be driven by a tool that builds with BUILD_TESTING=OFF and without GoogleTest.
//
//  It satisfies the bus assumptions documented for the engine
//  (docs/STRATEGIES.md section 4, strategy_engine.hpp):
//   1. unsubscribe() is a barrier. Single-threaded and synchronous, so when it returns
//      no handler is running; a handler unsubscribed from inside a delivery is skipped
//      for the rest of that delivery.
//   2. Handler invocations are serialized by construction. The bus remembers the first
//      thread that uses it and throws std::logic_error if another one ever does, so a
//      violation is loud instead of a data race.
//   3. subscribe() that throws leaves no subscription (an empty handler is refused
//      before anything is stored).
//   4. The owner outlives the engine and drains the bus before stopping it: the
//      composition root (lab/runner.cpp) calls start(), request_shutdown() and
//      wait_until_drained(); the engine never does.
//   5. A handler may publish. The engine publishes Signal events from inside its
//      MarketData handler; that nested delivery happens before the outer publish()
//      returns (depth first, capped at kMaxDepth to turn runaway recursion into an error).
//
//  Behaviour beyond that contract (lab-local policy):
//   * publish() before start() or after request_shutdown() returns false (rejected).
//   * publish() assigns Event::sequence (1, 2, 3... across ALL event types) and sets
//     Event::enqueued_at from the injected clock, as the real bus is meant to.
//   * An exception from a handler propagates out of publish() to the publisher.
//   * Optional fault injection, for demonstrating the engine's documented publication
//     failures: every Signal event can be refused (publish() returns false) or can make
//     publish() throw. Market data is never faulted. A refused signal is recorded in
//     failures() and is not delivered or counted as published.
//   * depth() is always 0: nothing is ever queued.
// -----------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::lab {

class SynchronousDemoBus final : public events::IEventBus {
public:
    enum class Fault : std::uint8_t { None, RejectSignals, ThrowOnSignal };

    struct FailedPublication {
        enum class Kind : std::uint8_t { Rejected, Threw };
        events::Event event{};      // as the engine offered it (id, strategy and time already stamped)
        Kind          kind{Kind::Rejected};
        std::string   message{};
    };

    struct Counters {
        std::array<std::uint64_t, 4> published_by_type{};   // indexed by events::EventType
        std::uint64_t                rejected_not_running{0};   // publish() while not running
        std::uint64_t                refused_by_fault{0};       // Signal events refused or made to throw
    };

    static constexpr std::size_t kMaxDepth = 32;

    explicit SynchronousDemoBus(const common::IClock& clock, Fault fault = Fault::None);
    ~SynchronousDemoBus() override;

    SynchronousDemoBus(const SynchronousDemoBus&)            = delete;
    SynchronousDemoBus& operator=(const SynchronousDemoBus&) = delete;

    // --- events::IEventBus -----------------------------------------------------
    bool publish(events::Event event) override;
    common::SubscriptionId subscribe(events::EventType type, events::EventHandler handler) override;
    void unsubscribe(common::SubscriptionId id) override;
    void start() override;                 // throws std::logic_error after request_shutdown()
    void request_shutdown() override;
    void wait_until_drained() override;    // nothing is queued: returns at once
    [[nodiscard]] std::size_t depth() const override { return 0; }

    // --- observation -------------------------------------------------------------
    [[nodiscard]] bool running() const noexcept { return state_ == State::Running; }
    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return sequence_; }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    [[nodiscard]] const std::vector<FailedPublication>& failures() const noexcept { return failures_; }
    [[nodiscard]] std::size_t subscription_count(events::EventType type) const noexcept;

private:
    enum class State : std::uint8_t { NotStarted, Running, ShutDown };

    struct Subscription {
        common::SubscriptionId id{};
        events::EventType      type{events::EventType::MarketData};
        events::EventHandler   handler{};
    };

    void require_owner_thread();
    [[nodiscard]] bool still_subscribed(common::SubscriptionId id) const noexcept;
    void deliver(const events::Event& event);

    const common::IClock& clock_;
    Fault                 fault_;
    State                 state_{State::NotStarted};

    std::vector<Subscription>                         subscriptions_{};
    common::SequentialIdGenerator<common::SubscriptionId> subscription_ids_{};
    std::uint64_t                                     sequence_{0};
    std::size_t                                       depth_{0};
    Counters                                          counters_{};
    std::vector<FailedPublication>                    failures_{};
    std::thread::id                                   owner_{};
};

}  // namespace trading_engine::lab
