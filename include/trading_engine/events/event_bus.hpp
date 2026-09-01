#pragma once

// -----------------------------------------------------------------------------
//  EventBus -- the decoupling seam between pipeline stages.
//
//  Responsibility: accept typed engine events from producers and deliver them
//  to registered consumers. This is the "Event Bus / Data Queue" box in the
//  architecture: market data in, signals/orders/fills flowing between stages.
//
//  FUTURE DESIGN (none of this is built yet):
//   * bounded, multi-producer / multi-consumer queue(s);
//   * backpressure policy when full -- block producer, drop-oldest, or reject
//     (OPEN QUESTION, see docs/OPEN_QUESTIONS.md);
//   * explicit lifecycle: start() spins consumer threads, request_shutdown()
//     stops accepting new events, wait_until_drained() blocks until in-flight
//     events are processed;
//   * per-type sequencing and timestamps for observability.
//
//  Ownership / lifecycle: owned by TradingEngine; outlives all producers and
//  consumers. Thread-safety: the future implementation MUST be safe for
//  concurrent publish() from many threads. The scaffold does nothing.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <functional>
#include <variant>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace trading_engine::events {

enum class EventType : std::uint8_t {
    MarketData = 0,
    Signal,
    Order,
    Fill
    // TODO: PortfolioUpdate, Control (shutdown/flush), Timer, Error.
};

// PROVISIONAL envelope. A closed std::variant keeps the scaffold dependency-free
// and lets consumers std::visit. If the payload set grows large or needs to be
// extended by plugins, revisit (type-erased Event, or per-type queues).
using EventPayload =
    std::variant<domain::MarketEvent, domain::TradeSignal, domain::Order, domain::Fill>;

struct Event {
    EventType         type{EventType::MarketData};
    common::Timestamp enqueued_at{};   // UTC, set by the bus on publish
    std::uint64_t     sequence{0};     // bus-global monotonic counter
    EventPayload      payload{};
};

using EventHandler = std::function<void(const Event&)>;

class IEventBus {
public:
    virtual ~IEventBus();

    // Producer side. Non-blocking contract is TBD (depends on backpressure
    // policy). Returns false if the event was dropped/rejected.
    virtual bool publish(Event event) = 0;

    // Consumer side. Handlers are invoked on bus worker threads (future).
    virtual common::SubscriptionId subscribe(EventType type, EventHandler handler) = 0;
    virtual void unsubscribe(common::SubscriptionId id) = 0;

    // Lifecycle.
    virtual void start() = 0;
    virtual void request_shutdown() = 0;
    virtual void wait_until_drained() = 0;

    // Observability.
    [[nodiscard]] virtual std::size_t depth() const = 0;
};

// In-process implementation placeholder. Every method throws
// common::NotImplemented until the queue and threading model are chosen.
class InProcessEventBus final : public IEventBus {
public:
    InProcessEventBus();
    ~InProcessEventBus() override;

    bool publish(Event event) override;
    common::SubscriptionId subscribe(EventType type, EventHandler handler) override;
    void unsubscribe(common::SubscriptionId id) override;
    void start() override;
    void request_shutdown() override;
    void wait_until_drained() override;
    [[nodiscard]] std::size_t depth() const override;

    // TODO: capacity, per-type ring buffers, worker-thread pool, drop counters,
    //       high-water-mark metric wired to SystemMetrics.
};

}  // namespace trading_engine::events
