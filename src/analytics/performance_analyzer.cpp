#include "trading_engine/analytics/performance_analyzer.hpp"

#include "trading_engine/common/errors.hpp"

namespace trading_engine::analytics {

PerformanceAnalyzer::PerformanceAnalyzer(AnalysisOptions options)
    : options_{options} {}

domain::PerformanceReport PerformanceAnalyzer::analyze(
    std::span<const domain::PortfolioSnapshot> /*equity_curve*/,
    std::span<const domain::Fill> /*fills*/) const {
    // TODO: build a return series from the equity curve, then compute
    //       total_return, annualised volatility (using options_.periods_per_year
    //       and options_.risk_free_rate), max drawdown, profit factor and trade
    //       count. Must not return a zero-filled report as a stand-in.
    throw common::NotImplemented("PerformanceAnalyzer::analyze");
}

}  // namespace trading_engine::analytics
