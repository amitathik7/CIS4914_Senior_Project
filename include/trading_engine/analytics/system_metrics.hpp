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

enum class Stage : std::uint8_t {
    Ingest = 0,
    Strategy,
    Risk,
    Execution,
    Portfolio,
    Persistence
};

inline constexpr std::size_t kStageCount = 6;

// converts a pipeline stage to a readable name
[[nodiscard]] std::string_view to_string(Stage stage) noexcept;

// Fixed-size log-linear latency histogram: values below 128 ns are exact and
// larger values land in buckets at most 1/64 (~1.6%) of their value wide, so
// memory stays constant however many samples are recorded.
class LatencyHistogram {
public:
    static constexpr std::size_t kSubBuckets = 64;
    static constexpr std::size_t kExactLimit = 2 * kSubBuckets;
    static constexpr std::size_t kBucketCount = kExactLimit + (64 - 7) * kSubBuckets;

    void record(common::Duration elapsed) noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] common::Duration min() const noexcept;
    [[nodiscard]] common::Duration max() const noexcept;

    // nearest-rank percentile, reported as its bucket's lower bound clamped to [min, max]
    [[nodiscard]] common::Duration percentile(std::size_t percent) const noexcept;

    [[nodiscard]] static std::size_t bucket_index(std::uint64_t nanos) noexcept;
    [[nodiscard]] static std::uint64_t bucket_lower_bound(std::size_t index) noexcept;

private:
    std::array<std::uint64_t, kBucketCount> buckets_{};
    std::uint64_t count_{0};
    std::uint64_t min_{UINT64_MAX};
    std::uint64_t max_{0};
};

class SystemMetrics {
public:
    SystemMetrics();
    ~SystemMetrics();

    // records one latency measurement for a pipeline stage
    void record_latency(Stage stage, common::Duration elapsed);

    // stores the current depth of a named queue
    void set_queue_depth(std::string_view queue_name, std::size_t depth);

    // adds processed events to a stage's event count
    void increment_events(Stage stage, std::uint64_t n = 1);

    // adds errors to a stage's error count
    void increment_errors(Stage stage, std::uint64_t n = 1);

    struct LatencyStats {
        std::uint64_t count{0};
        common::Duration min{};
        common::Duration max{};
        common::Duration p50{};
        common::Duration p99{};
    };

    struct StageStats {
        Stage stage{Stage::Ingest};
        std::uint64_t events{0};
        std::uint64_t errors{0};
        LatencyStats latency{};
    };

    struct QueueDepth {
        std::string name{};
        std::size_t depth{0};
    };

    struct Snapshot {
        std::uint64_t total_events{0};
        std::uint64_t total_errors{0};
        std::array<StageStats, kStageCount> stages{};
        std::vector<QueueDepth> queue_depths{};
    };

    // returns a thread safe snapshot of all currently recorded metrics
    [[nodiscard]] Snapshot snapshot() const;

private:
    // converts a stage enum into the matching array index
    [[nodiscard]] static std::size_t stage_index(Stage stage) noexcept;

    mutable std::mutex mutex_;

    std::array<std::uint64_t, kStageCount> events_{};
    std::array<std::uint64_t, kStageCount> errors_{};
    std::array<LatencyHistogram, kStageCount> latencies_{};
    std::unordered_map<std::string, std::size_t> queue_depths_{};
};

} // namespace trading_engine::analytics
