#include "trading_engine/events/event_bus.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace trading_engine::events {

IEventBus::~IEventBus() = default;

// --- SubscriptionRegistry -------------------------------------------------------

common::SubscriptionId SubscriptionRegistry::add(EventType type, EventHandler handler) {
    if (!handler) {
        throw std::invalid_argument("event bus: subscribe() needs a non-empty handler");
    }
    auto entry = std::make_shared<Entry>();
    entry->type    = type;
    entry->handler = std::move(handler);

    const std::lock_guard lock{mutex_};
    entry->id = common::SubscriptionId{++next_id_};
    entries_.push_back(entry);
    return entry->id;
}

void SubscriptionRegistry::remove(common::SubscriptionId id) {
    {
        const std::lock_guard lock{mutex_};
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [id](const auto& e) { return e->id == id; });
        if (it == entries_.end()) {
            return;   // unknown or already removed: nothing to wait for
        }
        (*it)->active.store(false);
        entries_.erase(it);
    }
    if (!on_dispatcher()) {
        // Barrier: a delivery that read active == true before we cleared it
        // holds delivery_mutex_ until the handler returns.
        const std::lock_guard wait_for_delivery{delivery_mutex_};
    }
}

std::size_t SubscriptionRegistry::deliver(const Event& event) {
    std::vector<std::shared_ptr<Entry>> targets;
    {
        const std::lock_guard lock{mutex_};
        for (const auto& entry : entries_) {
            if (entry->type == event.type) {
                targets.push_back(entry);
            }
        }
    }
    std::size_t errors = 0;
    for (const auto& target : targets) {
        const std::lock_guard delivering{delivery_mutex_};
        if (!target->active.load()) {
            continue;   // unsubscribed earlier in this delivery
        }
        try {
            target->handler(event);
        } catch (...) {
            ++errors;
        }
    }
    return errors;
}

std::size_t SubscriptionRegistry::count(EventType type) const {
    const std::lock_guard lock{mutex_};
    return static_cast<std::size_t>(std::count_if(
        entries_.begin(), entries_.end(), [type](const auto& e) { return e->type == type; }));
}

// --- InProcessEventBus ------------------------------------------------------------

InProcessEventBus::InProcessEventBus(const common::IClock& clock, InProcessEventBusOptions options)
    : clock_{clock}, options_{options} {
    if (options_.capacity == 0) {
        throw std::invalid_argument("InProcessEventBus: capacity must be > 0");
    }
}

InProcessEventBus::~InProcessEventBus() {
    request_shutdown();
    if (!registry_.on_dispatcher()) {
        wait_until_drained();
    }
}

bool InProcessEventBus::publish(Event event) {
    const bool from_dispatcher = registry_.on_dispatcher();
    std::unique_lock lock{mutex_};

    const auto accepting = [&] {
        return state_ == State::Running || (from_dispatcher && state_ == State::Draining);
    };
    if (!accepting()) {
        ++stats_.rejected;
        return false;
    }
    if (!from_dispatcher && queue_.size() >= options_.capacity) {
        if (options_.when_full == BackpressurePolicy::Reject) {
            ++stats_.rejected;
            return false;
        }
        not_full_.wait(lock, [&] { return queue_.size() < options_.capacity || !accepting(); });
        if (!accepting()) {
            ++stats_.rejected;
            return false;
        }
    }

    event.sequence    = ++sequence_;
    event.enqueued_at = clock_.now();
    queue_.push_back(std::move(event));
    ++stats_.published;
    stats_.high_water_mark = std::max(stats_.high_water_mark, queue_.size());
    lock.unlock();
    not_empty_.notify_one();
    return true;
}

common::SubscriptionId InProcessEventBus::subscribe(EventType type, EventHandler handler) {
    return registry_.add(type, std::move(handler));
}

void InProcessEventBus::unsubscribe(common::SubscriptionId id) { registry_.remove(id); }

void InProcessEventBus::start() {
    const std::lock_guard lock{mutex_};
    if (state_ == State::Running) {
        return;
    }
    if (state_ != State::NotStarted) {
        throw std::logic_error("InProcessEventBus: cannot start after shutdown");
    }
    state_  = State::Running;
    worker_ = std::thread{[this] { run(); }};
}

void InProcessEventBus::request_shutdown() {
    {
        const std::lock_guard lock{mutex_};
        if (state_ == State::Running) {
            state_ = State::Draining;
        } else if (state_ == State::NotStarted) {
            state_ = State::Stopped;
        }
    }
    not_empty_.notify_all();
    not_full_.notify_all();   // blocked producers re-check and are refused
}

void InProcessEventBus::wait_until_drained() {
    if (registry_.on_dispatcher()) {
        throw std::logic_error("InProcessEventBus: wait_until_drained() called from a handler");
    }
    bool join = false;
    {
        std::unique_lock lock{mutex_};
        if (state_ == State::Running) {
            idle_.wait(lock, [&] { return (queue_.empty() && !busy_) || state_ != State::Running; });
        }
        if (state_ == State::Draining || state_ == State::Stopped) {
            idle_.wait(lock, [&] { return state_ == State::Stopped; });
            join = true;
        }
    }
    if (join) {
        const std::lock_guard lock{join_mutex_};
        if (worker_.joinable()) {
            worker_.join();
        }
    }
}

std::size_t InProcessEventBus::depth() const {
    const std::lock_guard lock{mutex_};
    return queue_.size();
}

InProcessEventBus::Stats InProcessEventBus::stats() const {
    const std::lock_guard lock{mutex_};
    return stats_;
}

void InProcessEventBus::run() {
    registry_.bind_dispatcher(std::this_thread::get_id());
    std::unique_lock lock{mutex_};
    while (true) {
        not_empty_.wait(lock, [&] { return !queue_.empty() || state_ != State::Running; });
        if (queue_.empty()) {
            // Draining with nothing left (handlers can only add while busy_).
            state_ = State::Stopped;
            break;
        }
        Event event = std::move(queue_.front());
        queue_.pop_front();
        busy_ = true;
        lock.unlock();
        not_full_.notify_one();

        const std::size_t errors = registry_.deliver(event);

        lock.lock();
        busy_ = false;
        ++stats_.delivered;
        stats_.handler_errors += errors;
        if (queue_.empty()) {
            idle_.notify_all();
        }
    }
    lock.unlock();
    // Thread ids are recycled: forget ours so no later thread is mistaken for it.
    registry_.bind_dispatcher(std::thread::id{});
    idle_.notify_all();
}

}  // namespace trading_engine::events
