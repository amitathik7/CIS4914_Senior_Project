// The `train` and `sweep` commands (lab/training.hpp): the period split, the scores against
// the majority baseline, the warm-start text, the sweep's ranking and its rejected rows, the
// limits, and the command line around them.

#include <charconv>
#include <cmath>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "lab/cli.hpp"
#include "lab/training.hpp"
#include "strategy_lab_test_support.hpp"

namespace {

using namespace lab_test;
namespace strategy = trading_engine::strategy;

lab::Dataset trending() { return lab::load_dataset(fixture_path("ml_trending.csv")); }

std::vector<lab::ParamAssignment> assign(const Params& params) {
    std::vector<lab::ParamAssignment> out;
    for (const auto& [name, text] : params) {
        out.push_back(lab::ParamAssignment{name, text});
    }
    return out;
}

const Params kQuick = {{"horizon", "1"}, {"min_training_samples", "20"}};

std::vector<double> parse_doubles(const std::string& text) {
    std::vector<double> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end   = comma == std::string::npos ? text.size() : comma;
        double value            = 0.0;
        const auto result       = std::from_chars(text.data() + start, text.data() + end, value);
        EXPECT_EQ(result.ec, std::errc{});
        EXPECT_EQ(result.ptr, text.data() + end);
        out.push_back(value);
        start = end + 1;
    }
    return out;
}

void expect_code(lab::ErrorCode code, const std::function<void()>& action) {
    try {
        action();
        ADD_FAILURE() << "expected a LabError";
    } catch (const lab::LabError& error) {
        EXPECT_EQ(error.code(), code) << error.what();
    }
}

// ---- train -------------------------------------------------------------------------------------

TEST(LabTraining, LearnsThePlantedTrendWellAboveTheMajorityBaselineInBothPeriods) {
    const lab::TrainingRun run = lab::train_ml(trending(), assign(kQuick), 0.3);
    EXPECT_EQ(run.rows, 240u);
    EXPECT_EQ(run.training_rows, 168u);   // floor(240 * 0.7)
    ASSERT_EQ(run.symbols.size(), 1u);
    EXPECT_EQ(run.symbols[0].symbol, "AAPL");

    for (const lab::PeriodScore* period : {&run.training, &run.holdout_total}) {
        ASSERT_TRUE(period->accuracy().has_value());
        ASSERT_TRUE(period->majority_baseline().has_value());
        EXPECT_GT(*period->accuracy(), 0.85);
        EXPECT_GT(*period->accuracy(), *period->majority_baseline() + 0.25);
        EXPECT_GT(period->signals, 0u);
    }
    for (const double value : run.symbols[0].parameters) {
        EXPECT_TRUE(std::isfinite(value));
    }
}

TEST(LabTraining, TheSplitOnlyMovesScoresBetweenPeriodsAndNeverChangesTheModel) {
    const lab::TrainingRun a = lab::train_ml(trending(), assign(kQuick), 0.3);
    const lab::TrainingRun b = lab::train_ml(trending(), assign(kQuick), 0.0);
    EXPECT_EQ(b.training_rows, 240u);
    EXPECT_EQ(b.holdout_total.scored, 0u);
    EXPECT_FALSE(b.holdout_total.accuracy().has_value());
    EXPECT_FALSE(b.holdout_total.majority_baseline().has_value());

    EXPECT_EQ(a.training.scored + a.holdout_total.scored, b.training.scored);
    EXPECT_EQ(a.training.correct + a.holdout_total.correct, b.training.correct);
    EXPECT_EQ(a.training.signals + a.holdout_total.signals, b.training.signals);
    EXPECT_EQ(a.symbols[0].parameters, b.symbols[0].parameters);
}

