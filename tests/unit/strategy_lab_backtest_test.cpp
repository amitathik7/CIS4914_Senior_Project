// strategy_lab_backtest in process: every catalog kind backtests end to end, the document is
// deterministic, cash reconciles with the fills it lists, and failures keep the lab's exit
// statuses. tests/strategy_lab (Python) parses the same documents with a real JSON parser.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lab/backtest_cli.hpp"
#include "strategy_lab_test_support.hpp"

namespace {

using namespace lab_test;
using Args = std::vector<std::string>;

Args ml_args(const std::string& kind) {
    return {"--dataset", fixture_path("ml_trending.csv").string(), "--strategy", kind,
            "--param", "symbols=AAPL", "--param", "requested_quantity=50"};
}

TEST(StrategyLabBacktest, EveryCatalogKindRunsAndIsDeterministic) {
    for (const std::string kind : {"sma_crossover", "mean_reversion", "ml_logistic"}) {
        const auto a = lab::run_backtest_cli(ml_args(kind));
        const auto b = lab::run_backtest_cli(ml_args(kind));
        ASSERT_EQ(a.exit_code, 0) << kind << ": " << a.err;
        EXPECT_EQ(a.out, b.out) << kind;
        EXPECT_NE(a.out.find("\"schema\":\"strategy_lab.backtest\""), std::string::npos);
        EXPECT_NE(a.out.find("\"kind\":\"" + kind + "\""), std::string::npos);
        EXPECT_NE(a.out.find("\"fill_rule\":\"next_bar_open\""), std::string::npos);
        EXPECT_EQ(a.out.find('\n'), a.out.size() - 1) << "one line, one document";
    }
}

TEST(StrategyLabBacktest, TheMlStrategyTradesOnItsDemoDataset) {
    const auto r = lab::run_backtest_cli(ml_args("ml_logistic"));
    ASSERT_EQ(r.exit_code, 0) << r.err;
    EXPECT_EQ(r.out.find("\"fills\":[]"), std::string::npos) << "the demo dataset is meant to produce trades";
    EXPECT_NE(r.out.find("\"strategy_id\":\"ml_logistic\""), std::string::npos);
    EXPECT_NE(r.out.find("\"periods_per_year_source\":\"inferred_from_bar_spacing\""), std::string::npos);
}

TEST(StrategyLabBacktest, SettingsReachTheConfigAndTheRiskPolicies) {
    Args args = ml_args("ml_logistic");
    for (const char* extra : {"--cash", "2500", "--fee-per-trade", "1", "--max-order-quantity", "5",
                              "--periods-per-year", "252"}) {
        args.emplace_back(extra);
    }
    const auto r = lab::run_backtest_cli(args);
    ASSERT_EQ(r.exit_code, 0) << r.err;
    EXPECT_NE(r.out.find("\"starting_cash\":2500"), std::string::npos);
    EXPECT_NE(r.out.find("\"per_trade\":1"), std::string::npos);
    EXPECT_NE(r.out.find("\"policy\":\"max_order_quantity\""), std::string::npos);
    EXPECT_NE(r.out.find("\"adjusted_quantity\":5"), std::string::npos);
    EXPECT_NE(r.out.find("\"periods_per_year_source\":\"given\""), std::string::npos);
}

TEST(StrategyLabBacktest, FailuresKeepTheLabsExitStatuses) {
    EXPECT_EQ(lab::run_backtest_cli({"--strategy", "ml_logistic"}).exit_code, 2);   // no dataset
    EXPECT_EQ(lab::run_backtest_cli({"--dataset", fixture_path("missing.csv").string(), "--strategy",
                                     "ml_logistic", "--param", "symbols=A"})
                  .exit_code,
              3);
    Args bad = ml_args("ml_logistic");
    bad.insert(bad.end(), {"--cash", "-5"});
    const auto r = lab::run_backtest_cli(bad);
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_NE(r.out.find("\"schema\":\"strategy_lab.error\""), std::string::npos);
    Args twice = ml_args("ml_logistic");
    twice.insert(twice.end(), {"--strategy", "ml_logistic", "--param", "symbols=AAPL"});
    EXPECT_EQ(lab::run_backtest_cli(twice).exit_code, 2);   // duplicate strategy_id
}

}  // namespace
