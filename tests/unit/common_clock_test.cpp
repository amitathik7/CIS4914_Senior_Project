// Real behaviour the scaffold ships: the clock abstraction used to make
// backtests deterministic.

#include <chrono>

#include <gtest/gtest.h>

#include "trading_engine/common/clock.hpp"

namespace te = trading_engine::common;
using namespace std::chrono_literals;

TEST(ManualClock, StartsAtEpochByDefault) {
    const te::ManualClock clock;
    EXPECT_EQ(clock.now(), te::Timestamp{});
}

TEST(ManualClock, ConstructibleAtAGivenStart) {
    const te::Timestamp start = te::Timestamp{} + 10s;
    const te::ManualClock clock{start};
    EXPECT_EQ(clock.now(), start);
}

TEST(ManualClock, AdvanceMovesTimeForwardByExactlyTheDuration) {
    te::ManualClock clock;
    const te::Timestamp before = clock.now();
    clock.advance(1500ms);
    EXPECT_EQ(clock.now() - before, te::Duration{1500ms});
}

TEST(ManualClock, SetReplacesCurrentTime) {
    te::ManualClock clock;
    const te::Timestamp target = te::Timestamp{} + 42s;
    clock.set(target);
    EXPECT_EQ(clock.now(), target);
}

TEST(SystemClock, NowIsAfterTheUnixEpoch) {
    const te::SystemClock clock;
    EXPECT_GT(clock.now().time_since_epoch().count(), 0);
}
