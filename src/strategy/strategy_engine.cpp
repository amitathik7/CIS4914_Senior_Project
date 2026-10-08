#include "trading_engine/strategy/strategy_engine.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "trading_engine/common/errors.hpp"
#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::strategy {

namespace {

// Text of the exception being handled. Only valid inside a catch block.
std::string describe_current_exception() {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "non-standard exception";
    }
}

// Keeps the text of an exception that is being absorbed. It must not throw
// itself: running out of memory while copying the message costs the text, not
// the counter or the caller's isolation.
void remember_current_exception(std::string& slot, std::string_view where) noexcept {
    try {
        slot.assign(where);
        slot += ": ";
        slot += describe_current_exception();
    } catch (...) {
        // Deliberately empty: there is nowhere left to report this.
    }
}

}  // namespace

// One registered strategy. It doubles as the ISignalSink handed to that
// strategy, which is how emit() knows who is emitting. Heap-allocated and never
// moved, so the reference the strategy receives stays valid until the engine
// is destroyed.
class StrategyEngine::Registration final : public ISignalSink {
public:
    Registration(StrategyEngine& owning_engine, std::shared_ptr<IStrategy> impl, std::string strategy_id)
        : owner_{owning_engine}, strategy_{std::move(impl)} {
        stats_.id = std::move(strategy_id);
    }

    void emit(const domain::TradeSignal& signal) override;

    // Engine-side entry points. Neither throws.
    void deliver(const domain::MarketEvent& event) noexcept;
    void stop() noexcept;

    [[nodiscard]] IStrategy& strategy() const noexcept { return *strategy_; }
    [[nodiscard]] const StrategyStats& stats() const noexcept { return stats_; }

private:
    StrategyEngine&            owner_;
    std::shared_ptr<IStrategy> strategy_;
    StrategyStats              stats_{};
};

void StrategyEngine::Registration::emit(const domain::TradeSignal& signal) {
    // A sink is only live inside the callback it was handed to. A retained
    // reference used later (from on_stop(), after the callback returned, ...)
    // must not reach the bus: the engine may be stopped, and the id and
    // timestamp would belong to no event.
    if (owner_.active_ != this) {
        ++stats_.signals_dropped;
        return;
    }

    domain::TradeSignal stamped{signal};   // the strategy's own object is never touched
    stamped.created_at  = owner_.clock_.now();
    stamped.id          = owner_.signal_ids_.next();
    stamped.strategy_id = stats_.id;

    events::Event event;
    event.type    = events::EventType::Signal;
    event.payload = std::move(stamped);

    bool accepted = false;
    try {
        accepted = owner_.bus_.publish(std::move(event));
    } catch (...) {
        // The bus failed, not the strategy: count it and let the strategy carry on.
        ++stats_.publish_errors;
        remember_current_exception(stats_.last_error, "publish");
        return;
    }

    if (accepted) {
        ++stats_.signals_published;
    } else {
        ++stats_.signals_rejected;
    }
}

void StrategyEngine::Registration::deliver(const domain::MarketEvent& event) noexcept {
    ++stats_.events_delivered;
    owner_.active_ = this;
    try {
        strategy_->on_market_event(event, *this);
    } catch (...) {
        ++stats_.errors;
        remember_current_exception(stats_.last_error, "on_market_event");
    }
    owner_.active_ = nullptr;
}

void StrategyEngine::Registration::stop() noexcept {
    try {
        strategy_->on_stop();
    } catch (...) {
        ++stats_.errors;
        remember_current_exception(stats_.last_error, "on_stop");
    }
}

StrategyEngine::StrategyEngine(events::IEventBus& bus, const common::IClock& clock)
    : bus_{bus}, clock_{clock} {}

StrategyEngine::~StrategyEngine() {
    stop();
}

void StrategyEngine::register_strategy(std::shared_ptr<IStrategy> strategy) {
    // Setup-time wiring: the registry has to work before any events flow, and
    // its input is validated at this boundary. Everything is checked before
    // anything is stored, so a rejected call leaves the engine unchanged.
    if (state_ != State::Stopped) {
        throw std::logic_error(
            "StrategyEngine::register_strategy: strategies can only be registered while "
            "the engine is stopped");
    }
    if (strategy == nullptr) {
        throw common::ValidationError(
            "StrategyEngine::register_strategy: strategy must not be null");
    }

    std::string id{strategy->id()};   // captured once; the engine never asks again
    if (id.empty()) {
        throw common::ValidationError(
            "StrategyEngine::register_strategy: strategy id must not be empty");
    }
    const bool taken = std::any_of(registrations_.begin(), registrations_.end(),
                                   [&id](const std::unique_ptr<Registration>& existing) {
                                       return existing->stats().id == id;
                                   });
    if (taken) {
        throw common::ValidationError(
            "StrategyEngine::register_strategy: duplicate strategy id '" + id + "'");
    }

    registrations_.push_back(
        std::make_unique<Registration>(*this, std::move(strategy), std::move(id)));
}

