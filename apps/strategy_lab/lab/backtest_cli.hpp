#pragma once

// -----------------------------------------------------------------------------
//  strategy_lab_backtest: a full simulated backtest (app::Backtest: strategies,
//  risk, execution, portfolio, analytics) over one Strategy Lab dataset, as a
//  pure function -- arguments in, stdout text, stderr text and an exit status
//  out. The strategy arguments are the replay tool's, so every catalog kind
//  (sma_crossover, mean_reversion, ml_logistic) works unchanged.
//
//    strategy_lab_backtest --dataset <csv> --strategy <kind> [--param name=value]...
//                          [--strategy <kind> [--param ...]]...
//                          [--cash D] [--slippage-bps X]
//                          [--fee-per-share D] [--fee-per-trade D] [--fee-bps X]
//                          [--max-order-quantity N] [--max-position-notional D]
//                          [--max-gross-exposure D] [--max-daily-loss D]
//                          [--max-open-orders N] [--no-risk-policies]
//                          [--periods-per-year N] [--max-rows N] [--pretty]
//    @<file>   anywhere: replaced by that file's lines (as in strategy_lab_replay)
//
//  D is an exact plain decimal ("100000", "0.005"), X a decimal number of basis
//  points, N a whole number. Defaults are config::SimulationConfig{} and
//  config::RiskLimits{} (printed in the output's `config`).
//
//  stdout: exactly one JSON document, `strategy_lab.backtest` (docs/BACKTESTING.md
//  section 5) or the shared `strategy_lab.error`; exit status as lab/errors.hpp.
//  Deterministic: the document is a pure function of the dataset and arguments.
// -----------------------------------------------------------------------------

#include <string>
#include <string_view>
#include <vector>

#include "lab/cli.hpp"

namespace trading_engine::lab {

inline constexpr std::string_view kBacktestSchema = "strategy_lab.backtest";

// `args` excludes the program name. Never throws.
[[nodiscard]] CliResult run_backtest_cli(const std::vector<std::string>& args);

[[nodiscard]] std::string backtest_usage_text();

}  // namespace trading_engine::lab
