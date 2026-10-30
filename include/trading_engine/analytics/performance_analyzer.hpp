#pragma once

// -----------------------------------------------------------------------------
//  PerformanceAnalyzer -- computes run metrics from recorded state.
//
//  Responsibility: given the time series of PortfolioSnapshots and the list of
//  Fills for a run, compute a domain::PerformanceReport (total return,
//  volatility, max drawdown, profit factor, trade count, and more later).
//  Pure computation: no I/O, no market-data access.
//
//  analyze() needs at least one snapshot (the first is the starting point)
//  and throws common::ValidationError for an empty curve rather than return
//  a zero-filled report. Fills must be in execution order. Metric definitions
//  are in domain/performance_report.hpp.
//
//  Ownership / lifecycle: cheap, stateless; construct on demand. Thread-safe
//  (nothing mutable).
// -----------------------------------------------------------------------------

#include <span>

#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/performance_report.hpp"
#include "trading_engine/domain/portfolio_snapshot.hpp"

namespace trading_engine::analytics {

struct AnalysisOptions {
    double   risk_free_rate{0.0};        // annualised, for Sharpe etc.
    unsigned periods_per_year{252};      // trading days; 252*390 for minute bars
    // TODO: return sampling frequency, benchmark series, drawdown definition
    //       (return-based vs equity-based), currency.
};

class PerformanceAnalyzer {
public:
    explicit PerformanceAnalyzer(AnalysisOptions options = {});

    [[nodiscard]] domain::PerformanceReport analyze(
        std::span<const domain::PortfolioSnapshot> equity_curve,
        std::span<const domain::Fill> fills) const;

private:
    AnalysisOptions options_;

    // TODO: helpers for return series construction, drawdown curve, rolling
    //       volatility, trade round-trip pairing for profit factor / win rate.
};

}  // namespace trading_engine::analytics
