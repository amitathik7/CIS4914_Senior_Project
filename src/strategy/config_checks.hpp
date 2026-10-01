#pragma once

// -----------------------------------------------------------------------------
//  Configuration checks shared by the reference strategies. PRIVATE to src/strategy:
//  not under include/, not part of the public API. Every check throws
//  common::ConfigError whose message starts with the strategy's class name and
//  names the offending field, so two strategies reject the same mistake in the
//  same words. Nothing is rounded, clamped or defaulted on the caller's behalf.
// -----------------------------------------------------------------------------

#include <charconv>
#include <cmath>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "trading_engine/common/errors.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::strategy::detail {

// The shortest text that reads back as exactly `value`, whatever the locale.
inline std::string format_number(double value) {
    char buffer[32];   // the longest shortest-round-trip double is 24 characters
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (result.ec != std::errc{}) {
        return std::to_string(value);   // unreachable with this buffer; never lose a signal over it
    }
    return std::string(std::begin(buffer), result.ptr);
}

[[noreturn]] inline void reject(std::string_view strategy, const std::string& problem) {
    throw common::ConfigError(std::string{strategy} + ": " + problem);
}

// A finite, positive, whole number of shares. 1.5 is rejected, never rounded.
inline void require_whole_share_quantity(std::string_view strategy, double quantity) {
    if (!std::isfinite(quantity) || quantity <= 0.0 || std::floor(quantity) != quantity) {
        reject(strategy, "requested_quantity must be a finite, positive whole number of shares (got " +
                             format_number(quantity) + ")");
    }
}

// Non-empty, no empty entry, no symbol twice.
inline void require_symbol_allowlist(std::string_view strategy,
                                     const std::vector<common::Symbol>& symbols) {
    if (symbols.empty()) {
        reject(strategy, "symbols must name at least one symbol: the strategy trades only its allowlist");
    }
    std::unordered_set<std::string_view> seen;
    for (const common::Symbol& symbol : symbols) {
        if (symbol.empty()) {
            reject(strategy, "symbols must not contain an empty entry");
        }
        if (!seen.insert(symbol).second) {
            reject(strategy, "symbols lists '" + symbol + "' more than once");
        }
    }
}

}  // namespace trading_engine::strategy::detail
