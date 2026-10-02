#pragma once

// RFC 3339 UTC timestamps, parsed and printed without <chrono>'s parse support (not
// available in every standard library the project targets) and without any locale or
// time-zone database.
//
// Accepted: YYYY-MM-DDTHH:MM:SS[.f{1,9}]Z -- upper-case T and Z, a four-digit year in
// 1970..2261 (so the nanosecond count fits a signed 64-bit integer), a real calendar
// date, seconds 00..59 (no leap second). No UTC offset is accepted: "+00:00",
// "-05:00" and a missing zone are all rejected rather than interpreted.
// Printed: always with nine fractional digits, as in docs/adr/0002 section 3.

#include <optional>
#include <string>
#include <string_view>

#include "trading_engine/common/types.hpp"

namespace trading_engine::lab {

// On failure returns nullopt and, if `why` is given, says what is wrong.
[[nodiscard]] std::optional<common::Timestamp> parse_utc_timestamp(std::string_view text,
                                                                   std::string* why = nullptr);

[[nodiscard]] std::string format_utc_timestamp(common::Timestamp time);

}  // namespace trading_engine::lab
