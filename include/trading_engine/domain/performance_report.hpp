#pragma once

// -----------------------------------------------------------------------------
//  PerformanceReport -- computed metrics for one run (backtest or live session).
//
//  Responsibility: the output of PerformanceAnalyzer and the payload the
//  Python visualization layer and the web console load from an export.
//
//  Plain value type. The member initialisers below are structural defaults,
//  NOT results: only PerformanceAnalyzer::analyze() fills one in.
//
//  Definitions (docs/BACKTESTING.md section 4):
//   * returns are per equity-curve sample: r_i = E_i / E_{i-1} - 1;
//   * a "trade" is one closing execution (a sell that reduces a long, a buy
//     that reduces a short); its P&L is (fill price - average cost) * closed
//     shares, minus that execution's fees and the opening fees of the shares
//     it closes (so trade P&L sums to the portfolio's realised P&L);
//   * profit_factor = gross_profit / gross_loss, meaningful only when
//     losing_trades > 0 (it is 0 otherwise; check the counts).
// -----------------------------------------------------------------------------

#include <cstdint>
#include <optional>

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
    std::uint64_t trade_count{0};            // closing executions

    std::uint64_t fill_count{0};             // every execution, opening or closing
    std::uint64_t winning_trades{0};         // trade P&L > 0
    std::uint64_t losing_trades{0};          // trade P&L < 0
    std::optional<double> win_rate{};        // winning / trade_count; unset with no trades
    std::optional<double> sharpe{};          // annualised; unset under 3 returns or zero volatility
    common::Money starting_equity{};
    common::Money ending_equity{};
    common::Money gross_profit{};            // sum of positive trade P&L
    common::Money gross_loss{};              // sum of |negative trade P&L|, >= 0
    common::Money fees_paid{};               // over every fill
};

}  // namespace trading_engine::domain
