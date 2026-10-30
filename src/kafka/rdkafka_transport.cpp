#include "trading_engine/kafka/rdkafka_transport.hpp"

#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>

#include <cstring>
#include <thread>
#include <utility>

namespace trading_engine::kafka {

namespace {

int to_ms(std::chrono::milliseconds d) { return static_cast<int>(d.count()); }

[[noreturn]] void fail(const std::string& what) { throw KafkaError("kafka: " + what); }

void set(rd_kafka_conf_t* conf, const std::string& key, const std::string& value) {
    char error[512];
    if (rd_kafka_conf_set(conf, key.c_str(), value.c_str(), error, sizeof error) !=
        RD_KAFKA_CONF_OK) {
        rd_kafka_conf_destroy(conf);
        fail("config '" + key + "': " + error);
    }
}

struct HandleDeleter {
    void operator()(rd_kafka_t* rk) const noexcept { rd_kafka_destroy(rk); }
};
using HandlePtr = std::unique_ptr<rd_kafka_t, HandleDeleter>;

struct PartitionListDeleter {
    void operator()(rd_kafka_topic_partition_list_t* l) const noexcept {
        rd_kafka_topic_partition_list_destroy(l);
    }
};

struct MetadataDeleter {
    void operator()(const rd_kafka_metadata_t* m) const noexcept { rd_kafka_metadata_destroy(m); }
};

}  // namespace

// --- RdKafkaTransport --------------------------------------------------------------

struct RdKafkaTransport::Handles {
    HandlePtr producer;
    HandlePtr consumer;
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> send_refused{0};
    std::atomic<std::uint64_t> delivery_failures{0};
    std::atomic<std::uint64_t> consume_errors{0};
};

namespace {

void on_delivery(rd_kafka_t* /*rk*/, const rd_kafka_message_t* message, void* opaque) {
    if (message->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        static_cast<std::atomic<std::uint64_t>*>(opaque)->fetch_add(1);
    }
}

rd_kafka_conf_t* base_conf(const KafkaTransportConfig& config, const std::string& role) {
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    set(conf, "bootstrap.servers", config.bootstrap_servers);
    set(conf, "client.id", config.client_id + "-" + role);
    return conf;
}

void apply_extra(rd_kafka_conf_t* conf, const KafkaTransportConfig& config) {
    for (const auto& [key, value] : config.properties) {
        set(conf, key, value);
    }
}

HandlePtr make_handle(rd_kafka_type_t type, rd_kafka_conf_t* conf) {
    char error[512];
    rd_kafka_t* rk = rd_kafka_new(type, conf, error, sizeof error);   // takes conf on success
    if (rk == nullptr) {
        rd_kafka_conf_destroy(conf);
        fail(std::string("cannot create client: ") + error);
    }
    return HandlePtr{rk};
}

}  // namespace

RdKafkaTransport::RdKafkaTransport(KafkaTransportConfig config)
    : config_{std::move(config)}, handles_{std::make_unique<Handles>()} {
    rd_kafka_conf_t* producer_conf = base_conf(config_, "producer");
    set(producer_conf, "acks", "all");
    set(producer_conf, "enable.idempotence", "true");
    set(producer_conf, "linger.ms", "1");
    apply_extra(producer_conf, config_);
    rd_kafka_conf_set_dr_msg_cb(producer_conf, on_delivery);
    rd_kafka_conf_set_opaque(producer_conf, &handles_->delivery_failures);
    handles_->producer = make_handle(RD_KAFKA_PRODUCER, producer_conf);

    rd_kafka_conf_t* consumer_conf = base_conf(config_, "consumer");
    // group.id is required by the consumer API even though partitions are
    // assigned manually; nothing is ever committed under it.
    set(consumer_conf, "group.id", config_.client_id + "-bus");
    set(consumer_conf, "enable.auto.commit", "false");
    set(consumer_conf, "enable.partition.eof", "false");
    set(consumer_conf, "allow.auto.create.topics", "true");
    set(consumer_conf, "fetch.wait.max.ms", std::to_string(config_.fetch_wait_max.count()));
    apply_extra(consumer_conf, config_);
    handles_->consumer = make_handle(RD_KAFKA_CONSUMER, consumer_conf);
    rd_kafka_poll_set_consumer(handles_->consumer.get());
}

RdKafkaTransport::~RdKafkaTransport() {
    if (handles_->producer) {
        rd_kafka_flush(handles_->producer.get(), to_ms(config_.timeout));
    }
    if (handles_->consumer) {
        rd_kafka_consumer_close(handles_->consumer.get());
    }
}

bool RdKafkaTransport::send(events::TransportMessage message) {
    rd_kafka_t* rk = handles_->producer.get();
    const rd_kafka_resp_err_t err = rd_kafka_producev(
        rk, RD_KAFKA_V_TOPIC(message.topic.c_str()),
        RD_KAFKA_V_KEY(message.key.data(), message.key.size()),
        RD_KAFKA_V_VALUE(message.payload.data(), message.payload.size()),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY), RD_KAFKA_V_END);
    rd_kafka_poll(rk, 0);   // serve delivery reports
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        handles_->send_refused.fetch_add(1);
        return false;
    }
    handles_->sent.fetch_add(1);
    return true;
}

