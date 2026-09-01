#pragma once

// -----------------------------------------------------------------------------
//  PerformanceReport -- computed metrics for one run (backtest or live session).
//
//  Responsibility: the output of PerformanceAnalyzer and the payload the
//  Python visualization layer will eventually load from an export file.
//
//  Plain value type. The member initialisers below are structural defaults,
//  NOT results: PerformanceAnalyzer does not populate this yet and must not
//  hand back a zero-filled report as if it were real.
// -----------------------------------------------------------------------------

#include <cstdint>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::domain {

struct PerformanceReport {
    common::RunId     run_id{};
    common::Timestamp generated_at{};        // UTC
    common::Timestamp period_start{};
    common::Timestamp period_end{};

    double        total_return{0.0};         // fractional, e.g. 0.12 == +12%
    double        volatility{0.0};           // annualised std-dev of returns
    double        max_drawdown{0.0};         // most negative peak-to-trough, <= 0
    double        profit_factor{0.0};        // gross profit / gross loss
    std::uint64_t trade_count{0};

    // TODO: Sharpe, Sortino, Calmar, CAGR, win rate, average win / average
    //       loss, exposure %, turnover, per-strategy attribution, and a
    //       reference (id or path) to the equity-curve series used for charts.
};

}  // namespace trading_engine::domain
