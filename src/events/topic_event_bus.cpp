#include "trading_engine/events/topic_event_bus.hpp"

#include <cstdio>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "trading_engine/events/event_codec.hpp"

namespace trading_engine::events {

namespace {

constexpr EventType kAllTypes[] = {EventType::MarketData, EventType::Signal, EventType::Order,
                                   EventType::Fill, EventType::RiskDecision};

std::string random_origin() {
    std::random_device device;
    const std::uint64_t value =
        (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device());
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

bool valid_topic_prefix(const std::string& prefix) {
    if (prefix.empty() || prefix.size() > 200) {
        return false;
    }
    for (const char c : prefix) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

TopicEventBus::TopicEventBus(IMessageTransport& transport, const common::IClock& clock,
                             TopicEventBusOptions options)
    : transport_{transport}, clock_{clock}, options_{std::move(options)} {
    if (!valid_topic_prefix(options_.topic_prefix)) {
        throw std::invalid_argument("TopicEventBus: topic_prefix '" + options_.topic_prefix +
                                    "' must be 1-200 characters of [A-Za-z0-9._-]");
    }
    if (options_.origin.empty()) {
        options_.origin = random_origin();
    }
}

TopicEventBus::~TopicEventBus() {
    request_shutdown();
    if (!registry_.on_dispatcher()) {
        wait_until_drained();
    }
}

std::string TopicEventBus::topic(EventType type) const {
    return options_.topic_prefix + "." + std::string(to_string(type));
}

bool TopicEventBus::publish(Event event) {
    const bool from_dispatcher = registry_.on_dispatcher();
    const std::lock_guard lock{mutex_};
    if (!(state_ == State::Running || (from_dispatcher && state_ == State::Draining))) {
        ++stats_.rejected;
        return false;
    }

    event.enqueued_at = clock_.now();
    event.sequence    = sequence_ + 1;
    TransportMessage message;
    message.topic   = topic(event.type);
    message.key     = partition_key(event);
    message.payload = encode(WireEnvelope{options_.origin, event});   // may throw invalid_argument

    bool sent = false;
    try {
        sent = transport_.send(std::move(message));
    } catch (...) {
        sent = false;
    }
    if (!sent) {
        ++stats_.rejected;   // the sequence number is not consumed
        return false;
    }
    sequence_ = event.sequence;
    ++sent_own_;
    ++stats_.published;
    return true;
}

common::SubscriptionId TopicEventBus::subscribe(EventType type, EventHandler handler) {
    return registry_.add(type, std::move(handler));
}

void TopicEventBus::unsubscribe(common::SubscriptionId id) { registry_.remove(id); }

void TopicEventBus::start() {
    const std::lock_guard lock{mutex_};
    if (state_ == State::Running) {
        return;
    }
    if (state_ != State::NotStarted) {
        throw std::logic_error("TopicEventBus: cannot start after shutdown");
    }
    std::vector<std::string> topics;
    for (const EventType type : kAllTypes) {
        topics.push_back(topic(type));
    }
    // Before the dispatcher exists and before Running: from here on, every
    // message published to these topics will be read back.
    transport_.subscribe(topics);
    state_  = State::Running;
    worker_ = std::thread{[this] { run(); }};
}

void TopicEventBus::request_shutdown() {
    const std::lock_guard lock{mutex_};
    if (state_ == State::Running) {
        state_ = State::Draining;
    } else if (state_ == State::NotStarted) {
        state_ = State::Stopped;
    }
}

void TopicEventBus::wait_until_drained() {
    if (registry_.on_dispatcher()) {
        throw std::logic_error("TopicEventBus: wait_until_drained() called from a handler");
    }
    {
        const std::lock_guard lock{mutex_};
        if (state_ == State::NotStarted || (state_ == State::Stopped && !worker_.joinable())) {
            return;
        }
    }

    transport_.flush(options_.drain_timeout);

    bool join = false;
    {
        std::unique_lock lock{mutex_};
        const bool drained = idle_.wait_for(lock, options_.drain_timeout, [&] {
            return (stats_.received_own == sent_own_ && !busy_) || stop_;
        });
        if (!drained) {
            ++stats_.drain_timeouts;
        }
        if (state_ == State::Draining || state_ == State::Stopped) {
            stop_  = true;
            state_ = State::Stopped;
            join   = true;
        }
    }
    if (join) {
        const std::lock_guard lock{join_mutex_};
        if (worker_.joinable()) {
            worker_.join();
        }
    }
}

std::size_t TopicEventBus::depth() const {
    const std::lock_guard lock{mutex_};
    return static_cast<std::size_t>(sent_own_ - stats_.received_own);
}

TopicEventBus::Stats TopicEventBus::stats() const {
    const std::lock_guard lock{mutex_};
    return stats_;
}

void TopicEventBus::run() {
    registry_.bind_dispatcher(std::this_thread::get_id());
    while (true) {
        {
            const std::lock_guard lock{mutex_};
            if (stop_) {
                break;
            }
        }

        std::optional<TransportMessage> message;
        try {
            message = transport_.poll(options_.poll_interval);
        } catch (...) {
            message.reset();   // a transport hiccup: try again on the next turn
        }
        if (!message) {
            continue;
        }

        WireEnvelope envelope;
        bool own = false;
        {
            const std::lock_guard lock{mutex_};
            ++stats_.received;
            try {
                envelope = decode(message->payload);
            } catch (const DecodeError&) {
                ++stats_.decode_errors;
                continue;
            }
            own   = envelope.origin == options_.origin;
            busy_ = true;
        }

        const std::size_t errors = registry_.deliver(envelope.event);

        {
            const std::lock_guard lock{mutex_};
            busy_ = false;
            stats_.handler_errors += errors;
            if (own) {
                ++stats_.received_own;
            }
        }
        idle_.notify_all();
    }
    registry_.bind_dispatcher(std::thread::id{});
}

}  // namespace trading_engine::events
