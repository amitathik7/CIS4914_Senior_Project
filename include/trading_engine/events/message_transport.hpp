#pragma once

// -----------------------------------------------------------------------------
//  IMessageTransport -- the narrow seam between TopicEventBus and a broker.
//
//  TopicEventBus owns everything engine-specific (encoding, topic names,
//  ordering keys, dispatch, lifecycle); a transport only moves opaque bytes
//  to and from named topics. Two implementations:
//   * kafka::RdKafkaTransport (trading_engine_kafka_adapter, librdkafka) --
//     production. Built only with -DTRADING_ENGINE_WITH_KAFKA=ON so the core
//     never links a broker client.
//   * InMemoryBroker / InMemoryTransport (below) -- a broker-free stand-in
//     with the same visible semantics, for unit tests and for running the
//     topic bus without Kafka.
//
//  Semantics a transport must provide:
//   * send() is thread-safe and non-blocking (or nearly so); it returns false
//     if the message was refused locally (queue full, closed). Delivery to the
//     broker may complete later; flush() waits for it.
//   * subscribe() is called once, before the first poll(). Consumption starts
//     at the end of each topic as it is at that moment (so a process sees
//     every message produced after it subscribed, and none from before),
//     unless the transport was configured to read from the beginning.
//   * poll() is called from exactly one thread (the bus dispatcher). Messages
//     with the same topic and key are returned in send order.
// -----------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace trading_engine::events {

struct TransportMessage {
    std::string topic{};
    std::string key{};       // partitioning / ordering key (the symbol)
    std::string payload{};   // encoded WireEnvelope
};

class IMessageTransport {
public:
    virtual ~IMessageTransport();

    virtual bool send(TransportMessage message) = 0;
    virtual void subscribe(const std::vector<std::string>& topics) = 0;
    [[nodiscard]] virtual std::optional<TransportMessage> poll(std::chrono::milliseconds timeout) = 0;
    // true once every message handed to send() has reached the broker.
    virtual bool flush(std::chrono::milliseconds timeout) = 0;
};

// One shared, append-only log of messages: what a Kafka cluster is to several
// processes. Thread-safe. Create one per test (or per in-process run) and
// attach as many InMemoryTransport endpoints as there are simulated processes.
class InMemoryBroker {
public:
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::vector<TransportMessage> messages(const std::string& topic) const;

private:
    friend class InMemoryTransport;

    void append(TransportMessage message);
    // Next message at or after `position` on one of `topics`, waiting up to
    // `timeout`. Advances `position` past it.
    std::optional<TransportMessage> next(std::size_t& position, const std::set<std::string>& topics,
                                         std::chrono::milliseconds timeout);

    mutable std::mutex mutex_;
    std::condition_variable appended_;
    std::vector<TransportMessage> log_{};
};

class InMemoryTransport final : public IMessageTransport {
public:
    explicit InMemoryTransport(std::shared_ptr<InMemoryBroker> broker, bool from_beginning = false);

    bool send(TransportMessage message) override;
    void subscribe(const std::vector<std::string>& topics) override;
    [[nodiscard]] std::optional<TransportMessage> poll(std::chrono::milliseconds timeout) override;
    bool flush(std::chrono::milliseconds timeout) override;

    // Fault injection for tests: while set, send() refuses every message.
    void refuse_sends(bool refuse) noexcept { refuse_sends_ = refuse; }

private:
    std::shared_ptr<InMemoryBroker> broker_;
    bool from_beginning_;
    std::atomic<bool> refuse_sends_{false};
    std::set<std::string> topics_{};
    std::size_t position_{0};
};

}  // namespace trading_engine::events
