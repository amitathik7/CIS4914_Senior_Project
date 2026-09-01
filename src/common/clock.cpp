#include "trading_engine/common/clock.hpp"

#include <chrono>

namespace trading_engine::common {

IClock::~IClock() = default;

Timestamp SystemClock::now() const {
    return std::chrono::time_point_cast<Timestamp::duration>(Clock::now());
}

Timestamp ManualClock::now() const {
    return current_;
}

void ManualClock::set(Timestamp t) noexcept {
    current_ = t;
}

void ManualClock::advance(Duration d) noexcept {
    current_ += d;
}

}  // namespace trading_engine::common
