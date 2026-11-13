#pragma once

// -----------------------------------------------------------------------------
//  Results export -- one finished run as a JSON file.
//
//  `trading_engine run --report FILE` writes it and
//  python/visualization/plot_performance.py charts it. Schema
//  "trading_engine.results/1":
//
//    { "schema", "name", "mode", "source", "symbols": [..],
//      "summary": { PerformanceReport fields; money as numbers, times as
//                   ISO-8601 UTC, unset ratios as null },
//      "equity_curve": [ { "time", "equity", "cash" } ],   // starting point first
//      "fills": [ { "time", "order_id", "symbol", "quantity", "price", "fees" } ],
//      "stages": [ { "stage", "events", "errors", "p50_ns", "p99_ns", "max_ns" } ] }
//
//  quantity is signed (+buy / -sell). "stages" lists only stages that ran.
// -----------------------------------------------------------------------------

#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "trading_engine/analytics/system_metrics.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/performance_report.hpp"
#include "trading_engine/domain/portfolio_snapshot.hpp"

namespace trading_engine::analytics {

inline constexpr const char* kResultsSchema = "trading_engine.results/1";

struct RunResults {
    std::string                            name;
    std::string                            mode;     // backtest | live
    std::string                            source;   // the bar file, or the live feed
    std::vector<common::Symbol>            symbols;
    domain::PerformanceReport              report;
    std::vector<domain::PortfolioSnapshot> equity_curve;
    std::vector<domain::Fill>              fills;
    std::optional<SystemMetrics::Snapshot> metrics;
};

void write_results_json(std::ostream& out, const RunResults& results);

}  // namespace trading_engine::analytics
