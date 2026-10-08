#pragma once

// -----------------------------------------------------------------------------
//  StrategyEngine -- routes market events to strategies and collects signals.
//
//  Responsibility: own the set of registered strategies, deliver each inbound
//  MarketEvent to every one of them, provide the ISignalSink they emit
//  through, stamp each emitted TradeSignal with an id and timestamp, and
//  publish it to the event bus for the RiskManager.
//
//  Lifecycle: Stopped -> Starting -> Running -> Stopping -> Stopped, and round
//  again; the engine is restartable.
//   * register_strategy() is accepted only while Stopped. Ids must be non-empty
//     and unique; registration order is delivery order for the engine's life.
//   * start() calls on_start() on each strategy in registration order, then
//     subscribes to EventType::MarketData. If an on_start() or the subscription
//     throws, the strategies that had already started get on_stop() (reverse
//     order), nothing stays subscribed, the engine stays Stopped, and the
//     original exception propagates. The strategy whose on_start() threw gets
//     no on_stop().
//   * stop() unsubscribes, then calls on_stop() on each strategy in reverse
//     registration order. It never throws: failures are absorbed and counted.
//   * start() on a running engine and stop() on a stopped one are no-ops, so
//     repeating them never duplicates a subscription, a callback or a signal.
//   * A restart subscribes afresh and calls on_start() again. SignalIds keep
//     increasing and the statistics keep accumulating: neither is ever reset.
//   * A MarketEvent that reaches on_market_event() while the engine is not
//     Running is dropped and counted; no strategy sees it.
//
//  Signals: every strategy gets its own engine-owned ISignalSink. emit() copies
//  the signal and sets id (fresh, nonzero, unique for the life of this object,
//  restarts included), created_at (injected IClock) and strategy_id (the id()
//  captured at registration), then publishes EventType::Signal. The bus owns
//  Event::enqueued_at and Event::sequence and the engine leaves them unset.
//  Ids are per engine object: a new engine, or a new process, counts from 1
//  again (see the TODO in common/identifiers.hpp about persisted ids).
//
//  Failures: an exception from one strategy's on_market_event() is caught,
//  counted and recorded (see strategy_stats()) and the remaining strategies
//  still receive the event; signals that strategy emitted before throwing stay
//  published. IEventBus::publish() returning false, or throwing, is counted
//  and never reported as a delivery. The engine neither retries nor buffers: a
//  rejected signal is lost, and the id it consumed is simply skipped.
//  on_market_event() itself does not throw.
//
//  Threading: the engine adds no threads and no locks; it relies on the
//  single-threaded callback model. on_market_event() calls never overlap one
//  another or start() / stop() / register_strategy(), and nothing re-enters the
//  engine from a strategy callback or from a synchronous bus handler running
//  inside publish() (a nested on_market_event() is dropped and counted; nested
//  start() / stop() are unsupported). The counters are plain integers: read
//  them from the thread that drives the engine, or once it is quiescent.
//
//  Bus lifetime and shutdown assumptions:
//   * The bus is borrowed, not owned, and must outlive this engine: stop() --
//     which the destructor runs for a running engine -- calls
//     bus.unsubscribe().
//   * The handler given to the bus holds no direct pointer to the engine, only
//     a token that stop() invalidates. A handler the bus still holds (its
//     unsubscribe() failed or is lazy, or it keeps a copy of the handler) is
//     therefore inert once the engine has stopped or been destroyed, and a
//     restart cannot revive it, so events are never delivered twice.
//   * unsubscribe() is assumed to be a barrier: when it returns, no invocation
//     of the handler is running or will begin. The token makes a late start
//     harmless; it cannot make an invocation already inside the engine safe, so
//     a bus that cannot give this guarantee must be quiesced before stop().
//     The engine is ordered to match, which is what lets it do without locks:
//     start() becomes Running before it subscribes and writes no state after,
//     and stop() withdraws the subscription before it changes any state.
//   * The engine never calls bus.start(), request_shutdown() or
//     wait_until_drained(); the composition root owns the bus lifecycle. Stop
//     the engine after the bus has drained: market events still queued behind a
//     stopped engine are dropped, and signals published after the bus stopped
//     accepting events are rejected and counted.
//
//  Ownership: owned by TradingEngine; holds strategies via shared_ptr and
//  borrows the bus and the clock. Not copyable or movable: the bus handler and
//  the per-strategy sinks refer back to this object.
//
//  TODO: a symbol -> [strategy] routing index (needs a symbol-interest
//        declaration on IStrategy); forward the counters to
//        analytics::SystemMetrics (Stage::Strategy) once that is implemented;
//        route failure text through the logging facade once one exists (OQ#8);
//        latency instrumentation.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/market_data/market_data_source.hpp"
#include "trading_engine/strategy/strategy.hpp"

