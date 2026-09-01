#pragma once

// -----------------------------------------------------------------------------
//  Clock abstraction.
//
//  Responsibility: decouple components from the wall clock so backtests are
//  deterministic and reproducible. Every component that needs "now" takes an
//  IClock& by dependency injection instead of calling std::chrono directly.
//
//  Lifecycle: a clock outlives every component that references it; the
//  composition root (TradingEngine) owns the concrete instance.
//
//  Thread-safety:
//   * SystemClock  -- thread-safe (stateless).
//   * ManualClock  -- NOT thread-safe; intended for single-threaded backtests
//                     and unit tests. TODO: add a synchronised variant if a
//                     replay ever drives multiple consumer threads.
// -----------------------------------------------------------------------------

#include "trading_engine/common/types.hpp"

namespace trading_engine::common {

class IClock {
public:
    virtual ~IClock();
    [[nodiscard]] virtual Timestamp now() const = 0;
};

// Real time. Use in live mode.
class SystemClock final : public IClock {
public:
    [[nodiscard]] Timestamp now() const override;
};

// Simulated time. Use in backtests / tests: time only moves when advanced.
class ManualClock final : public IClock {
public:
    ManualClock() = default;
    explicit ManualClock(Timestamp start) noexcept : current_{start} {}

    [[nodiscard]] Timestamp now() const override;

    void set(Timestamp t) noexcept;
    void advance(Duration d) noexcept;

private:
    Timestamp current_{};
};

}  // namespace trading_engine::common
