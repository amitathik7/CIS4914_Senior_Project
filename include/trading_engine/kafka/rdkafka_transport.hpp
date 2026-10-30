#pragma once

// -----------------------------------------------------------------------------
//  RdKafkaTransport -- events::IMessageTransport over Apache Kafka (librdkafka).
//
//  Part of trading_engine_kafka_adapter, built only with
//  -DTRADING_ENGINE_WITH_KAFKA=ON. librdkafka stays PRIVATE to the adapter:
//  this header exposes no librdkafka type, so nothing that includes it needs
//  the Kafka headers.
//
//  Plug it into a TopicEventBus:
//
//      kafka::RdKafkaTransport transport{{.bootstrap_servers = "localhost:9092"}};
//      events::TopicEventBus   bus{transport, clock, {.topic_prefix = "te.run42"}};
//
//  Behaviour:
//   * One producer handle and one consumer handle. Producer: acks=all,
//     idempotence on (no duplicates or reordering on retry), small linger for
//     latency. Messages are keyed, so librdkafka's default partitioner keeps
//     one key (symbol) on one partition.
//   * subscribe() creates any missing topic (create_topics), then ASSIGNS
//     every partition explicitly -- no consumer group, no rebalance wait. Each
//     transport therefore reads every message on its topics, which is what an
//     event bus needs: every process sees every event. The start offset is the
//     partition's high watermark at that moment (or the low watermark with
//     from_beginning), queried synchronously so nothing produced after
//     subscribe() returns can be missed.
//   * send() never blocks: a full local queue makes it return false.
//     Asynchronous delivery failures are counted in stats().
//   * poll() hides partition-EOF markers and transient errors (counted).
//
//  Testing without a broker: KafkaMockCluster wraps librdkafka's built-in mock
//  cluster (a real Kafka protocol implementation inside the process). Point
//  bootstrap_servers at its bootstrap_servers() and everything above runs
//  unchanged. The unit tests do exactly that.
//
//  Thread-safety: send() and flush() from any thread; subscribe() and poll()
//  from the bus dispatcher only (TopicEventBus guarantees this).
// -----------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "trading_engine/events/message_transport.hpp"

namespace trading_engine::kafka {

class KafkaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct KafkaTransportConfig {
    std::string bootstrap_servers{"localhost:9092"};
    std::string client_id{"trading_engine"};
    bool        create_topics{true};
    int         partitions{3};             // for topics this transport creates
    int         replication_factor{1};     // ditto; 1 suits a single local broker
    bool        from_beginning{false};     // replay the topics' retained history
    std::chrono::milliseconds timeout{std::chrono::seconds{15}};   // metadata / admin / assign
    // Extra librdkafka properties, applied to both handles after the defaults
    // (e.g. {"security.protocol", "SASL_SSL"}). Unknown keys throw KafkaError.
    std::map<std::string, std::string> properties{};
};

class RdKafkaTransport final : public events::IMessageTransport {
public:
    struct Stats {
        std::uint64_t sent{0};                // accepted by the producer queue
        std::uint64_t send_refused{0};        // send() returned false
        std::uint64_t delivery_failures{0};   // broker never acknowledged a message
        std::uint64_t consume_errors{0};      // errors poll() swallowed
    };

    // Connects lazily (librdkafka does); throws KafkaError on bad configuration.
    explicit RdKafkaTransport(KafkaTransportConfig config);
    ~RdKafkaTransport() override;

    RdKafkaTransport(const RdKafkaTransport&)            = delete;
    RdKafkaTransport& operator=(const RdKafkaTransport&) = delete;

    bool send(events::TransportMessage message) override;
    void subscribe(const std::vector<std::string>& topics) override;   // throws KafkaError
    [[nodiscard]] std::optional<events::TransportMessage> poll(std::chrono::milliseconds timeout) override;
    bool flush(std::chrono::milliseconds timeout) override;

    [[nodiscard]] Stats stats() const noexcept;

private:
    struct Handles;   // librdkafka state, defined in the .cpp
    KafkaTransportConfig     config_;
    std::unique_ptr<Handles> handles_;
};

// An in-process Kafka cluster (librdkafka's mock cluster) for tests and demos
// without a broker. Lives until destroyed; transports must be destroyed first.
class KafkaMockCluster {
public:
    explicit KafkaMockCluster(int brokers = 1);
    ~KafkaMockCluster();

    KafkaMockCluster(const KafkaMockCluster&)            = delete;
    KafkaMockCluster& operator=(const KafkaMockCluster&) = delete;

    [[nodiscard]] std::string bootstrap_servers() const;
    // Pre-creates a topic (the mock cluster also auto-creates on first use).
    void create_topic(const std::string& topic, int partitions);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace trading_engine::kafka
