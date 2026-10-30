#pragma once

// -----------------------------------------------------------------------------
//  TopicEventBus -- IEventBus over a message broker (Kafka).
//
//  This is how components talk to each other through Kafka without knowing
//  it: they keep calling IEventBus::publish()/subscribe(), and this bus turns
//  each Event into one message on a per-type topic and delivers what it reads
//  back from those topics to local subscribers. docs/adr/0003-kafka-transport.md
//  records the decision; docs/EVENT_BUS.md is the user guide.
//
//  Topics: "<topic_prefix>.market_data", ".signal", ".order", ".fill".
//  Message key: the payload's symbol, so one symbol's events stay ordered on
//  one partition. Value: the JSON wire format in event_codec.hpp.
//
//  Delivery path: publish() encodes and hands the message to the transport;
//  it is NOT delivered locally on the spot. Local subscribers receive it when
//  the dispatcher thread reads it back from the broker -- exactly like a
//  subscriber in another process would. So a single process can run the whole
//  pipeline through Kafka, and splitting components across processes later
//  changes nothing in them.
//
//  Meets the full IEventBus contract in event_bus.hpp, with these specifics:
//   * Event::sequence is monotonic per bus instance (per "origin"), not
//     global across processes. Each message also carries the origin id.
//   * Ordering: total per (topic, symbol). Events of different types, or of
//     different symbols, may interleave differently than they were published.
//   * depth() is the number of this bus's own messages published but not yet
//     read back and dispatched.
//   * wait_until_drained() flushes the transport, then waits until every
//     message this bus published has come back and been dispatched (or
//     drain_timeout passes; counted in Stats::drain_timeouts). With a
//     broker shared by several processes that is "my output has been
//     handled here", not "the whole system is idle".
//   * There is no capacity: backpressure is the transport's (librdkafka's
//     queue.buffering.max.messages); a refusal there makes publish() false.
//   * A message that fails to decode is counted and skipped, never delivered.
//
//  Thread-safety: every method may be called from any thread. Handlers run on
//  the dispatcher thread, which is also the only thread that calls
//  transport.poll().
// -----------------------------------------------------------------------------

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/events/message_transport.hpp"

namespace trading_engine::events {

struct TopicEventBusOptions {
    // Kafka topic names allow [A-Za-z0-9._-]; anything else throws.
    std::string topic_prefix{"trading_engine"};
    // Identifies this bus instance on the wire. Empty = random 64-bit hex.
    std::string origin{};
    // How long one transport.poll() may block; bounds shutdown latency.
    std::chrono::milliseconds poll_interval{std::chrono::milliseconds{50}};
    std::chrono::milliseconds drain_timeout{std::chrono::seconds{30}};
};

class TopicEventBus final : public IEventBus {
public:
    struct Stats {
        std::uint64_t published{0};       // handed to the transport
        std::uint64_t rejected{0};        // refused: not running, or transport refused / threw
        std::uint64_t received{0};        // messages read from the broker (any origin)
        std::uint64_t received_own{0};    // ... of which this bus published
        std::uint64_t decode_errors{0};   // unreadable messages, skipped
        std::uint64_t handler_errors{0};  // exceptions caught from handlers
        std::uint64_t drain_timeouts{0};  // wait_until_drained() gave up waiting
    };

    // The transport is borrowed and must outlive the bus.
    TopicEventBus(IMessageTransport& transport, const common::IClock& clock,
                  TopicEventBusOptions options = {});
    ~TopicEventBus() override;

    TopicEventBus(const TopicEventBus&)            = delete;
    TopicEventBus& operator=(const TopicEventBus&) = delete;

    bool publish(Event event) override;   // std::invalid_argument if type != payload
    common::SubscriptionId subscribe(EventType type, EventHandler handler) override;
    void unsubscribe(common::SubscriptionId id) override;
    void start() override;                // subscribes the transport, then spins the dispatcher
    void request_shutdown() override;
    void wait_until_drained() override;
    [[nodiscard]] std::size_t depth() const override;

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::string topic(EventType type) const;
    [[nodiscard]] const std::string& origin() const noexcept { return options_.origin; }
    [[nodiscard]] std::size_t subscription_count(EventType type) const {
        return registry_.count(type);
    }

private:
    enum class State : std::uint8_t { NotStarted, Running, Draining, Stopped };

    void run();

    IMessageTransport&    transport_;
    const common::IClock& clock_;
    TopicEventBusOptions  options_;
    SubscriptionRegistry  registry_;

    mutable std::mutex      mutex_;   // guards everything below except worker_
    std::condition_variable idle_;
    State                   state_{State::NotStarted};
    bool                    stop_{false};   // tells the dispatcher to exit
    bool                    busy_{false};
    std::uint64_t           sequence_{0};
    std::uint64_t           sent_own_{0};
    Stats                   stats_{};

    std::mutex  join_mutex_;
    std::thread worker_;
};

}  // namespace trading_engine::events
