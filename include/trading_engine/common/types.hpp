#pragma once

// -----------------------------------------------------------------------------
//  Shared primitive aliases for the core domain model.
//
//  Responsibility: give every component one vocabulary for time, money, price,
//  quantity and symbols, WITHOUT pulling in Alpaca, PostgreSQL or a JSON
//  library. Only the standard library is allowed here.
//
//  Thread-safety: these are plain value types; callers own synchronisation.
// -----------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace trading_engine::common {

// --- Time ------------------------------------------------------------------
// All timestamps are UTC. The engine never stores wall-clock local time.
// TODO: decide whether to move to std::chrono::utc_clock (leap-second aware)
//       once toolchain support is universal on the team's platforms.
using Clock     = std::chrono::system_clock;
using Timestamp = std::chrono::time_point<Clock, std::chrono::nanoseconds>;
using Duration  = std::chrono::nanoseconds;

[[nodiscard]] constexpr Timestamp epoch() noexcept { return Timestamp{}; }

// --- Money / price / quantity -------------------------------------------
// PROVISIONAL: double is a placeholder. Real money math needs a fixed-point or
// integer-minor-unit representation to avoid rounding drift. See
// docs/OPEN_QUESTIONS.md ("execution assumptions" / precision).
// TODO: replace with a Decimal type; keep the alias name stable so call sites
//       do not churn.
using Money    = double;   // account currency, whole units (e.g. USD)
using Price    = double;   // price per unit of the instrument
using Quantity = double;   // signed where direction matters (negative = short)

// --- Instrument identity -----------------------------------------------
// TODO: intern symbols to a small integer id for cache-friendly hot paths;
//       add exchange / asset-class qualifiers for multi-venue support.
using Symbol = std::string;

// Basis points helper used by fee / slippage configuration.
[[nodiscard]] constexpr double from_basis_points(double bps) noexcept {
    return bps / 10'000.0;
}

}  // namespace trading_engine::common