bool RdKafkaTransport::flush(std::chrono::milliseconds timeout) {
    return rd_kafka_flush(handles_->producer.get(), to_ms(timeout)) == RD_KAFKA_RESP_ERR_NO_ERROR;
}

namespace {

void create_missing_topics(rd_kafka_t* rk, const std::vector<std::string>& topics,
                           const KafkaTransportConfig& config) {
    std::vector<rd_kafka_NewTopic_t*> requests;
    char error[512];
    for (const std::string& topic : topics) {
        rd_kafka_NewTopic_t* t = rd_kafka_NewTopic_new(topic.c_str(), config.partitions,
                                                       config.replication_factor, error,
                                                       sizeof error);
        if (t == nullptr) {
            rd_kafka_NewTopic_destroy_array(requests.data(), requests.size());
            fail("topic '" + topic + "': " + error);
        }
        requests.push_back(t);
    }

    rd_kafka_queue_t* queue = rd_kafka_queue_new(rk);
    rd_kafka_AdminOptions_t* options = rd_kafka_AdminOptions_new(rk, RD_KAFKA_ADMIN_OP_CREATETOPICS);
    rd_kafka_AdminOptions_set_operation_timeout(options, to_ms(config.timeout), error, sizeof error);
    rd_kafka_CreateTopics(rk, requests.data(), requests.size(), options, queue);
    rd_kafka_event_t* event = rd_kafka_queue_poll(queue, to_ms(config.timeout) + 1000);

    std::string problem;
    if (event == nullptr) {
        problem = "CreateTopics timed out";
    } else if (const rd_kafka_CreateTopics_result_t* result = rd_kafka_event_CreateTopics_result(event)) {
        std::size_t count = 0;
        const rd_kafka_topic_result_t** results = rd_kafka_CreateTopics_result_topics(result, &count);
        for (std::size_t i = 0; i < count; ++i) {
            const rd_kafka_resp_err_t err = rd_kafka_topic_result_error(results[i]);
            if (err != RD_KAFKA_RESP_ERR_NO_ERROR && err != RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS) {
                problem = std::string(rd_kafka_topic_result_name(results[i])) + ": " +
                          rd_kafka_err2str(err);
            }
        }
    } else if (rd_kafka_event_error(event) != RD_KAFKA_RESP_ERR_NO_ERROR) {
        problem = rd_kafka_event_error_string(event);
    }

    if (event != nullptr) {
        rd_kafka_event_destroy(event);
    }
    rd_kafka_AdminOptions_destroy(options);
    rd_kafka_queue_destroy(queue);
    rd_kafka_NewTopic_destroy_array(requests.data(), requests.size());
    if (!problem.empty()) {
        fail("cannot create topics: " + problem);
    }
}

// Partition count of `topic`, retrying until its metadata has leaders (a
// freshly created topic can take a moment to appear) or the deadline passes.
int partition_count(rd_kafka_t* rk, const std::string& topic,
                    std::chrono::steady_clock::time_point deadline) {
    std::string last = "no metadata";
    while (std::chrono::steady_clock::now() < deadline) {
        rd_kafka_topic_t* handle = rd_kafka_topic_new(rk, topic.c_str(), nullptr);
        const rd_kafka_metadata_t* raw = nullptr;
        const rd_kafka_resp_err_t err = rd_kafka_metadata(rk, 0, handle, &raw, 5000);
        rd_kafka_topic_destroy(handle);
        if (err == RD_KAFKA_RESP_ERR_NO_ERROR) {
            const std::unique_ptr<const rd_kafka_metadata_t, MetadataDeleter> metadata{raw};
            if (metadata->topic_cnt == 1) {
                const rd_kafka_metadata_topic_t& t = metadata->topics[0];
                bool leaders = t.partition_cnt > 0 && t.err == RD_KAFKA_RESP_ERR_NO_ERROR;
                for (int p = 0; leaders && p < t.partition_cnt; ++p) {
                    leaders = t.partitions[p].leader >= 0 &&
                              t.partitions[p].err == RD_KAFKA_RESP_ERR_NO_ERROR;
                }
                if (leaders) {
                    return t.partition_cnt;
                }
                last = rd_kafka_err2str(t.err);
            }
        } else {
            last = rd_kafka_err2str(err);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    fail("no usable metadata for topic '" + topic + "': " + last);
}

}  // namespace

void RdKafkaTransport::subscribe(const std::vector<std::string>& topics) {
    rd_kafka_t* consumer = handles_->consumer.get();
    if (config_.create_topics) {
        create_missing_topics(handles_->producer.get(), topics, config_);
    }

    const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
    const std::unique_ptr<rd_kafka_topic_partition_list_t, PartitionListDeleter> assignment{
        rd_kafka_topic_partition_list_new(static_cast<int>(topics.size()))};
    for (const std::string& topic : topics) {
        const int partitions = partition_count(consumer, topic, deadline);
        for (int p = 0; p < partitions; ++p) {
            std::int64_t low = 0;
            std::int64_t high = 0;
            rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
            // A topic created a moment ago can list a leader in its metadata
            // before that broker serves it; retry those answers until the deadline.
            do {
                err = rd_kafka_query_watermark_offsets(consumer, topic.c_str(), p, &low, &high,
                                                       to_ms(config_.timeout));
                const bool settling = err == RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION ||
                                      err == RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE ||
                                      err == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;
                if (!settling || std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            } while (true);
            if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
                fail("watermarks of " + topic + "[" + std::to_string(p) + "]: " + rd_kafka_err2str(err));
            }
            rd_kafka_topic_partition_list_add(assignment.get(), topic.c_str(), p)->offset =
                config_.from_beginning ? low : high;
        }
    }
    const rd_kafka_resp_err_t err = rd_kafka_assign(consumer, assignment.get());
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        fail(std::string("assign: ") + rd_kafka_err2str(err));
    }
}

std::optional<events::TransportMessage> RdKafkaTransport::poll(std::chrono::milliseconds timeout) {
    rd_kafka_poll(handles_->producer.get(), 0);   // keep delivery reports flowing
    rd_kafka_message_t* message = rd_kafka_consumer_poll(handles_->consumer.get(), to_ms(timeout));
    if (message == nullptr) {
        return std::nullopt;
    }
    std::optional<events::TransportMessage> out;
    if (message->err == RD_KAFKA_RESP_ERR_NO_ERROR) {
        events::TransportMessage m;
        m.topic = rd_kafka_topic_name(message->rkt);
        if (message->key != nullptr) {
            m.key.assign(static_cast<const char*>(message->key), message->key_len);
        }
        if (message->payload != nullptr) {
            m.payload.assign(static_cast<const char*>(message->payload), message->len);
        }
        out = std::move(m);
    } else if (message->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
        handles_->consume_errors.fetch_add(1);
    }
    rd_kafka_message_destroy(message);
    return out;
}

RdKafkaTransport::Stats RdKafkaTransport::stats() const noexcept {
    Stats s;
    s.sent              = handles_->sent.load();
    s.send_refused      = handles_->send_refused.load();
    s.delivery_failures = handles_->delivery_failures.load();
    s.consume_errors    = handles_->consume_errors.load();
    return s;
}

// --- KafkaMockCluster ----------------------------------------------------------------

struct KafkaMockCluster::Impl {
    HandlePtr owner;   // the mock cluster runs on this handle's threads
    rd_kafka_mock_cluster_t* cluster{nullptr};
};

KafkaMockCluster::KafkaMockCluster(int brokers) : impl_{std::make_unique<Impl>()} {
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    set(conf, "client.id", "trading_engine-mock-cluster");
    set(conf, "log_level", "3");   // this handle never connects anywhere; hush its config warning
    impl_->owner   = make_handle(RD_KAFKA_PRODUCER, conf);
    impl_->cluster = rd_kafka_mock_cluster_new(impl_->owner.get(), brokers);
    if (impl_->cluster == nullptr) {
        fail("cannot start the mock cluster");
    }
}

KafkaMockCluster::~KafkaMockCluster() {
    if (impl_->cluster != nullptr) {
        rd_kafka_mock_cluster_destroy(impl_->cluster);
    }
}

std::string KafkaMockCluster::bootstrap_servers() const {
    return rd_kafka_mock_cluster_bootstraps(impl_->cluster);
}

void KafkaMockCluster::create_topic(const std::string& topic, int partitions) {
    const rd_kafka_resp_err_t err =
        rd_kafka_mock_topic_create(impl_->cluster, topic.c_str(), partitions, 1);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        fail("mock topic '" + topic + "': " + rd_kafka_err2str(err));
    }
}

}  // namespace trading_engine::kafka
