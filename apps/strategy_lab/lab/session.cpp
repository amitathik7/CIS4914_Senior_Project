#include "lab/session.hpp"

#include <algorithm>
#include <set>
#include <variant>

#include "lab/errors.hpp"
#include "lab/result_json.hpp"

namespace trading_engine::lab {

namespace {

std::vector<Warning> collect_warnings(const DatasetInfo& info, const std::vector<BuiltStrategy>& built) {
    std::vector<Warning> out;

    std::set<std::string> traded;
    for (const BuiltStrategy& strategy : built) {
        traded.insert(strategy.symbols.begin(), strategy.symbols.end());
    }
    for (const SymbolSummary& symbol : info.symbols) {
        if (traded.count(symbol.symbol) == 0) {
            out.push_back(Warning{"dataset_symbol_not_allowlisted",
                                  "dataset symbol '" + symbol.symbol +
                                      "' is not in any strategy's symbols list, so every row for it is ignored "
                                      "(matching is exact and case-sensitive)",
                                  ""});
        }
    }

    for (const BuiltStrategy& strategy : built) {
        for (const std::string& traded_symbol : strategy.symbols) {
            const auto found = std::find_if(info.symbols.begin(), info.symbols.end(),
                                            [&](const SymbolSummary& s) { return s.symbol == traded_symbol; });
            if (found == info.symbols.end()) {
                out.push_back(Warning{"allowlisted_symbol_not_in_dataset",
                                      "strategy '" + strategy.strategy_id + "' trades '" + traded_symbol +
                                          "', which does not appear in the dataset",
                                      strategy.strategy_id});
            } else if (found->bar_rows < strategy.window_size) {
                out.push_back(Warning{"fewer_bars_than_warmup",
                                      "'" + traded_symbol + "' has " + std::to_string(found->bar_rows) +
                                          " bar rows but strategy '" + strategy.strategy_id + "' needs " +
                                          std::to_string(strategy.window_size) +
                                          " accepted bars to leave warm-up, so it cannot decide on this symbol",
                                      strategy.strategy_id});
            }
        }
        for (const auto& [name, value] : strategy.derived) {
            if (name == "entry_threshold_reachable" && std::holds_alternative<bool>(value) &&
                !std::get<bool>(value)) {
                out.push_back(Warning{"entry_threshold_unreachable",
                                      "strategy '" + strategy.strategy_id +
                                          "': entry_threshold exceeds sqrt(lookback - 1), the bound on |z|, so it "
                                          "can never fire",
                                      strategy.strategy_id});
            }
        }
    }
    return out;
}

}  // namespace

RunDocument run_session(const Dataset& dataset, const std::vector<StrategyRequest>& requests,
                        const SessionOptions& options) {
    std::vector<BuiltStrategy> built;
    built.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        built.push_back(build_strategy(requests[i], i));
    }

    RunDocument document;
    document.input    = dataset.info;
    document.max_rows = options.max_rows;
    document.warnings = collect_warnings(dataset.info, built);

    std::vector<ReplayEvent> events;
    events.reserve(dataset.rows.size());
    for (const DatasetRow& row : dataset.rows) {
        events.push_back(ReplayEvent{row.event, row.line});
    }

    ReplayOptions replay_options;
    replay_options.fault = options.fault;
    document.replay      = run_replay(std::move(built), events, replay_options);
    document.run_id      = make_run_id(document);
    return document;
}

}  // namespace trading_engine::lab
