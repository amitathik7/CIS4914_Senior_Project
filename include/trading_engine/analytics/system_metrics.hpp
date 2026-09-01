#pragma once

// -----------------------------------------------------------------------------
//  SystemMetrics -- engineering telemetry for the pipeline itself.
//
//  Responsibility: record how the ENGINE is performing (not the strategy):
//  per-stage processing latency, queue depth, event throughput, and error
//  counts. This is the data the project's latency/throughput goals are judged
//  against.
//
//  STATUS: declaration only. Recording methods and snapshot() throw
//  common::NotImplemented so no fabricated metrics are ever reported.
//
//  Ownership / lifecycle: one instance owned by TradingEngine, shared by
//  reference with every stage.
//
//  Thread-safety: the future implementation MUST be safe for concurrent
//  recording from every pipeline thread (lock-free counters / per-thread
//  accumulation). The scaffold provides no implementation.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "trading_engine/common/types.hpp"

namespace trading_engine::analytics {

// Pipeline stages we want to time independently.
enum class Stage : std::uint8_t {
    Ingest = 0,     // source -> MarketDataService
    Strategy,       // StrategyEngine dispatch
    Risk,           // RiskManager evaluation
    Execution,      // ExecutionSimulator + FillModel
    Portfolio,      // PortfolioManager update
    Persistence     // repository write
};

[[nodiscard]] std::string_view to_string(Stage) noexcept;

class SystemMetrics {
public:
    SystemMetrics();
    ~SystemMetrics();

    void record_latency(Stage stage, common::Duration elapsed);   // NOT IMPLEMENTED
    void set_queue_depth(std::string_view queue_name, std::size_t depth);  // NOT IMPLEMENTED
    void increment_events(Stage stage, std::uint64_t n = 1);      // NOT IMPLEMENTED
    void increment_errors(Stage stage, std::uint64_t n = 1);      // NOT IMPLEMENTED

    struct LatencyStats {
        std::uint64_t     count{0};
        common::Duration  min{};
        common::Duration  max{};
        common::Duration  p50{};
        common::Duration  p99{};
    };

    struct Snapshot {
        std::uint64_t total_events{0};
        std::uint64_t total_errors{0};
        // TODO: per-stage LatencyStats, per-queue depth gauges, throughput
        //       (events/sec) over a rolling window, uptime.
    };

    [[nodiscard]] Snapshot snapshot() const;   // NOT IMPLEMENTED

private:
    // TODO: HDR-histogram or fixed-bucket latency accumulators per Stage,
    //       atomic counters, and an export hook (Prometheus text / JSON).
};

}  // namespace trading_engine::analytics
