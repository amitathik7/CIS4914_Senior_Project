#include "lab/training.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <map>
#include <memory>
#include <system_error>
#include <utility>

#include "lab/errors.hpp"
#include "lab/json_writer.hpp"
#include "lab/utf8.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/strategy/strategy.hpp"

namespace trading_engine::lab {

namespace {

constexpr std::string_view kKind          = "ml_logistic";
constexpr std::string_view kSchemaVersion = "1.0";

[[noreturn]] void usage_error(const std::string& message) {
    throw LabError(ErrorCode::UsageError, message, {Problem{0, "", "", message}}, 1);
}

std::string shortest(double value) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    return result.ec == std::errc{} ? std::string(std::begin(buffer), result.ptr) : std::to_string(value);
}

// Counts the signals of each symbol, split at the boundary row.
class CountingSink final : public strategy::ISignalSink {
public:
    void emit(const domain::TradeSignal& signal) override { ++counts[signal.symbol]; }
    std::map<std::string, std::uint64_t> counts{};
};

bool names(const std::vector<ParamAssignment>& params, std::string_view name) {
    return std::any_of(params.begin(), params.end(), [name](const ParamAssignment& p) { return p.name == name; });
}

std::string dataset_symbols(const Dataset& dataset) {
    std::string out;
    for (const SymbolSummary& summary : dataset.info.symbols) {
        if (summary.bar_rows == 0) {
            continue;
        }
        out += out.empty() ? "" : ",";
        out += summary.symbol;
    }
    return out;
}

PeriodScore difference(const strategy::ml::Scoreboard& later, const strategy::ml::Scoreboard& earlier) {
    PeriodScore out;
    out.scored    = later.scored - earlier.scored;
    out.correct   = later.correct - earlier.correct;
    out.up_labels = later.up_labels - earlier.up_labels;
    return out;
}

void write_param_value(JsonWriter& w, const ParamValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        w.string(*text);
    } else if (const auto* whole = std::get_if<std::uint64_t>(&value)) {
        w.unsigned_integer(*whole);
    } else if (const auto* real = std::get_if<double>(&value)) {
        w.number(*real);
    } else {
        w.begin_array();
        for (const std::string& item : std::get<std::vector<std::string>>(value)) {
            w.string(item);
        }
        w.end_array();
    }
}

void write_optional(JsonWriter& w, std::string_view name, std::optional<double> value) {
    w.key(name);
    if (value.has_value()) {
        w.number(*value);
    } else {
        w.null();
    }
}

void write_period(JsonWriter& w, std::string_view name, const PeriodScore& score) {
    w.key(name).begin_object();
    w.field("scored", score.scored);
    w.field("correct", score.correct);
    w.field("up_labels", score.up_labels);
    write_optional(w, "accuracy", score.accuracy());
    write_optional(w, "majority_baseline", score.majority_baseline());
    w.field("signals", score.signals);
    w.end_object();
}

void write_header(JsonWriter& w, std::string_view schema, const DatasetInfo& input, const Provenance& provenance) {
    w.field("schema", schema);
    w.field("schema_version", kSchemaVersion);
    w.key("provenance").begin_object();
    w.field("tool", provenance.tool);
    w.field("project_version", provenance.project_version);
    w.field("compiler", provenance.compiler);
    w.field("build", provenance.build);
    if (provenance.generated_at.has_value()) {
        w.field("generated_at", *provenance.generated_at);
    }
    w.end_object();
    w.key("input").begin_object();
    w.field("name", input.name);
    w.field("sha256", input.sha256);
    w.field("rows", static_cast<std::uint64_t>(input.rows));
    w.end_object();
}

void write_run(JsonWriter& w, const TrainingRun& run, bool with_parameters) {
    w.key("split").begin_object();
    w.field("rows", static_cast<std::uint64_t>(run.rows));
    w.field("training_rows", static_cast<std::uint64_t>(run.training_rows));
    w.field("holdout_rows", static_cast<std::uint64_t>(run.rows - run.training_rows));
    w.field("holdout", run.holdout);
    w.end_object();
    if (with_parameters) {
        w.key("parameters").begin_object();
        for (const auto& [name, value] : run.parameters) {
            w.key(name);
            write_param_value(w, value);
        }
        w.end_object();
    }
    write_period(w, "training", run.training);
    write_period(w, "holdout", run.holdout_total);
    w.key("symbols").begin_array();
    for (const SymbolTraining& symbol : run.symbols) {
        w.begin_object();
        w.field("symbol", symbol.symbol);
        write_period(w, "training", symbol.training);
        write_period(w, "holdout", symbol.holdout);
        w.key("weights").begin_object();
        for (std::size_t i = 0; i < strategy::ml::kFeatureCount; ++i) {
            w.field(strategy::ml::feature_names()[i], symbol.parameters[i]);
        }
        w.end_object();
        w.field("bias", symbol.parameters[strategy::ml::kFeatureCount]);
        w.field("initial_parameters", initial_parameters_text(symbol.parameters));
        w.end_object();
    }
    w.end_array();
}

}  // namespace

