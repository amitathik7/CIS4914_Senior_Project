#include "lab/runner.hpp"

#include <memory>
#include <unordered_map>
#include <variant>

#include "lab/errors.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::lab {

std::string_view fault_name(SynchronousDemoBus::Fault fault) noexcept {
    switch (fault) {
        case SynchronousDemoBus::Fault::None:          return "none";
        case SynchronousDemoBus::Fault::RejectSignals: return "reject-signals";
        case SynchronousDemoBus::Fault::ThrowOnSignal: return "throw-on-signal";
    }
    return "none";
}

namespace {

// Detaches every observer when the run ends, however it ends: the collectors live on
// this function's stack and must not be left referenced by the strategies.
class ObserverDetacher {
public:
    explicit ObserverDetacher(std::vector<BuiltStrategy>& strategies) : strategies_{strategies} {}
    ObserverDetacher(const ObserverDetacher&)            = delete;
    ObserverDetacher& operator=(const ObserverDetacher&) = delete;
    ~ObserverDetacher() {
        for (BuiltStrategy& each : strategies_) {
            try {
                each.set_observer(nullptr);
            } catch (...) {
                // Nothing sensible to do while unwinding; the strategy is discarded next.
            }
        }
    }

private:
    std::vector<BuiltStrategy>& strategies_;
};

}  // namespace

ReplayResult run_replay(std::vector<BuiltStrategy> strategies, const std::vector<ReplayEvent>& events,
                        const ReplayOptions& options) {
    ReplayResult result;
    result.bus.fault = std::string{fault_name(options.fault)};

    // Declaration order is destruction order in reverse: the engine goes first, then
    // the bus, then the clock; the collectors outlive all of them.
    std::size_t current_event = 0;
    std::vector<std::unique_ptr<SlotCollector>> collectors;
    ObserverDetacher detacher{strategies};
    common::ManualClock clock{events.empty() ? common::Timestamp{} : events.front().event.exchange_time};
    SynchronousDemoBus bus{clock, options.fault};
    strategy::StrategyEngine engine{bus, clock};

    // --- register ----------------------------------------------------------------
    std::unordered_map<std::string, std::size_t> slot_of;
    for (std::size_t i = 0; i < strategies.size(); ++i) {
        collectors.push_back(std::make_unique<SlotCollector>(&current_event));
        strategies[i].set_observer(collectors.back().get());
        try {
            engine.register_strategy(strategies[i].strategy);
        } catch (const common::ValidationError& error) {
            throw LabError(ErrorCode::ConfigError, error.what(),
                           {Problem{0, "", "strategies[" + std::to_string(i) + "]", error.what()}}, 1);
        }
        slot_of.emplace(strategies[i].strategy_id, i);
    }

    // --- the lab's own recorders, subscribed before the engine so they see each event first
    std::uint64_t market_sequence = 0;
    std::vector<SignalRecord> signals;
    bus.subscribe(events::EventType::MarketData,
                  [&market_sequence](const events::Event& event) { market_sequence = event.sequence; });
    bus.subscribe(events::EventType::Signal, [&](const events::Event& event) {
        if (const auto* signal = std::get_if<domain::TradeSignal>(&event.payload)) {
            signals.push_back(SignalRecord{*signal, current_event, event.sequence});
        }
    });

    // --- run ----------------------------------------------------------------------
    bus.start();
    engine.start();

    result.events.reserve(events.size());
    for (current_event = 0; current_event < events.size(); ++current_event) {
        const ReplayEvent& input = events[current_event];
        const std::size_t signals_before  = signals.size();
        const std::size_t failures_before = bus.failures().size();

        clock.set(input.event.exchange_time);   // before delivery
        events::Event envelope;
        envelope.type    = events::EventType::MarketData;
        envelope.payload = input.event;
        if (!bus.publish(std::move(envelope))) {
            throw LabError(ErrorCode::InternalError, "the demo bus refused a market data event");
        }

        EventResult row;
        row.index        = current_event;
        row.input        = input;
        row.bus_sequence = market_sequence;
        row.per_strategy.resize(strategies.size());
        for (std::size_t s = signals_before; s < signals.size(); ++s) {
            const auto slot = slot_of.find(signals[s].signal.strategy_id);
            if (slot == slot_of.end()) {
                throw LabError(ErrorCode::InternalError,
                               "a signal carried an unknown strategy id '" + signals[s].signal.strategy_id + "'");
            }
            row.per_strategy[slot->second].signal_ids.push_back(signals[s].signal.id.value);
        }
        for (std::size_t f = failures_before; f < bus.failures().size(); ++f) {
            const SynchronousDemoBus::FailedPublication& failure = bus.failures()[f];
            PublicationFailure record;
            if (const auto* signal = std::get_if<domain::TradeSignal>(&failure.event.payload)) {
                record.signal = *signal;
            }
            record.event_index = current_event;
            record.kind        = failure.kind == SynchronousDemoBus::FailedPublication::Kind::Rejected
                                     ? "rejected"
                                     : "publish_threw";
            record.message     = failure.message;
            result.failures.push_back(std::move(record));
        }
        result.events.push_back(std::move(row));
    }

    // --- stop: drain the bus first, then the engine (the order the engine documents) ---
    bus.request_shutdown();
    bus.wait_until_drained();
    engine.stop();

    // --- collect -------------------------------------------------------------------
    for (std::size_t s = 0; s < collectors.size(); ++s) {
        for (auto& [index, record] : collectors[s]->records) {
            if (index >= result.events.size() || result.events[index].per_strategy[s].diagnostics.has_value()) {
                throw LabError(ErrorCode::InternalError,
                               "strategy '" + strategies[s].strategy_id + "' reported more than once for event " +
                                   std::to_string(index));
            }
            result.events[index].per_strategy[s].diagnostics = std::move(record);
        }
    }

    const auto per_strategy = engine.strategy_stats();
    for (std::size_t s = 0; s < strategies.size(); ++s) {
        StrategyRunInfo info;
        info.kind              = strategies[s].kind;
        info.strategy_id       = strategies[s].strategy_id;
        info.parameters        = strategies[s].parameters;
        info.derived           = strategies[s].derived;
        info.symbols           = strategies[s].symbols;
        info.window_size       = strategies[s].window_size;
        info.stats             = per_strategy[s];
        info.observer_failures = strategies[s].observer_failures();
        result.strategies.push_back(std::move(info));
    }
    result.signals      = std::move(signals);
    result.engine_stats = engine.stats();

    const auto& counters = bus.counters();
    result.bus.market_data_published =
        counters.published_by_type[static_cast<std::size_t>(events::EventType::MarketData)];
    result.bus.signals_published = counters.published_by_type[static_cast<std::size_t>(events::EventType::Signal)];
    result.bus.signals_refused_by_fault = counters.refused_by_fault;
    result.bus.rejected_not_running     = counters.rejected_not_running;
    return result;
}

}  // namespace trading_engine::lab
