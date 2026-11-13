#pragma once

// Reads market data for the pipeline CLI from a CSV with a header row, the
// same layout the Strategy Lab fixtures use (tests/fixtures/strategy_lab/):
//
//   symbol,exchange_time,type,price[,open,high,low,volume]
//   AAPL,2026-01-05T14:30:00Z,bar,101.25,100.5,102,100.25,15000
//
// Required columns: symbol, exchange_time (RFC 3339 UTC), price. Optional:
// type (bar | trade | quote; default bar), open, high, low, volume. Columns
// may appear in any order; unknown columns are ignored. Plain comma
// separation, no quoting. Rows are returned in file order and are NOT
// validated beyond parsing: that is MarketDataService's job.

#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "trading_engine/domain/market_event.hpp"

namespace trading_engine::cli {

class CsvError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Throws CsvError naming the line and column of the first problem.
[[nodiscard]] std::vector<domain::MarketEvent> read_bar_csv(const std::string& path);

// Writes the full layout above (all eight columns), one row per event, in the
// given order; read_bar_csv() reads it back unchanged.
void write_bar_csv(std::ostream& out, const std::vector<domain::MarketEvent>& events);

// One row of that layout, for writing events as they arrive (after a
// write_bar_csv(out, {}) for the header).
void write_bar_csv_row(std::ostream& out, const domain::MarketEvent& event);

}  // namespace trading_engine::cli