TEST(LabTraining, TheWarmStartTextReadsBackAsTheExactLearnedParameters) {
    const lab::TrainingRun run = lab::train_ml(trending(), assign(kQuick), 0.3);
    const std::string text     = lab::initial_parameters_text(run.symbols[0].parameters);
    const std::vector<double> values = parse_doubles(text);
    ASSERT_EQ(values.size(), strategy::ml::kParameterCount);
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], run.symbols[0].parameters[i]) << "parameter " << i;
    }

    strategy::LogisticRegressionConfig config;
    config.symbols            = {"AAPL"};
    config.initial_parameters = values;
    strategy::LogisticRegressionStrategy warm{config};
    warm.on_start();
    EXPECT_EQ(warm.parameters("AAPL"), run.symbols[0].parameters);
}

TEST(LabTraining, DefaultsToEverySymbolWithABarAndReportsEachSeparately) {
    const lab::Dataset dataset = lab::load_dataset(fixture_path("two_symbols_interleaved.csv"));
    const lab::TrainingRun run = lab::train_ml(dataset, {}, 0.3);
    ASSERT_EQ(run.symbols.size(), 2u);
    EXPECT_EQ(run.symbols[0].symbol, "AAPL");
    EXPECT_EQ(run.symbols[1].symbol, "MSFT");
    EXPECT_EQ(run.training.scored, 0u);   // too few bars to finish warming up
}

TEST(LabTraining, ABadHoldoutOrParameterIsRefused) {
    expect_code(lab::ErrorCode::UsageError, [] { (void)lab::train_ml(trending(), {}, 1.0); });
    expect_code(lab::ErrorCode::UsageError, [] { (void)lab::train_ml(trending(), {}, -0.1); });
    expect_code(lab::ErrorCode::InvalidParameter, [] { (void)lab::train_ml(trending(), assign({{"horizon", "x"}}), 0.3); });
    expect_code(lab::ErrorCode::ConfigError,
                [] { (void)lab::train_ml(trending(), assign({{"rearm_band", "0.2"}}), 0.3); });
}

// ---- sweep -------------------------------------------------------------------------------------

TEST(LabSweep, RanksByTrainingAccuracyAndPutsRejectedCombinationsLast) {
    const lab::SweepResult result =
        lab::sweep_ml(trending(), assign({{"min_training_samples", "20"}}),
                      {{"horizon", {"5", "1", "3"}}, {"rearm_band", {"0.05", "0.2"}}}, 0.3);
    ASSERT_EQ(result.rows.size(), 6u);
    for (std::size_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(result.rows[i].run.has_value());
        EXPECT_TRUE(result.rows[i].rejected.empty());
        if (i > 0) {
            EXPECT_GE(*result.rows[i - 1].run->training.accuracy(), *result.rows[i].run->training.accuracy());
        }
    }
    EXPECT_EQ(result.rows[0].assignment[0].text, "1");   // the shortest horizon fits a 1-bar trend best
    for (std::size_t i = 3; i < 6; ++i) {
        EXPECT_FALSE(result.rows[i].run.has_value());
        EXPECT_NE(result.rows[i].rejected.find("rearm_band"), std::string::npos) << result.rows[i].rejected;
        EXPECT_EQ(result.rows[i].assignment[1].text, "0.2");
    }
}

TEST(LabSweep, TheBestRowIsTheSameRunAsTrainingThatConfigurationAlone) {
    const lab::SweepResult result =
        lab::sweep_ml(trending(), assign({{"min_training_samples", "20"}}), {{"horizon", {"1", "3"}}}, 0.3);
    const lab::TrainingRun alone = lab::train_ml(trending(), assign(kQuick), 0.3);
    ASSERT_TRUE(result.rows[0].run.has_value());
    EXPECT_EQ(result.rows[0].run->symbols[0].parameters, alone.symbols[0].parameters);
    EXPECT_EQ(result.rows[0].run->holdout_total.correct, alone.holdout_total.correct);
}

