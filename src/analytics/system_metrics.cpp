#include "trading_engine/analytics/system_metrics.hpp"

#include <algorithm>
#include <array>

namespace trading_engine::analytics {

using namespace std;

namespace {

constexpr array<Stage, kStageCount> kStages{
    Stage::Ingest,
    Stage::Strategy,
    Stage::Risk,
    Stage::Execution,
    Stage::Portfolio,
    Stage::Persistence
};

// finds the nearest rank index for a requested percentile
size_t percentile_index(size_t count, size_t percentile) {
    if (count == 0) {
        return 0;
    }

    const size_t rank = (percentile * count + 99) / 100;
    return rank == 0 ? 0 : rank - 1;
}

} // namespace

// converts a pipeline stage to a readable string
string_view to_string(Stage stage) noexcept {
    switch (stage) {
        case Stage::Ingest:
            return "ingest";
        case Stage::Strategy:
            return "strategy";
        case Stage::Risk:
            return "risk";
        case Stage::Execution:
            return "execution";
        case Stage::Portfolio:
            return "portfolio";
        case Stage::Persistence:
            return "persistence";
    }

    return "invalid";
}

SystemMetrics::SystemMetrics() = default;

SystemMetrics::~SystemMetrics() = default;

// converts a stage into its matching array position
size_t SystemMetrics::stage_index(Stage stage) noexcept {
    return static_cast<size_t>(stage);
}

// records a latency sample for the requested stage
void SystemMetrics::record_latency(Stage stage, common::Duration elapsed) {
    const lock_guard lock{mutex_};

    latencies_[stage_index(stage)].push_back(elapsed);
}

// updates the current depth of a named queue
void SystemMetrics::set_queue_depth(string_view queue_name, size_t depth) {
    const lock_guard lock{mutex_};

    queue_depths_[string{queue_name}] = depth;
}

// increments the number of processed events for a stage
void SystemMetrics::increment_events(Stage stage, uint64_t n) {
    const lock_guard lock{mutex_};

    events_[stage_index(stage)] += n;
}

// increments the number of errors recorded for a stage
void SystemMetrics::increment_errors(Stage stage, uint64_t n) {
    const lock_guard lock{mutex_};

    errors_[stage_index(stage)] += n;
}

// calculates basic latency statistics from a copy of the samples
SystemMetrics::LatencyStats SystemMetrics::summarize_latency(
    vector<common::Duration> samples) {

    LatencyStats stats;

    if (samples.empty()) {
        return stats;
    }

    sort(samples.begin(), samples.end());

    stats.count = static_cast<uint64_t>(samples.size());
    stats.min = samples.front();
    stats.max = samples.back();
    stats.p50 = samples[percentile_index(samples.size(), 50)];
    stats.p99 = samples[percentile_index(samples.size(), 99)];

    return stats;
}

// builds a consistent snapshot without changing the stored metrics
SystemMetrics::Snapshot SystemMetrics::snapshot() const {
    const lock_guard lock{mutex_};

    Snapshot result;

    for (size_t i = 0; i < kStageCount; ++i) {
        StageStats stats;

        stats.stage = kStages[i];
        stats.events = events_[i];
        stats.errors = errors_[i];
        stats.latency = summarize_latency(latencies_[i]);

        result.stages[i] = stats;

        result.total_events += stats.events;
        result.total_errors += stats.errors;
    }

    result.queue_depths.reserve(queue_depths_.size());

    for (const auto& [name, depth] : queue_depths_) {
        result.queue_depths.push_back(QueueDepth{
            .name = name,
            .depth = depth
        });
    }

    sort(
        result.queue_depths.begin(),
        result.queue_depths.end(),
        [](const QueueDepth& left, const QueueDepth& right) {
            return left.name < right.name;
        });

    return result;
}

} // namespace trading_engine::analytics