// --- PeriodScore ------------------------------------------------------------------------

std::optional<double> PeriodScore::accuracy() const {
    if (scored == 0) {
        return std::nullopt;
    }
    return static_cast<double>(correct) / static_cast<double>(scored);
}

std::optional<double> PeriodScore::majority_baseline() const {
    if (scored == 0) {
        return std::nullopt;
    }
    return static_cast<double>(std::max(up_labels, scored - up_labels)) / static_cast<double>(scored);
}

PeriodScore& PeriodScore::operator+=(const PeriodScore& other) {
    scored += other.scored;
    correct += other.correct;
    up_labels += other.up_labels;
    signals += other.signals;
    return *this;
}

std::string initial_parameters_text(const strategy::ml::Parameters& parameters) {
    std::string out;
    for (const double value : parameters) {
        out += out.empty() ? "" : ",";
        out += shortest(value);
    }
    return out;
}

// --- train ------------------------------------------------------------------------------

TrainingRun train_ml(const Dataset& dataset, std::vector<ParamAssignment> params, double holdout) {
    if (!std::isfinite(holdout) || holdout < 0.0 || holdout >= 1.0) {
        usage_error("--holdout must be at least 0 and below 1 (got " + shortest(holdout) + ")");
    }
    if (!names(params, "symbols")) {
        params.push_back(ParamAssignment{"symbols", dataset_symbols(dataset)});
    }
    const BuiltStrategy built = build_strategy(StrategyRequest{std::string{kKind}, params});
    const auto model = std::dynamic_pointer_cast<strategy::LogisticRegressionStrategy>(built.strategy);
    if (model == nullptr) {
        throw LabError(ErrorCode::InternalError, "the ml_logistic kind did not build a LogisticRegressionStrategy");
    }
    model->on_start();

    TrainingRun run;
    run.parameters    = built.parameters;
    run.rows          = dataset.rows.size();
    run.training_rows = static_cast<std::size_t>(std::floor(static_cast<double>(run.rows) * (1.0 - holdout)));
    run.holdout       = holdout;

    CountingSink sink;
    std::map<std::string, strategy::ml::Scoreboard> at_split;
    std::map<std::string, std::uint64_t>            signals_at_split;
    const auto snapshot = [&] {
        for (const std::string& symbol : built.symbols) {
            at_split[symbol] = model->scoreboard(symbol).value_or(strategy::ml::Scoreboard{});
        }
        signals_at_split = sink.counts;
    };
    for (std::size_t i = 0; i < dataset.rows.size(); ++i) {
        if (i == run.training_rows) {
            snapshot();
        }
        model->on_market_event(dataset.rows[i].event, sink);
    }
    if (run.training_rows == run.rows) {
        snapshot();
    }
    model->on_stop();

    for (const std::string& symbol : built.symbols) {
        const strategy::ml::Scoreboard final_score = model->scoreboard(symbol).value_or(strategy::ml::Scoreboard{});
        SymbolTraining row;
        row.symbol           = symbol;
        row.training         = difference(at_split[symbol], strategy::ml::Scoreboard{});
        row.holdout          = difference(final_score, at_split[symbol]);
        row.training.signals = signals_at_split[symbol];
        row.holdout.signals  = sink.counts[symbol] - signals_at_split[symbol];
        row.parameters       = model->parameters(symbol).value_or(strategy::ml::Parameters{});
        run.training += row.training;
        run.holdout_total += row.holdout;
        run.symbols.push_back(std::move(row));
    }
    return run;
}

// --- sweep ------------------------------------------------------------------------------

