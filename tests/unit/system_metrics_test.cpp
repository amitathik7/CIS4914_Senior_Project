#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

#include "trading_engine/analytics/system_metrics.hpp"

namespace {

using namespace std;
using namespace std::chrono;

namespace an = trading_engine::analytics;

// check that each stage has the expected readable name
TEST(SystemMetrics, StageNamesAreStable) {
    EXPECT_EQ(an::to_string(an::Stage::Ingest), "ingest");
    EXPECT_EQ(an::to_string(an::Stage::Strategy), "strategy");
    EXPECT_EQ(an::to_string(an::Stage::Risk), "risk");
    EXPECT_EQ(an::to_string(an::Stage::Execution), "execution");
    EXPECT_EQ(an::to_string(an::Stage::Portfolio), "portfolio");
    EXPECT_EQ(an::to_string(an::Stage::Persistence), "persistence");
}

// check event and error totals as well as per-stage counts
TEST(SystemMetrics, CountsEventsAndErrorsPerStage) {
    an::SystemMetrics metrics;

    metrics.increment_events(an::Stage::Ingest, 3);
    metrics.increment_events(an::Stage::Strategy, 2);
    metrics.increment_events(an::Stage::Risk);

    metrics.increment_errors(an::Stage::Strategy);
    metrics.increment_errors(an::Stage::Risk, 2);

    const auto snapshot = metrics.snapshot();

    EXPECT_EQ(snapshot.total_events, 6u);
    EXPECT_EQ(snapshot.total_errors, 3u);

    const auto ingest = static_cast<size_t>(an::Stage::Ingest);
    const auto strategy = static_cast<size_t>(an::Stage::Strategy);
    const auto risk = static_cast<size_t>(an::Stage::Risk);

    EXPECT_EQ(snapshot.stages[ingest].events, 3u);
    EXPECT_EQ(snapshot.stages[ingest].errors, 0u);

    EXPECT_EQ(snapshot.stages[strategy].events, 2u);
    EXPECT_EQ(snapshot.stages[strategy].errors, 1u);

    EXPECT_EQ(snapshot.stages[risk].events, 1u);
    EXPECT_EQ(snapshot.stages[risk].errors, 2u);
}

// check that latency statistics are calculated correctly
TEST(SystemMetrics, RecordsLatencyStatistics) {
    an::SystemMetrics metrics;

    metrics.record_latency(an::Stage::Strategy, nanoseconds{100});
    metrics.record_latency(an::Stage::Strategy, nanoseconds{20});
    metrics.record_latency(an::Stage::Strategy, nanoseconds{40});
    metrics.record_latency(an::Stage::Strategy, nanoseconds{10});
    metrics.record_latency(an::Stage::Strategy, nanoseconds{30});

    const auto snapshot = metrics.snapshot();

    const auto strategy = static_cast<size_t>(an::Stage::Strategy);
    const auto& latency = snapshot.stages[strategy].latency;

    EXPECT_EQ(latency.count, 5u);
    EXPECT_EQ(latency.min, nanoseconds{10});
    EXPECT_EQ(latency.max, nanoseconds{100});
    EXPECT_EQ(latency.p50, nanoseconds{30});
    EXPECT_EQ(latency.p99, nanoseconds{100});
}

TEST(SystemMetrics, EndToEndFallsIntoItsHistogramBuckets) {
    an::SystemMetrics metrics;

    metrics.record_end_to_end(microseconds{50});        // <= 100 us
    metrics.record_end_to_end(microseconds{100});       // <= 100 us, on the bound
    metrics.record_end_to_end(microseconds{700});       // <= 1000 us
    metrics.record_end_to_end(seconds{5});              // slower than the last bound: still counted in it

    const auto snapshot = metrics.snapshot();
    const auto& buckets = snapshot.end_to_end_buckets;
    const auto& bounds = an::SystemMetrics::kEndToEndBoundsUs;

    EXPECT_EQ(snapshot.end_to_end.count, 4u);
    EXPECT_EQ(snapshot.end_to_end.min, microseconds{50});
    EXPECT_EQ(snapshot.end_to_end.max, seconds{5});
    EXPECT_EQ(buckets[0], 2u);
    EXPECT_EQ(buckets[static_cast<size_t>(std::find(bounds.begin(), bounds.end(), 1'000u) - bounds.begin())], 1u);
    EXPECT_EQ(buckets.back(), 1u);
    EXPECT_EQ(std::accumulate(buckets.begin(), buckets.end(), uint64_t{0}), 4u);
}

// check that a stage with no latency data returns zero values
TEST(SystemMetrics, EmptyLatencyStatsRemainZero) {
    an::SystemMetrics metrics;

    const auto snapshot = metrics.snapshot();

    const auto execution = static_cast<size_t>(an::Stage::Execution);
    const auto& latency = snapshot.stages[execution].latency;

    EXPECT_EQ(latency.count, 0u);
    EXPECT_EQ(latency.min, nanoseconds{0});
    EXPECT_EQ(latency.max, nanoseconds{0});
    EXPECT_EQ(latency.p50, nanoseconds{0});
    EXPECT_EQ(latency.p99, nanoseconds{0});
}

// check that queue depths can be added and updated
TEST(SystemMetrics, TracksAndUpdatesQueueDepths) {
    an::SystemMetrics metrics;

    metrics.set_queue_depth("event_bus", 4);
    metrics.set_queue_depth("kafka", 7);
    metrics.set_queue_depth("event_bus", 2);

    const auto snapshot = metrics.snapshot();

    ASSERT_EQ(snapshot.queue_depths.size(), 2u);

    EXPECT_EQ(snapshot.queue_depths[0].name, "event_bus");
    EXPECT_EQ(snapshot.queue_depths[0].depth, 2u);

    EXPECT_EQ(snapshot.queue_depths[1].name, "kafka");
    EXPECT_EQ(snapshot.queue_depths[1].depth, 7u);
}

// check that multiple threads can record data safely
TEST(SystemMetrics, RecordingIsThreadSafe) {
    an::SystemMetrics metrics;

    constexpr int thread_count = 8;
    constexpr int iterations = 1000;

    vector<thread> workers;

    for (int t = 0; t < thread_count; ++t) {
        workers.emplace_back([&metrics] {
            for (int i = 0; i < iterations; ++i) {
                metrics.increment_events(an::Stage::Strategy);

                metrics.record_latency(
                    an::Stage::Strategy,
                    nanoseconds{i % 100});

                if (i % 100 == 0) {
                    metrics.increment_errors(an::Stage::Strategy);
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    const auto snapshot = metrics.snapshot();

    const auto strategy = static_cast<size_t>(an::Stage::Strategy);
    const auto& stats = snapshot.stages[strategy];

    EXPECT_EQ(
        stats.events,
        static_cast<uint64_t>(thread_count * iterations));

    EXPECT_EQ(
        stats.errors,
        static_cast<uint64_t>(thread_count * (iterations / 100)));

    EXPECT_EQ(
        stats.latency.count,
        static_cast<uint64_t>(thread_count * iterations));

    EXPECT_EQ(stats.latency.min, nanoseconds{0});
    EXPECT_EQ(stats.latency.max, nanoseconds{99});
}

// check that taking a snapshot does not clear existing data
TEST(SystemMetrics, SnapshotDoesNotResetRecordedData) {
    an::SystemMetrics metrics;

    metrics.increment_events(an::Stage::Portfolio, 5);
    metrics.increment_errors(an::Stage::Portfolio);
    metrics.record_latency(an::Stage::Portfolio, microseconds{10});

    const auto first = metrics.snapshot();
    const auto second = metrics.snapshot();

    EXPECT_EQ(first.total_events, second.total_events);
    EXPECT_EQ(first.total_errors, second.total_errors);

    const auto portfolio = static_cast<size_t>(an::Stage::Portfolio);

    EXPECT_EQ(
        first.stages[portfolio].latency.count,
        second.stages[portfolio].latency.count);

    EXPECT_EQ(
        first.stages[portfolio].latency.p50,
        second.stages[portfolio].latency.p50);
}

// check that every value maps to a bucket whose lower bound is within 1/64 of it
TEST(LatencyHistogram, BucketsStayWithinTheRelativeErrorBound) {
    using H = an::LatencyHistogram;

    for (uint64_t v = 0; v < H::kExactLimit; ++v) {
        EXPECT_EQ(H::bucket_lower_bound(H::bucket_index(v)), v);
    }

    for (unsigned exponent = 7; exponent < 64; ++exponent) {
        const uint64_t base = uint64_t{1} << exponent;

        for (const uint64_t v : {base, base + base / 3, base + (base - 1)}) {
            const size_t index = H::bucket_index(v);
            const uint64_t lower = H::bucket_lower_bound(index);

            ASSERT_LT(index, H::kBucketCount);
            EXPECT_LE(lower, v);
            EXPECT_LE(v - lower, v / H::kSubBuckets);
        }
    }

    EXPECT_EQ(H::bucket_index(UINT64_MAX), H::kBucketCount - 1);
}

// check that percentiles over many samples land within the bucket error
TEST(LatencyHistogram, PercentilesOverManySamplesAreAccurate) {
    an::LatencyHistogram histogram;

    for (int64_t i = 1; i <= 1'000'000; ++i) {
        histogram.record(nanoseconds{i});
    }

    EXPECT_EQ(histogram.count(), 1'000'000u);
    EXPECT_EQ(histogram.min(), nanoseconds{1});
    EXPECT_EQ(histogram.max(), nanoseconds{1'000'000});

    const auto p50 = histogram.percentile(50).count();
    const auto p99 = histogram.percentile(99).count();

    EXPECT_NEAR(static_cast<double>(p50), 500'000.0, 500'000.0 / 64);
    EXPECT_NEAR(static_cast<double>(p99), 990'000.0, 990'000.0 / 64);
}

// check that negative durations count as zero instead of corrupting the histogram
TEST(LatencyHistogram, NegativeDurationsRecordAsZero) {
    an::LatencyHistogram histogram;

    histogram.record(nanoseconds{-5});

    EXPECT_EQ(histogram.count(), 1u);
    EXPECT_EQ(histogram.min(), nanoseconds{0});
    EXPECT_EQ(histogram.percentile(99), nanoseconds{0});
}

} // namespace
