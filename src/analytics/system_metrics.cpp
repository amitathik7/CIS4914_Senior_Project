#include "trading_engine/analytics/system_metrics.hpp"

#include <algorithm>
#include <array>
#include <bit>

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

// log2 of the sub-bucket count: each power-of-two range above the exact
// region is split into this many equal-width buckets
constexpr unsigned kSubBucketBits = 6;
static_assert(LatencyHistogram::kSubBuckets == (size_t{1} << kSubBucketBits));

// finds the 1-based nearest rank for a requested percentile
uint64_t percentile_rank(uint64_t count, size_t percent) {
    const uint64_t rank = (percent * count + 99) / 100;
    return max<uint64_t>(rank, 1);
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

// --- LatencyHistogram -------------------------------------------------------

size_t LatencyHistogram::bucket_index(uint64_t nanos) noexcept {
    if (nanos < kExactLimit) {
        return static_cast<size_t>(nanos);
    }

    const unsigned exponent = static_cast<unsigned>(bit_width(nanos)) - 1;
    const unsigned shift = exponent - kSubBucketBits;
    const size_t sub = static_cast<size_t>(nanos >> shift) - kSubBuckets;

    return kExactLimit + (exponent - kSubBucketBits - 1) * kSubBuckets + sub;
}

uint64_t LatencyHistogram::bucket_lower_bound(size_t index) noexcept {
    if (index < kExactLimit) {
        return index;
    }

    const size_t offset = index - kExactLimit;
    const unsigned shift = static_cast<unsigned>(offset / kSubBuckets) + 1;
    const uint64_t sub = offset % kSubBuckets;

    return (kSubBuckets + sub) << shift;
}

void LatencyHistogram::record(common::Duration elapsed) noexcept {
    const uint64_t nanos =
        elapsed.count() < 0 ? 0 : static_cast<uint64_t>(elapsed.count());

    ++buckets_[bucket_index(nanos)];
    ++count_;
    min_ = std::min(min_, nanos);
    max_ = std::max(max_, nanos);
}

common::Duration LatencyHistogram::min() const noexcept {
    return count_ == 0 ? common::Duration{} : common::Duration{static_cast<int64_t>(min_)};
}

common::Duration LatencyHistogram::max() const noexcept {
    return common::Duration{static_cast<int64_t>(max_)};
}

common::Duration LatencyHistogram::percentile(size_t percent) const noexcept {
    if (count_ == 0) {
        return common::Duration{};
    }

    const uint64_t rank = percentile_rank(count_, percent);
    uint64_t seen = 0;

    for (size_t i = 0; i < kBucketCount; ++i) {
        seen += buckets_[i];

        if (seen >= rank) {
            const uint64_t value = clamp(bucket_lower_bound(i), min_, max_);
            return common::Duration{static_cast<int64_t>(value)};
        }
    }

    return max();
}

// --- SystemMetrics ----------------------------------------------------------

SystemMetrics::SystemMetrics() = default;

SystemMetrics::~SystemMetrics() = default;

// converts a stage into its matching array position
size_t SystemMetrics::stage_index(Stage stage) noexcept {
    return static_cast<size_t>(stage);
}

// records a latency sample for the requested stage
void SystemMetrics::record_latency(Stage stage, common::Duration elapsed) {
    const lock_guard lock{mutex_};

    latencies_[stage_index(stage)].record(elapsed);
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

// builds a consistent snapshot without changing the stored metrics
SystemMetrics::Snapshot SystemMetrics::snapshot() const {
    const lock_guard lock{mutex_};

    Snapshot result;

    for (size_t i = 0; i < kStageCount; ++i) {
        const LatencyHistogram& histogram = latencies_[i];

        StageStats stats;

        stats.stage = kStages[i];
        stats.events = events_[i];
        stats.errors = errors_[i];
        stats.latency = LatencyStats{
            .count = histogram.count(),
            .min = histogram.min(),
            .max = histogram.max(),
            .p50 = histogram.percentile(50),
            .p99 = histogram.percentile(99)
        };

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
