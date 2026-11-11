#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "trading_engine/common/types.hpp"

namespace trading_engine::analytics {

using namespace std;

enum class Stage : uint8_t {
    Ingest = 0,
    Strategy,
    Risk,
    Execution,
    Portfolio,
    Persistence
};

inline constexpr size_t kStageCount = 6;

// converts a pipeline stage to a readable name
[[nodiscard]] string_view to_string(Stage stage) noexcept;

class SystemMetrics {
public:
    SystemMetrics();
    ~SystemMetrics();

    // records one latency measurement for a pipeline stage
    void record_latency(Stage stage, common::Duration elapsed);

    // stores the current depth of a named queue
    void set_queue_depth(string_view queue_name, size_t depth);

    // adds processed events to a stage's event count
    void increment_events(Stage stage, uint64_t n = 1);

    // adds errors to a stage's error count
    void increment_errors(Stage stage, uint64_t n = 1);

    struct LatencyStats {
        uint64_t count{0};
        common::Duration min{};
        common::Duration max{};
        common::Duration p50{};
        common::Duration p99{};
    };

    struct StageStats {
        Stage stage{Stage::Ingest};
        uint64_t events{0};
        uint64_t errors{0};
        LatencyStats latency{};
    };

    struct QueueDepth {
        string name{};
        size_t depth{0};
    };

    struct Snapshot {
        uint64_t total_events{0};
        uint64_t total_errors{0};
        array<StageStats, kStageCount> stages{};
        vector<QueueDepth> queue_depths{};
    };

    // returns a thread safe snapshot of all currently recorded metrics
    [[nodiscard]] Snapshot snapshot() const;

private:
    // converts a stage enum into the matching array index
    [[nodiscard]] static size_t stage_index(Stage stage) noexcept;

    // calculates min, max, median, and p99 for latency samples
    [[nodiscard]] static LatencyStats summarize_latency(
        vector<common::Duration> samples);

    mutable mutex mutex_;

    array<uint64_t, kStageCount> events_{};
    array<uint64_t, kStageCount> errors_{};
    array<vector<common::Duration>, kStageCount> latencies_{};
    unordered_map<string, size_t> queue_depths_{};
};

} // namespace trading_engine::analytics