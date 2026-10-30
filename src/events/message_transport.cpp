#include "trading_engine/events/message_transport.hpp"

#include <stdexcept>
#include <utility>

namespace trading_engine::events {

IMessageTransport::~IMessageTransport() = default;

// --- InMemoryBroker ----------------------------------------------------------------

std::size_t InMemoryBroker::size() const {
    const std::lock_guard lock{mutex_};
    return log_.size();
}

std::vector<TransportMessage> InMemoryBroker::messages(const std::string& topic) const {
    const std::lock_guard lock{mutex_};
    std::vector<TransportMessage> out;
    for (const TransportMessage& m : log_) {
        if (m.topic == topic) {
            out.push_back(m);
        }
    }
    return out;
}

void InMemoryBroker::append(TransportMessage message) {
    {
        const std::lock_guard lock{mutex_};
        log_.push_back(std::move(message));
    }
    appended_.notify_all();
}

std::optional<TransportMessage> InMemoryBroker::next(std::size_t& position,
                                                     const std::set<std::string>& topics,
                                                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock{mutex_};
    while (true) {
        for (; position < log_.size(); ++position) {
            if (topics.count(log_[position].topic) != 0) {
                return log_[position++];
            }
        }
        if (appended_.wait_until(lock, deadline) == std::cv_status::timeout &&
            position >= log_.size()) {
            return std::nullopt;
        }
    }
}

// --- InMemoryTransport -------------------------------------------------------------

InMemoryTransport::InMemoryTransport(std::shared_ptr<InMemoryBroker> broker, bool from_beginning)
    : broker_{std::move(broker)}, from_beginning_{from_beginning} {
    if (!broker_) {
        throw std::invalid_argument("InMemoryTransport: broker must not be null");
    }
}

bool InMemoryTransport::send(TransportMessage message) {
    if (refuse_sends_) {
        return false;
    }
    broker_->append(std::move(message));
    return true;
}

void InMemoryTransport::subscribe(const std::vector<std::string>& topics) {
    topics_   = std::set<std::string>(topics.begin(), topics.end());
    position_ = from_beginning_ ? 0 : broker_->size();
}

std::optional<TransportMessage> InMemoryTransport::poll(std::chrono::milliseconds timeout) {
    if (topics_.empty()) {
        return std::nullopt;
    }
    return broker_->next(position_, topics_, timeout);
}

bool InMemoryTransport::flush(std::chrono::milliseconds /*timeout*/) {
    return true;   // send() appends synchronously: nothing is ever in flight
}

}  // namespace trading_engine::events
