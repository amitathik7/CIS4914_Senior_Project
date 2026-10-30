#pragma once

// -----------------------------------------------------------------------------
//  Training and tuning the ML strategy (ml_logistic) on recorded bars: the `train` and
//  `sweep` commands of strategy_lab_replay (docs/STRATEGY_LAB_TRAINING.md).
//
//  Both drive the REAL LogisticRegressionStrategy, built through the catalog, so a learned
//  weight here is exactly what the strategy would learn live: nothing is re-implemented.
//  The dataset's rows are split by position: the first (1 - holdout) of them are the
//  TRAINING period, the rest the HOLDOUT. The model keeps learning through both, exactly as
//  it would live (walk-forward); what the split changes is only which period a scored
//  prediction is counted in. A prediction is counted in the period of the row on which its
//  label became known.
//
//  train: one configuration. Reports, per symbol, the accuracy in each period against a
//         majority-class baseline, the signals in each period, and the final weights as an
//         `initial_parameters` warm start.
//  sweep: a grid of configurations. Each is scored on the TRAINING period only, and ranked
//         by that; the holdout of each is reported beside it but never used to choose, so
//         the best row's holdout accuracy is an honest out-of-sample check.
//
//  Deterministic: the same dataset and arguments give the same document, provenance aside.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lab/catalog.hpp"
#include "lab/dataset.hpp"
#include "lab/result_json.hpp"
#include "trading_engine/strategy/logistic_model.hpp"
#include "trading_engine/strategy/logistic_regression_strategy.hpp"

namespace trading_engine::lab {

inline constexpr std::string_view kTrainingSchema = "strategy_lab.training";
inline constexpr std::string_view kSweepSchema    = "strategy_lab.sweep";

// At most this many grid combinations in one sweep.
inline constexpr std::size_t kMaxSweepCombinations = 256;

// Accuracy figures for one period. Empty values mean "nothing was scored".
struct PeriodScore {
    std::uint64_t scored{0};
    std::uint64_t correct{0};
    std::uint64_t up_labels{0};
    std::uint64_t signals{0};

    [[nodiscard]] std::optional<double> accuracy() const;
    // The accuracy of always predicting the more frequent label in this period.
    [[nodiscard]] std::optional<double> majority_baseline() const;
    PeriodScore& operator+=(const PeriodScore& other);
};

struct SymbolTraining {
    std::string            symbol{};
    PeriodScore            training{};
    PeriodScore            holdout{};
    strategy::ml::Parameters parameters{};   // final weights then bias
};

struct TrainingRun {
    std::vector<std::pair<std::string, ParamValue>> parameters;   // every parameter, defaults filled in
    std::size_t                 rows{0};
    std::size_t                 training_rows{0};   // rows [0, training_rows) are the training period
    double                      holdout{0.0};
    std::vector<SymbolTraining> symbols{};          // in allowlist order
    PeriodScore                 training{};         // summed over symbols
    PeriodScore                 holdout_total{};
};

// `params` configure ml_logistic exactly as `run --param` does. When they do not name
// `symbols`, every symbol with a bar in the dataset is used. holdout is in [0, 1).
// Throws LabError: InvalidParameter / ConfigError as build_strategy does, UsageError for a
// bad holdout.
[[nodiscard]] TrainingRun train_ml(const Dataset& dataset, std::vector<ParamAssignment> params, double holdout);

struct SweepAxis {
    std::string              name{};
    std::vector<std::string> values{};   // texts, parsed like --param values
};

struct SweepRow {
    std::vector<ParamAssignment> assignment{};   // one value per axis, in axis order
    std::optional<TrainingRun>   run{};          // empty when the strategy rejected the combination
    std::string                  rejected{};     // the strategy's own words, when it did
};

struct SweepResult {
    std::vector<ParamAssignment> fixed{};
    std::vector<SweepAxis>       axes{};
    double                       holdout{0.0};
    std::vector<SweepRow>        rows{};   // ranked: best training accuracy first, rejected last
};

// Throws LabError(UsageError) for no axis, an empty or repeated axis, an axis that is also a
// fixed parameter, or more than kMaxSweepCombinations combinations; LabError(InvalidParameter)
// for a value that is not of the parameter's type. A value the STRATEGY rejects (say
// rearm_band >= entry_probability - 0.5) is recorded on its row, not thrown.
[[nodiscard]] SweepResult sweep_ml(const Dataset& dataset, const std::vector<ParamAssignment>& fixed,
                                   const std::vector<SweepAxis>& axes, double holdout);

[[nodiscard]] std::string training_json(const TrainingRun& run, const DatasetInfo& input,
                                        const Provenance& provenance, bool pretty);
[[nodiscard]] std::string sweep_json(const SweepResult& result, const DatasetInfo& input,
                                     const Provenance& provenance, bool pretty);

// "w0,w1,...,w5,bias", each the shortest text that reads back as the exact double: the
// value for LogisticRegressionConfig::initial_parameters.
[[nodiscard]] std::string initial_parameters_text(const strategy::ml::Parameters& parameters);

}  // namespace trading_engine::lab
