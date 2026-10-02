#pragma once

// -----------------------------------------------------------------------------
//  The replay runner: the lab's composition root for ONE run.
//
//  Per run it builds, in this order, a fresh ManualClock, SynchronousDemoBus and
//  StrategyEngine, registers the already-built strategies (fresh objects per run: state
//  never carries over), attaches one diagnostics collector per strategy, subscribes the
//  lab's own recorders, and drives the engine through the bus lifecycle the engine
//  documents (the owner starts, shuts down and drains the bus; the engine never does):
//
//      bus.start(); engine.start();
//      for each event, in the order given:
//          clock.set(event.exchange_time);      // BEFORE delivery, so a signal's
//          bus.publish(MarketData event);       // created_at is the triggering bar's time
//      bus.request_shutdown(); bus.wait_until_drained(); engine.stop();
//
//  The runner reads no wall clock and holds no randomness. It does not sort or repair
//  its input (lab/dataset.hpp validates the CSV; an in-memory event list, as in tests,
//  is replayed exactly as given, bad prices and all, so the strategies' own handling of
//  malformed events can be observed).
//
//  What a result contains is only what the real engine, strategies and bus did: the
//  signals actually published (real ids, quantities, metadata, timestamps; ids count
//  from 1 per run), the engine's counters, publication failures if a bus fault was
//  injected, and one diagnostic record per strategy per event. No hold signal, fill,
//  position, P&L or confidence is ever created.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lab/catalog.hpp"
#include "lab/collector.hpp"
#include "lab/demo_bus.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

namespace trading_engine::lab {

struct ReplayEvent {
    domain::MarketEvent event{};
    std::size_t         source_line{0};   // 1-based CSV line, 0 when not from a file
};

struct ReplayOptions {
    SynchronousDemoBus::Fault fault{SynchronousDemoBus::Fault::None};
};

struct StrategyRunInfo {
    std::string kind{};
    std::string strategy_id{};
    std::vector<std::pair<std::string, ParamValue>>   parameters{};
    std::vector<std::pair<std::string, DerivedValue>> derived{};
    std::vector<std::string> symbols{};
    std::size_t              window_size{0};
    strategy::StrategyEngine::StrategyStats stats{};   // after the run
    std::uint64_t observer_failures{0};
};

struct StrategyEventResult {
    // Empty when the strategy reported nothing for this event (it threw, or the
    // collector failed): shown as unavailable, never guessed.
    std::optional<DiagnosticRecord> diagnostics{};
    std::vector<std::uint64_t>      signal_ids{};   // published signals this strategy emitted on this event
};

struct EventResult {
    std::size_t                      index{0};   // position in the replay
    ReplayEvent                      input{};
    std::uint64_t                    bus_sequence{0};
    std::vector<StrategyEventResult> per_strategy{};   // registration order
};

struct SignalRecord {
    domain::TradeSignal signal{};
    std::size_t         event_index{0};
    std::uint64_t       bus_sequence{0};
};

struct PublicationFailure {
    domain::TradeSignal signal{};   // as the engine stamped it; its id was consumed and is skipped
    std::size_t         event_index{0};
    std::string         kind{};     // "rejected" or "publish_threw"
    std::string         message{};
};

struct BusSummary {
    std::string   fault{"none"};
    std::uint64_t market_data_published{0};
    std::uint64_t signals_published{0};
    std::uint64_t signals_refused_by_fault{0};
    std::uint64_t rejected_not_running{0};
};

struct ReplayResult {
    std::vector<StrategyRunInfo>            strategies{};
    std::vector<EventResult>                events{};
    std::vector<SignalRecord>               signals{};      // publish order == signal id order
    std::vector<PublicationFailure>         failures{};
    strategy::StrategyEngine::Stats         engine_stats{};
    BusSummary                              bus{};
};

// Runs `strategies` over `events`. Throws LabError(ConfigError) if the engine refuses a
// registration (a duplicate strategy_id) and LabError(InternalError) if the lab's own
// bookkeeping is inconsistent. A strategy that throws, or a bus fault, is a RESULT, not
// an error: it is counted in the engine's statistics.
[[nodiscard]] ReplayResult run_replay(std::vector<BuiltStrategy> strategies,
                                      const std::vector<ReplayEvent>& events,
                                      const ReplayOptions& options = {});

[[nodiscard]] std::string_view fault_name(SynchronousDemoBus::Fault fault) noexcept;

}  // namespace trading_engine::lab