TEST(LabSweep, RefusesMalformedGridsAndTooManyCombinations) {
    const lab::Dataset dataset = trending();
    expect_code(lab::ErrorCode::UsageError, [&] { (void)lab::sweep_ml(dataset, {}, {}, 0.3); });
    expect_code(lab::ErrorCode::UsageError, [&] { (void)lab::sweep_ml(dataset, {}, {{"horizon", {}}}, 0.3); });
    expect_code(lab::ErrorCode::UsageError,
                [&] { (void)lab::sweep_ml(dataset, {}, {{"horizon", {"1"}}, {"horizon", {"2"}}}, 0.3); });
    expect_code(lab::ErrorCode::UsageError,
                [&] { (void)lab::sweep_ml(dataset, assign({{"horizon", "1"}}), {{"horizon", {"2"}}}, 0.3); });
    expect_code(lab::ErrorCode::InvalidParameter, [&] { (void)lab::sweep_ml(dataset, {}, {{"horizon", {"1", "x"}}}, 0.3); });

    std::vector<std::string> seventeen;
    for (int i = 1; i <= 17; ++i) {
        seventeen.push_back(std::to_string(i));
    }
    std::vector<std::string> sixteen(seventeen.begin(), seventeen.end() - 1);
    expect_code(lab::ErrorCode::LimitExceeded,
                [&] { (void)lab::sweep_ml(dataset, {}, {{"horizon", seventeen}, {"min_training_samples", sixteen}}, 0.3); });
}

// ---- the command line --------------------------------------------------------------------------

lab::CliResult cli(std::vector<std::string> args) {
    lab::CliEnvironment env;
    env.now_utc = [] { return std::string{"2030-01-01T00:00:00.000000000Z"}; };
    return lab::run_cli(args, env);
}

bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

TEST(LabTrainingCli, TrainWritesOneTrainingDocument) {
    const lab::CliResult result = cli({"train", "--dataset", fixture_path("ml_trending.csv").string(), "--param",
                                       "horizon=1", "--param", "min_training_samples=20", "--holdout", "0.25"});
    EXPECT_EQ(result.exit_code, 0) << result.err;
    EXPECT_TRUE(result.err.empty()) << result.err;
    EXPECT_TRUE(contains(result.out, "{\"schema\":\"strategy_lab.training\",\"schema_version\":\"1.0\""));
    EXPECT_TRUE(contains(result.out, "\"training_rows\":180"));
    EXPECT_TRUE(contains(result.out, "\"initial_parameters\":\""));
    EXPECT_TRUE(contains(result.out, "\"generated_at\":\"2030-01-01T00:00:00.000000000Z\""));
}

TEST(LabTrainingCli, SweepWritesOneSweepDocumentWithItsBestRow) {
    const lab::CliResult result =
        cli({"sweep", "--dataset", fixture_path("ml_trending.csv").string(), "--param", "min_training_samples=20",
             "--grid", "horizon=1,3", "--no-wall-clock"});
    EXPECT_EQ(result.exit_code, 0) << result.err;
    EXPECT_TRUE(contains(result.out, "{\"schema\":\"strategy_lab.sweep\",\"schema_version\":\"1.0\""));
    EXPECT_TRUE(contains(result.out, "\"ranked_by\":\"training.accuracy\""));
    EXPECT_TRUE(contains(result.out, "\"best\":{"));
    EXPECT_FALSE(contains(result.out, "generated_at"));
}

TEST(LabTrainingCli, ArgumentMistakesAreUsageErrors) {
    const std::string csv = fixture_path("ml_trending.csv").string();
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"train"},
             {"train", "--dataset", csv, "--holdout", "1"},
             {"train", "--dataset", csv, "--holdout", "abc"},
             {"train", "--dataset", csv, "--holdout", "0.1", "--holdout", "0.2"},
             {"train", "--dataset", csv, "--grid", "horizon=1,2"},
             {"train", "--dataset", csv, "--strategy", "ml_logistic"},
             {"train", "--dataset", csv, "--param", "horizon"},
             {"sweep", "--dataset", csv},
             {"sweep", "--dataset", csv, "--grid", "=1,2"},
             {"fit", "--dataset", csv}}) {
        const lab::CliResult result = cli(args);
        EXPECT_EQ(result.exit_code, lab::kExitUsage) << args.front() << " " << result.err;
        EXPECT_TRUE(contains(result.out, "\"code\":\"usage_error\"")) << result.out;
    }
}

}  // namespace
