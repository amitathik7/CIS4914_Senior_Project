#include "trading_engine/common/clock.hpp"

#include <chrono>

namespace trading_engine::common {

IClock::~IClock() = default;

Timestamp SystemClock::now() const {
    return std::chrono::time_point_cast<Timestamp::duration>(Clock::now());
}

Timestamp ManualClock::now() const {
    return current_.load();
}

void ManualClock::set(Timestamp t) noexcept {
    current_.store(t);
}

void ManualClock::advance(Duration d) noexcept {
    Timestamp expected = current_.load();
    while (!current_.compare_exchange_weak(expected, expected + d)) {
    }
}

}  // namespace trading_engine::common