void StrategyEngine::start() {
    if (state_ != State::Stopped) {
        return;   // already running (or mid-transition): never subscribe twice
    }
    state_ = State::Starting;

    std::size_t started = 0;   // strategies whose on_start() returned
    try {
        for (const auto& registration : registrations_) {
            registration->strategy().on_start();
            ++started;
        }

        // Subscribe last, so no event can reach a strategy that has not started.
        // The handler captures a weak token rather than `this`; see Gate.
        auto gate    = std::make_shared<Gate>();
        gate->engine = this;

        // Running is set BEFORE subscribing: once subscribe() returns the bus
        // may call the handler from another thread, and from then on start()
        // must write nothing that handler reads. (If subscribe() throws, the
        // catch below puts the state back.)
        state_ = State::Running;
        subscription_ = bus_.subscribe(
            events::EventType::MarketData,
            [token = std::weak_ptr<Gate>{gate}](const events::Event& event) {
                const std::shared_ptr<Gate> live = token.lock();
                if (live == nullptr) {
                    return;   // the engine stopped or is gone: this handler is stale
                }
                StrategyEngine& engine = *live->engine;
                if (const auto* market = std::get_if<domain::MarketEvent>(&event.payload)) {
                    engine.on_market_event(*market);
                } else {
                    ++engine.events_dropped_;   // a producer broke the MarketData contract
                }
            });
        gate_ = std::move(gate);
    } catch (...) {
        // Undo a partial startup, then report the original failure unchanged.
        const std::exception_ptr failure = std::current_exception();
        stop_strategies(started);
        state_ = State::Stopped;
        std::rethrow_exception(failure);
    }
}

void StrategyEngine::stop() noexcept {
    if (state_ != State::Running) {
        return;   // never started, already stopped, or mid-transition
    }

    // Withdraw from the bus BEFORE changing any state the handler reads: the
    // token first, so a handler the bus still holds can no longer reach this
    // engine even if unsubscribe() fails, then unsubscribe(), which is assumed
    // to be a barrier (see "Threading" in the header). Only once it returns can
    // no handler be running, so only then is it safe to move state_.
    gate_.reset();
    try {
        bus_.unsubscribe(subscription_);
    } catch (...) {
        ++bus_errors_;
    }
    subscription_ = common::SubscriptionId{};

    state_ = State::Stopping;
    stop_strategies(registrations_.size());
    state_ = State::Stopped;
}

void StrategyEngine::on_market_event(const domain::MarketEvent& event) {
    // `active_` is set only while a strategy callback is on the stack, so this
    // also refuses an event that arrives re-entrantly (a synchronous bus
    // handler that feeds one back): strategies must never see overlapping calls.
    if (state_ != State::Running || active_ != nullptr) {
        ++events_dropped_;
        return;
    }

    ++events_routed_;
    for (const auto& registration : registrations_) {
        registration->deliver(event);
    }
}

void StrategyEngine::stop_strategies(std::size_t count) noexcept {
    for (std::size_t i = count; i-- > 0;) {
        registrations_[i]->stop();
    }
}

StrategyEngine::Stats StrategyEngine::stats() const {
    Stats total;
    total.events_routed  = events_routed_;
    total.events_dropped = events_dropped_;
    total.bus_errors     = bus_errors_;
    for (const auto& registration : registrations_) {
        const StrategyStats& each = registration->stats();
        total.strategy_errors   += each.errors;
        total.signals_published += each.signals_published;
        total.signals_rejected  += each.signals_rejected;
        total.publish_errors    += each.publish_errors;
        total.signals_dropped   += each.signals_dropped;
    }
    return total;
}

std::vector<StrategyEngine::StrategyStats> StrategyEngine::strategy_stats() const {
    std::vector<StrategyStats> all;
    all.reserve(registrations_.size());
    for (const auto& registration : registrations_) {
        all.push_back(registration->stats());
    }
    return all;
}

}  // namespace trading_engine::strategy
