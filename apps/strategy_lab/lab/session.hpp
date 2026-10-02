#pragma once

// A session ties one validated Dataset and one or more strategy requests to a replay:
// build fresh strategies, run the real engine over the dataset's rows in file order, and
// collect warnings about the match between data and configuration.
//
// Warnings are FACTS the user may want to see, never repairs and never errors:
//   dataset_symbol_not_allowlisted    a symbol in the data that no strategy trades (the
//                                     strategies match case-sensitively: "aapl" is not "AAPL")
//   allowlisted_symbol_not_in_dataset a symbol a strategy trades that never appears in the data
//   fewer_bars_than_warmup            a traded symbol has fewer bar rows than the window needs,
//                                     so that strategy cannot leave warm-up on it
//   entry_threshold_unreachable       mean reversion: entry_threshold exceeds sqrt(lookback - 1),
//                                     the bound on |z|, so it can never fire

#include <cstddef>
#include <string>
#include <vector>

#include "lab/catalog.hpp"
#include "lab/dataset.hpp"
#include "lab/runner.hpp"

namespace trading_engine::lab {

struct Warning {
    std::string code{};
    std::string message{};
    std::string strategy_id{};   // empty for a dataset-level warning
};

struct SessionOptions {
    std::size_t               max_rows{kDefaultMaxRows};   // echoed in the output; enforced by the loader
    SynchronousDemoBus::Fault fault{SynchronousDemoBus::Fault::None};
};

// Everything the output document is built from.
struct RunDocument {
    std::string            run_id{};   // content-addressed, see result_json.hpp
    DatasetInfo            input{};
    std::size_t            max_rows{kDefaultMaxRows};
    std::vector<Warning>   warnings{};
    ReplayResult           replay{};
};

// Throws LabError (see catalog.hpp and runner.hpp). Strategies are built fresh here.
[[nodiscard]] RunDocument run_session(const Dataset& dataset, const std::vector<StrategyRequest>& requests,
                                      const SessionOptions& options = {});

}  // namespace trading_engine::lab