namespace trading_engine::events { class IEventBus; }

namespace trading_engine::strategy {

class StrategyEngine final : public market_data::IMarketEventSink {
public:
    // Engine-wide counters, monotonic for the life of the object.
    struct Stats {
        std::uint64_t events_routed{0};       // events fanned out to the strategies
        std::uint64_t events_dropped{0};      // received but not delivered: engine not running,
                                              // nested inside another delivery, or wrong payload
        std::uint64_t strategy_errors{0};     // exceptions absorbed from strategy code
                                              // (on_market_event, and on_stop during stop())
        std::uint64_t signals_published{0};   // bus.publish() returned true
        std::uint64_t signals_rejected{0};    // bus.publish() returned false; the signal is lost
        std::uint64_t publish_errors{0};      // bus.publish() threw; the signal is lost
        std::uint64_t signals_dropped{0};     // emit() outside its strategy's callback; never offered
        std::uint64_t bus_errors{0};          // bus.unsubscribe() threw during stop()
    };

    // The same counters per strategy. `errors` also covers on_stop() failures.
    struct StrategyStats {
        std::string   id{};
        std::uint64_t events_delivered{0};    // on_market_event() invoked, including calls that threw
        std::uint64_t errors{0};
        std::uint64_t signals_published{0};
        std::uint64_t signals_rejected{0};
        std::uint64_t publish_errors{0};
        std::uint64_t signals_dropped{0};
        std::string   last_error{};           // "<where>: <what>" of the latest absorbed exception
    };

    StrategyEngine(events::IEventBus& bus, const common::IClock& clock);
    ~StrategyEngine() override;   // runs stop() if the engine is still running

    StrategyEngine(const StrategyEngine&)            = delete;
    StrategyEngine& operator=(const StrategyEngine&) = delete;

    // Setup-time, while stopped. Throws common::ValidationError for a null
    // strategy, an empty id or a duplicate id, and std::logic_error if the
    // engine is not stopped. A rejected call leaves the engine unchanged.
    void register_strategy(std::shared_ptr<IStrategy> strategy);

    // See "Lifecycle" above. start() rethrows whatever aborted the startup.
    void start();
    void stop() noexcept;

    // Fan-out entry point: reached through the bus subscription, or called
    // directly by anything holding this as an IMarketEventSink. Delivers the
    // event to every strategy in registration order. Does not throw.
    void on_market_event(const domain::MarketEvent& event) override;

    [[nodiscard]] bool is_running() const noexcept { return state_ == State::Running; }
    [[nodiscard]] std::size_t strategy_count() const noexcept { return registrations_.size(); }

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::vector<StrategyStats> strategy_stats() const;   // registration order

private:
    class Registration;   // a strategy, its engine-owned sink and its counters

    // The bus handler's only link to the engine: stop() resets the owning
    // pointer, which expires every token the handler holds.
    struct Gate {
        StrategyEngine* engine{nullptr};
    };

    enum class State : std::uint8_t { Stopped, Starting, Running, Stopping };

    void stop_strategies(std::size_t count) noexcept;   // on_stop() to the first `count`, in reverse

    events::IEventBus&    bus_;
    const common::IClock& clock_;

    State state_{State::Stopped};

    // Registration is incomplete in this header. Do NOT give this member a `{}`
    // default initializer: MSVC (C++20) then instantiates ~vector, and with it
    // ~unique_ptr<Registration>, in every translation unit that includes this
    // header, and that does not compile. The constructor in strategy_engine.cpp,
    // where Registration is complete, value-initializes it just the same.
    std::vector<std::unique_ptr<Registration>> registrations_;

    // Never reset, so SignalIds stay unique across stop()/start() cycles.
    common::SequentialIdGenerator<common::SignalId> signal_ids_{};

    common::SubscriptionId subscription_{};
    std::shared_ptr<Gate>  gate_{};              // non-null exactly while subscribed
    Registration*          active_{nullptr};     // the strategy whose callback is running, if any

    std::uint64_t events_routed_{0};
    std::uint64_t events_dropped_{0};
    std::uint64_t bus_errors_{0};
};

}  // namespace trading_engine::strategy
