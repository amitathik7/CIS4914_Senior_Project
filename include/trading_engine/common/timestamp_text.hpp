#pragma once

// RFC 3339 UTC text <-> common::Timestamp, exact to the nanosecond.
//
// format_timestamp() always writes nine fractional digits and a 'Z':
//   2026-01-05T14:30:00.000000000Z
// parse_timestamp() accepts "YYYY-MM-DDTHH:MM:SS", an optional '.' followed by
// one to nine digits, and a mandatory 'Z'. Offsets other than Z, lowercase
// 't'/'z', leap seconds and out-of-range fields are refused: a timestamp that
// crosses a process boundary must mean exactly one instant.

#include <optional>
#include <string>
#include <string_view>

#include "trading_engine/common/types.hpp"

namespace trading_engine::common {

[[nodiscard]] std::string format_timestamp(Timestamp t);

// nullopt for anything that is not exactly the form above.
[[nodiscard]] std::optional<Timestamp> parse_timestamp(std::string_view text) noexcept;

}  // namespace trading_engine::common