SweepResult sweep_ml(const Dataset& dataset, const std::vector<ParamAssignment>& fixed,
                     const std::vector<SweepAxis>& axes, double holdout) {
    if (axes.empty()) {
        usage_error("sweep needs at least one --grid name=v1,v2,...");
    }
    std::size_t combinations = 1;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const SweepAxis& axis = axes[i];
        if (axis.values.empty()) {
            usage_error("--grid " + axis.name + " lists no value");
        }
        if (names(fixed, axis.name)) {
            usage_error("'" + axis.name + "' is both a --param and a --grid axis");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (axes[j].name == axis.name) {
                usage_error("--grid " + axis.name + " was given more than once");
            }
        }
        combinations *= axis.values.size();
        if (combinations > kMaxSweepCombinations) {
            throw LabError(ErrorCode::LimitExceeded,
                           "the grid has more than " + std::to_string(kMaxSweepCombinations) + " combinations",
                           {Problem{0, "", "grid", "too many combinations"}}, 1);
        }
    }

    SweepResult result;
    result.fixed   = fixed;
    result.axes    = axes;
    result.holdout = holdout;

    std::vector<std::size_t> index(axes.size(), 0);
    for (std::size_t n = 0; n < combinations; ++n) {
        SweepRow row;
        std::vector<ParamAssignment> params = fixed;
        for (std::size_t a = 0; a < axes.size(); ++a) {
            const ParamAssignment chosen{axes[a].name, axes[a].values[index[a]]};
            row.assignment.push_back(chosen);
            params.push_back(chosen);
        }
        try {
            row.run = train_ml(dataset, params, holdout);
        } catch (const LabError& error) {
            if (error.code() != ErrorCode::ConfigError) {
                throw;   // a typo or a bad value type is the caller's mistake, not a grid point
            }
            row.rejected = error.what();
        }
        result.rows.push_back(std::move(row));

        // Odometer, last axis fastest.
        for (std::size_t a = axes.size(); a-- > 0;) {
            if (++index[a] < axes[a].values.size()) {
                break;
            }
            index[a] = 0;
        }
    }

    // Rank on the TRAINING period only; the holdout is never used to choose.
    std::stable_sort(result.rows.begin(), result.rows.end(), [](const SweepRow& a, const SweepRow& b) {
        const std::optional<double> x = a.run ? a.run->training.accuracy() : std::nullopt;
        const std::optional<double> y = b.run ? b.run->training.accuracy() : std::nullopt;
        if (x.has_value() != y.has_value()) {
            return x.has_value();
        }
        return x.has_value() && *x > *y;
    });
    return result;
}

// --- JSON -------------------------------------------------------------------------------

std::string training_json(const TrainingRun& run, const DatasetInfo& input, const Provenance& provenance,
                          bool pretty) {
    JsonWriter w{pretty};
    w.begin_object();
    write_header(w, kTrainingSchema, input, provenance);
    w.key("result").begin_object();
    w.field("kind", kKind);
    write_run(w, run, true);
    w.end_object();
    w.end_object();
    return w.take() + "\n";
}

std::string sweep_json(const SweepResult& result, const DatasetInfo& input, const Provenance& provenance,
                       bool pretty) {
    JsonWriter w{pretty};
    w.begin_object();
    write_header(w, kSweepSchema, input, provenance);
    w.key("result").begin_object();
    w.field("kind", kKind);
    w.field("ranked_by", "training.accuracy");
    w.field("holdout", result.holdout);
    w.key("fixed").begin_object();
    for (const ParamAssignment& p : result.fixed) {
        w.field(p.name, p.text);
    }
    w.end_object();
    w.key("axes").begin_array();
    for (const SweepAxis& axis : result.axes) {
        w.begin_object();
        w.field("name", axis.name);
        w.key("values").begin_array();
        for (const std::string& value : axis.values) {
            w.string(value);
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.key("rows").begin_array();
    for (const SweepRow& row : result.rows) {
        w.begin_object();
        w.key("assignment").begin_object();
        for (const ParamAssignment& p : row.assignment) {
            w.field(p.name, p.text);
        }
        w.end_object();
        if (row.run.has_value()) {
            write_period(w, "training", row.run->training);
            write_period(w, "holdout", row.run->holdout_total);
        } else {
            w.field("rejected", to_valid_utf8(row.rejected));
        }
        w.end_object();
    }
    w.end_array();
    // The winner in full, so its weights can be used as a warm start.
    const auto best = std::find_if(result.rows.begin(), result.rows.end(),
                                   [](const SweepRow& row) { return row.run.has_value(); });
    w.key("best");
    if (best == result.rows.end()) {
        w.null();
    } else {
        w.begin_object();
        w.key("assignment").begin_object();
        for (const ParamAssignment& p : best->assignment) {
            w.field(p.name, p.text);
        }
        w.end_object();
        write_run(w, *best->run, true);
        w.end_object();
    }
    w.end_object();
    w.end_object();
    return w.take() + "\n";
}

}  // namespace trading_engine::lab
