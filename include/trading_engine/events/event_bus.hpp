#pragma once

// -----------------------------------------------------------------------------
//  EventBus -- the decoupling seam between pipeline stages.
//
//  Responsibility: accept typed engine events from producers and deliver them
//  to registered consumers. This is the "Event Bus / Data Queue" box in the
//  architecture: market data in, signals/orders/fills flowing between stages.
//
//  Components only ever see IEventBus. Two implementations exist
//  (docs/adr/0003-kafka-transport.md, docs/EVENT_BUS.md):
//   * InProcessEventBus (this file) -- a bounded in-memory queue with one
//     dispatcher thread. Deterministic, no dependencies; the default for unit
//     tests, backtests and single-process runs.
//   * TopicEventBus (topic_event_bus.hpp) -- the same contract over a message
//     transport: Kafka in production (trading_engine_kafka_adapter), an
//     in-memory broker in tests. This is the team's chosen inter-component
//     transport.
//
//  CONTRACT every implementation meets (the StrategyEngine relies on all of
//  it; docs/STRATEGIES.md section 4):
//   1. Handlers run on ONE dispatcher thread per bus, so invocations are
//      serialized and never overlap. Within one event, subscribers of its type
//      run in subscription order.
//   2. unsubscribe() is a barrier: when it returns, no invocation of that
//      handler is running or will begin. Called from inside a handler (on the
//      dispatcher thread) it cannot wait for itself, so it returns at once and
//      the handler is skipped for the rest of the current delivery.
//   3. subscribe() with an empty handler throws std::invalid_argument and
//      stores nothing. Subscribing from inside a handler is allowed; the new
//      handler sees events from the next delivery on.
//   4. The owner drives the lifecycle: start() spins the dispatcher,
//      request_shutdown() stops accepting events from outside the dispatcher,
//      wait_until_drained() blocks until everything accepted has been
//      delivered (and, after request_shutdown(), stops the dispatcher).
//      start() after shutdown throws std::logic_error: a bus is single-use.
//   5. A handler may publish. A publish() from the dispatcher thread never
//      blocks (it would deadlock on itself) and is accepted while the bus is
//      running or draining, so the consequences of in-flight events -- e.g.
//      the signal a strategy emits for a bar that was queued before shutdown
//      -- are not lost during a drain.
//   6. publish() returns false when the event is refused (not started, shut
//      down, or full under BackpressurePolicy::Reject) and never throws for
//      those cases. The bus owns Event::sequence and Event::enqueued_at and
//      overwrites whatever the publisher set.
//   7. An exception escaping a handler is caught on the dispatcher thread and
//      counted; it cannot reach the publisher, which may be long gone.
//
//  Ownership / lifecycle: owned by the composition root; outlives all
//  producers and consumers. Thread-safety: every method may be called from
//  any thread.
// -----------------------------------------------------------------------------

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/risk_decision_report.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace trading_engine::events
{

    enum class EventType : std::uint8_t
    {
        MarketData = 0,
        Signal,
        Order,
        Fill,
        RiskDecision   // the RiskManager's verdict on each signal (domain::RiskDecisionReport)
        // TODO: PortfolioUpdate, Control (shutdown/flush), Timer, Error.
    };

    inline constexpr std::size_t kEventTypeCount = 5;

    // PROVISIONAL envelope. A closed std::variant keeps the scaffold dependency-free
    // and lets consumers std::visit. If the payload set grows large or needs to be
    // extended by plugins, revisit (type-erased Event, or per-type queues).
    using EventPayload =
        std::variant<domain::MarketEvent, domain::TradeSignal, domain::Order, domain::Fill,
                     domain::RiskDecisionReport>;

    struct Event
    {
        EventType type{EventType::MarketData};
        common::Timestamp enqueued_at{}; // UTC, set by the bus on publish
        std::uint64_t sequence{0};       // monotonic per publishing bus instance
        EventPayload payload{};
    };

    using EventHandler = std::function<void(const Event &)>;

    class IEventBus
    {
    public:
        virtual ~IEventBus();

        // Producer side. See the contract above (items 5 and 6).
        virtual bool publish(Event event) = 0;

        // Consumer side. Handlers are invoked on the bus's dispatcher thread.
        virtual common::SubscriptionId subscribe(EventType type, EventHandler handler) = 0;
        virtual void unsubscribe(common::SubscriptionId id) = 0;

        // Lifecycle.
        virtual void start() = 0;
        virtual void request_shutdown() = 0;
        virtual void wait_until_drained() = 0;

        // Observability: events accepted but not yet delivered.
        [[nodiscard]] virtual std::size_t depth() const = 0;
    };

    // What publish() does when the queue is at capacity.
    enum class BackpressurePolicy : std::uint8_t
    {
        Block,  // wait for room (the default: a backtest must not lose bars)
        Reject  // return false at once (a live feed that cannot be paused)
    };

    // The subscriber table and delivery loop shared by both bus implementations.
    // It implements contract items 1-3 and 7; the buses add queueing and
    // lifecycle. Public so a new IEventBus can reuse it, not for components.
    class SubscriptionRegistry
    {
    public:
        common::SubscriptionId add(EventType type, EventHandler handler);
        void remove(common::SubscriptionId id);

        // Runs every live subscriber of event.type, in subscription order, on
        // the calling thread (which must be the bound dispatcher). Returns how
        // many handlers threw.
        std::size_t deliver(const Event &event);

        void bind_dispatcher(std::thread::id id) noexcept { dispatcher_.store(id); }
        [[nodiscard]] bool on_dispatcher() const noexcept
        {
            return dispatcher_.load() == std::this_thread::get_id();
        }
        [[nodiscard]] std::size_t count(EventType type) const;

    private:
        struct Entry
        {
            common::SubscriptionId id{};
            EventType type{EventType::MarketData};
            EventHandler handler{};
            std::atomic<bool> active{true};
        };

        mutable std::mutex mutex_;          // guards entries_ and next_id_
        std::vector<std::shared_ptr<Entry>> entries_{};
        std::uint64_t next_id_{0};
        std::mutex delivery_mutex_;         // held around every handler call: the barrier
        std::atomic<std::thread::id> dispatcher_{};
    };

    struct InProcessEventBusOptions
    {
        std::size_t capacity{65'536};   // events queued but not yet delivered; > 0
        BackpressurePolicy when_full{BackpressurePolicy::Block};
    };

    // Bounded in-memory queue + one dispatcher thread.
    class InProcessEventBus final : public IEventBus
    {
    public:
        struct Stats
        {
            std::uint64_t published{0};      // accepted by publish()
            std::uint64_t rejected{0};       // refused: not running, or full under Reject
            std::uint64_t delivered{0};      // events taken off the queue and dispatched
            std::uint64_t handler_errors{0}; // exceptions caught from handlers
            std::size_t high_water_mark{0};  // deepest the queue has been
        };

        // Throws std::invalid_argument for capacity == 0.
        explicit InProcessEventBus(const common::IClock &clock,
                                   InProcessEventBusOptions options = {});
        ~InProcessEventBus() override;   // request_shutdown() + drain + join

        InProcessEventBus(const InProcessEventBus &) = delete;
        InProcessEventBus &operator=(const InProcessEventBus &) = delete;

        bool publish(Event event) override;
        common::SubscriptionId subscribe(EventType type, EventHandler handler) override;
        void unsubscribe(common::SubscriptionId id) override;
        void start() override;
        void request_shutdown() override;
        void wait_until_drained() override;   // std::logic_error from a handler
        [[nodiscard]] std::size_t depth() const override;

        [[nodiscard]] Stats stats() const;
        [[nodiscard]] std::size_t subscription_count(EventType type) const
        {
            return registry_.count(type);
        }

    private:
        enum class State : std::uint8_t { NotStarted, Running, Draining, Stopped };

        void run();   // dispatcher thread body

        const common::IClock &clock_;
        const InProcessEventBusOptions options_;
        SubscriptionRegistry registry_;

        mutable std::mutex mutex_;   // guards everything below except worker_
        std::condition_variable not_empty_;
        std::condition_variable not_full_;
        std::condition_variable idle_;
        std::deque<Event> queue_{};
        bool busy_{false};           // the dispatcher is inside a delivery
        State state_{State::NotStarted};
        std::uint64_t sequence_{0};
        Stats stats_{};

        std::mutex join_mutex_;      // serializes joining worker_
        std::thread worker_;
    };

} // namespace trading_engine::events
