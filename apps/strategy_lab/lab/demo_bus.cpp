#include "lab/demo_bus.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

namespace trading_engine::lab {

SynchronousDemoBus::SynchronousDemoBus(const common::IClock& clock, Fault fault)
    : clock_{clock}, fault_{fault} {}

SynchronousDemoBus::~SynchronousDemoBus() = default;

void SynchronousDemoBus::require_owner_thread() {
    const std::thread::id current = std::this_thread::get_id();
    if (owner_ == std::thread::id{}) {
        owner_ = current;
    } else if (owner_ != current) {
        throw std::logic_error(
            "SynchronousDemoBus is single-threaded by design: it was used from a second thread");
    }
}

bool SynchronousDemoBus::still_subscribed(common::SubscriptionId id) const noexcept {
    return std::any_of(subscriptions_.begin(), subscriptions_.end(),
                       [id](const Subscription& s) { return s.id == id; });
}

std::size_t SynchronousDemoBus::subscription_count(events::EventType type) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        subscriptions_.begin(), subscriptions_.end(), [type](const Subscription& s) { return s.type == type; }));
}

bool SynchronousDemoBus::publish(events::Event event) {
    require_owner_thread();
    if (state_ != State::Running) {
        ++counters_.rejected_not_running;
        return false;
    }

    if (event.type == events::EventType::Signal && fault_ != Fault::None) {
        ++counters_.refused_by_fault;
        FailedPublication failure;
        failure.event = std::move(event);
        if (fault_ == Fault::RejectSignals) {
            failure.kind    = FailedPublication::Kind::Rejected;
            failure.message = "SynchronousDemoBus: injected fault reject-signals (publish returned false)";
            failures_.push_back(std::move(failure));
            return false;
        }
        failure.kind    = FailedPublication::Kind::Threw;
        failure.message = "SynchronousDemoBus: injected fault throw-on-signal";
        const std::string message = failure.message;
        failures_.push_back(std::move(failure));
        throw std::runtime_error(message);
    }

    event.sequence    = ++sequence_;
    event.enqueued_at = clock_.now();
    ++counters_.published_by_type[static_cast<std::size_t>(event.type)];
    deliver(event);
    return true;
}

void SynchronousDemoBus::deliver(const events::Event& event) {
    if (depth_ >= kMaxDepth) {
        throw std::logic_error("SynchronousDemoBus: publication nested more than " + std::to_string(kMaxDepth) +
                               " levels deep (a handler is publishing in a loop)");
    }
    // A snapshot, so a handler may subscribe or unsubscribe while the delivery runs.
    std::vector<Subscription> targets;
    for (const Subscription& s : subscriptions_) {
        if (s.type == event.type) {
            targets.push_back(s);
        }
    }

    struct DepthScope {
        std::size_t& depth;
        explicit DepthScope(std::size_t& d) : depth{d} { ++depth; }
        ~DepthScope() { --depth; }
    } scope{depth_};

    for (const Subscription& target : targets) {
        if (still_subscribed(target.id)) {
            target.handler(event);
        }
    }
}

common::SubscriptionId SynchronousDemoBus::subscribe(events::EventType type, events::EventHandler handler) {
    require_owner_thread();
    if (!handler) {
        throw std::invalid_argument("SynchronousDemoBus::subscribe: the handler is empty");
    }
    const common::SubscriptionId id = subscription_ids_.next();
    subscriptions_.push_back(Subscription{id, type, std::move(handler)});
    return id;
}

void SynchronousDemoBus::unsubscribe(common::SubscriptionId id) {
    require_owner_thread();
    std::erase_if(subscriptions_, [id](const Subscription& s) { return s.id == id; });
}

void SynchronousDemoBus::start() {
    require_owner_thread();
    if (state_ == State::ShutDown) {
        throw std::logic_error("SynchronousDemoBus::start: the bus was shut down and cannot be restarted");
    }
    state_ = State::Running;
}

void SynchronousDemoBus::request_shutdown() {
    require_owner_thread();
    state_ = State::ShutDown;
}

void SynchronousDemoBus::wait_until_drained() {
    require_owner_thread();   // nothing was ever queued, so there is nothing to wait for
}

}  // namespace trading_engine::lab
