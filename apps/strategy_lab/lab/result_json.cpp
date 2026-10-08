#include "lab/result_json.hpp"

#include <cmath>
#include <map>
#include <type_traits>
#include <variant>

#include "lab/dataset.hpp"
#include "lab/json_writer.hpp"
#include "lab/sha256.hpp"
#include "lab/timestamp.hpp"
#include "lab/utf8.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/version.hpp"

namespace trading_engine::lab {

namespace {

constexpr std::uint64_t u64(std::size_t value) noexcept {
    return static_cast<std::uint64_t>(value);
}

constexpr std::string_view kNotice =
    "Signal replay only: the output shows what the real StrategyEngine and strategies decided on each event and "
    "the TradeSignals they emitted. There are no orders, fills, positions, P&L or confidence, no ML, and no "
    "production event bus: events flow through a lab-local synchronous bus.";

// ---- small value writers --------------------------------------------------------------

void write_param(JsonWriter& w, const ParamValue& value) {
    std::visit(
        [&w](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                w.string(v);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                w.unsigned_integer(v);
            } else if constexpr (std::is_same_v<T, double>) {
                w.number(v);
            } else {
                w.begin_array();
                for (const std::string& item : v) {
                    w.string(item);
                }
                w.end_array();
            }
        },
        value);
}

void write_derived(JsonWriter& w, const DerivedValue& value) {
    std::visit(
        [&w](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                w.boolean(v);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                w.unsigned_integer(v);
            } else {
                w.number(v);
            }
        },
        value);
}

// An optional number: omitted when absent; when not finite, omitted and described by
// "<name>_status". Never writes NaN or infinity.
void write_optional_number(JsonWriter& w, std::string_view name, const std::optional<double>& value) {
    if (!value.has_value()) {
        return;
    }
    if (std::isfinite(*value)) {
        w.key(name).number(*value);
        return;
    }
    w.key(std::string{name} + "_status").string(std::isnan(*value) ? "nan" : (*value > 0.0 ? "inf" : "-inf"));
}

// A price: a Decimal (an exact integer count of 1e-6), written as its exact decimal text.
void write_optional_price(JsonWriter& w, std::string_view name, const std::optional<common::Price>& value) {
    if (value.has_value()) {
        w.key(name).scaled_number(value->micros(), common::kDecimalPlaces);
    }
}

// A quantity or volume: a whole number of shares.
void write_optional_quantity(JsonWriter& w, std::string_view name, const std::optional<common::Quantity>& value) {
    if (value.has_value()) {
        w.key(name).integer(*value);
    }
}

void write_string_map(JsonWriter& w, const std::map<std::string, std::string>& values) {
    w.begin_object();
    for (const auto& [key, value] : values) {
        w.key(key).string(value);
    }
    w.end_object();
}

void write_signal_fields(JsonWriter& w, const std::string& run_id, const domain::TradeSignal& signal,
                         std::size_t event_index) {
    w.field("signal_id", signal.id.value);
    w.field("signal_ref", run_id + ":" + std::to_string(signal.id.value));
    w.field("strategy_id", signal.strategy_id);
    w.field("event_index", u64(event_index));
    w.field("symbol", signal.symbol);
    w.field("side", domain::to_string(signal.side));
    if (signal.order_type.has_value()) {
        w.field("order_type", domain::to_string(*signal.order_type));
    }
    write_optional_quantity(w, "requested_quantity", signal.requested_quantity);
    write_optional_number(w, "target_exposure", signal.target_exposure);
    write_optional_price(w, "limit_price", signal.limit_price);
    write_optional_number(w, "confidence", signal.confidence);
    w.field("created_at", format_utc_timestamp(signal.created_at));
}

// ---- sections of `result` -------------------------------------------------------------------

void write_run(JsonWriter& w, const RunDocument& doc) {
    w.key("run").begin_object();
    w.field("run_id", doc.run_id);
    w.field("mode", "signal_replay");
    w.field("notice", kNotice);
    w.field("signal_id_scope",
            "Signal ids count from 1 in every run and are unique only within this run_id; "
            "signal_ref is run-scoped.");
    w.key("context").begin_object();
    w.field("engine", "trading_engine::strategy::StrategyEngine (real)");
    w.field("event_bus", "SynchronousDemoBus (lab-local, single-threaded; not the production queue)");
    w.field("clock", "ManualClock set to each event's exchange_time before it is published");
    w.field("replay_order", "file order; nothing is sorted or repaired");
    w.field("bus_fault", doc.replay.bus.fault);
    w.end_object();
    w.end_object();
}

void write_input(JsonWriter& w, const DatasetInfo& info) {
    w.key("input").begin_object();
    w.key("dataset").begin_object();
    w.field("name", info.name);
    w.field("format", kDatasetFormat);
    w.field("sha256", info.sha256);
    w.field("bytes", u64(info.bytes));
    w.field("rows", u64(info.rows));
    w.field("first_exchange_time", format_utc_timestamp(info.first_exchange_time));
    w.field("last_exchange_time", format_utc_timestamp(info.last_exchange_time));
    w.key("symbols").begin_array();
    for (const SymbolSummary& symbol : info.symbols) {
        w.begin_object();
        w.field("symbol", symbol.symbol);
        w.field("bar_rows", u64(symbol.bar_rows));
        w.field("trade_rows", u64(symbol.trade_rows));
        w.field("first_exchange_time", format_utc_timestamp(symbol.first));
        w.field("last_exchange_time", format_utc_timestamp(symbol.last));
        w.end_object();
    }
    w.end_array();
    w.end_object();
    w.end_object();
}

void write_configuration(JsonWriter& w, const RunDocument& doc) {
    w.key("configuration").begin_object();
    w.field("max_rows", u64(doc.max_rows));
    w.field("bus_fault", doc.replay.bus.fault);
    w.key("strategies").begin_array();
    for (const StrategyRunInfo& strategy : doc.replay.strategies) {
        w.begin_object();
        w.field("kind", strategy.kind);
        w.field("strategy_id", strategy.strategy_id);
        w.field("window_size", u64(strategy.window_size));
        w.key("parameters").begin_object();
        for (const auto& [name, value] : strategy.parameters) {
            w.key(name);
            write_param(w, value);
        }
        w.end_object();
        w.key("derived").begin_object();
        for (const auto& [name, value] : strategy.derived) {
            w.key(name);
            write_derived(w, value);
        }
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

void write_warnings(JsonWriter& w, const std::vector<Warning>& warnings) {
    w.key("warnings").begin_array();
    for (const Warning& warning : warnings) {
        w.begin_object();
        w.field("code", warning.code);
        w.field("message", warning.message);
        if (!warning.strategy_id.empty()) {
            w.field("strategy_id", warning.strategy_id);
        }
        w.end_object();
    }
    w.end_array();
}

// Why an indicator has no value on this bar.
std::string unavailable_reason(const DiagnosticRecord& record) {
    if (record.verdict == "ignored") {
        return "event_ignored";
    }
    if (record.verdict == "warming_up") {
        return "warming_up";
    }
    return record.reason;   // evaluated, but this value does not exist (constant_window, measurement_failed)
}

void write_diagnostics(JsonWriter& w, const DiagnosticRecord& record) {
    w.field("verdict", record.verdict);
    w.field("reason", record.reason);
    w.field("action", record.action);
    w.key("window").begin_object();
    if (record.window_fill.has_value()) {
        w.field("fill", u64(*record.window_fill));
        w.field("remaining", u64(record.window_size > *record.window_fill ? record.window_size - *record.window_fill : 0));
    }
    w.field("size", u64(record.window_size));
    w.end_object();

    w.key("indicators").begin_object();
    for (const auto& [name, value] : record.indicators) {
        if (value.has_value() && std::isfinite(*value)) {
            w.key(name).number(*value);
        }
    }
    w.end_object();
    w.key("unavailable").begin_object();
    for (const auto& [name, value] : record.indicators) {
        if (!value.has_value()) {
            w.key(name).string(unavailable_reason(record));
        } else if (!std::isfinite(*value)) {
            w.key(name).string("non_finite");
        }
    }
    w.end_object();
    w.key("states").begin_object();
    for (const auto& [name, value] : record.states) {
        w.key(name).string(value);
    }
    w.end_object();
}

void write_events(JsonWriter& w, const ReplayResult& replay) {
    w.key("events").begin_array();
    for (const EventResult& event : replay.events) {
        const domain::MarketEvent& input = event.input.event;
        w.begin_object();
        w.field("index", u64(event.index));
        if (event.input.source_line != 0) {
            w.field("source_line", u64(event.input.source_line));
        }
        w.field("symbol", input.symbol);
        w.field("exchange_time", format_utc_timestamp(input.exchange_time));
        w.field("type", domain::to_string(input.type));
        write_optional_price(w, "price", input.price);
        write_optional_price(w, "open", input.open);
        write_optional_price(w, "high", input.high);
        write_optional_price(w, "low", input.low);
        write_optional_quantity(w, "volume", input.volume);
        w.field("bus_sequence", event.bus_sequence);
        w.key("results").begin_array();
        for (std::size_t s = 0; s < event.per_strategy.size(); ++s) {
            const StrategyEventResult& result = event.per_strategy[s];
            w.begin_object();
            w.field("strategy_id", replay.strategies[s].strategy_id);
            if (result.diagnostics.has_value()) {
                w.field("diagnostics_available", true);
                write_diagnostics(w, *result.diagnostics);
            } else {
                w.field("diagnostics_available", false);
                w.field("diagnostics_unavailable_reason",
                        "the strategy reported nothing for this event (it threw, or the collector failed)");
            }
            w.key("signal_ids").begin_array();
            for (const std::uint64_t id : result.signal_ids) {
                w.unsigned_integer(id);
            }
            w.end_array();
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
}

void write_signals(JsonWriter& w, const RunDocument& doc) {
    w.key("signals").begin_array();
    for (const SignalRecord& record : doc.replay.signals) {
        w.begin_object();
        write_signal_fields(w, doc.run_id, record.signal, record.event_index);
        w.field("bus_sequence", record.bus_sequence);
        w.key("metadata");
        write_string_map(w, record.signal.metadata);
        w.end_object();
    }
    w.end_array();

    w.key("publication_failures").begin_array();
    for (const PublicationFailure& failure : doc.replay.failures) {
        w.begin_object();
        write_signal_fields(w, doc.run_id, failure.signal, failure.event_index);
        w.field("kind", failure.kind);
        w.field("message", failure.message);
        w.key("metadata");
        write_string_map(w, failure.signal.metadata);
        w.end_object();
    }
    w.end_array();
}

void write_engine(JsonWriter& w, const ReplayResult& replay) {
    const auto& stats = replay.engine_stats;
    w.key("engine").begin_object();
    w.key("stats").begin_object();
    w.field("events_routed", stats.events_routed);
    w.field("events_dropped", stats.events_dropped);
    w.field("strategy_errors", stats.strategy_errors);
    w.field("signals_published", stats.signals_published);
    w.field("signals_rejected", stats.signals_rejected);
    w.field("publish_errors", stats.publish_errors);
    w.field("signals_dropped", stats.signals_dropped);
    w.field("bus_errors", stats.bus_errors);
    w.end_object();
    w.key("strategies").begin_array();
    for (const StrategyRunInfo& strategy : replay.strategies) {
        w.begin_object();
        w.field("strategy_id", strategy.strategy_id);
        w.field("events_delivered", strategy.stats.events_delivered);
        w.field("errors", strategy.stats.errors);
        w.field("signals_published", strategy.stats.signals_published);
        w.field("signals_rejected", strategy.stats.signals_rejected);
        w.field("publish_errors", strategy.stats.publish_errors);
        w.field("signals_dropped", strategy.stats.signals_dropped);
        w.field("last_error", strategy.stats.last_error);
        w.field("observer_failures", strategy.observer_failures);
        w.end_object();
    }
    w.end_array();
    w.key("bus").begin_object();
    w.field("implementation", "SynchronousDemoBus");
    w.field("fault", replay.bus.fault);
    w.field("market_data_published", replay.bus.market_data_published);
    w.field("signals_published", replay.bus.signals_published);
    w.field("signals_refused_by_fault", replay.bus.signals_refused_by_fault);
    w.field("rejected_not_running", replay.bus.rejected_not_running);
    w.end_object();
    w.end_object();
}

// Counts only: nothing here is a decision.
void write_summary(JsonWriter& w, const ReplayResult& replay) {
    std::size_t bars = 0;
    for (const EventResult& event : replay.events) {
        bars += event.input.event.type == domain::MarketEventType::Bar ? 1U : 0U;
    }
    w.key("summary").begin_object();
    w.field("events", u64(replay.events.size()));
    w.field("bars", u64(bars));
    w.field("signals", u64(replay.signals.size()));
    w.key("strategies").begin_array();
    for (std::size_t s = 0; s < replay.strategies.size(); ++s) {
        std::size_t buys = 0;
        std::size_t sells = 0;
        for (const SignalRecord& record : replay.signals) {
            if (record.signal.strategy_id == replay.strategies[s].strategy_id) {
                (record.signal.side == domain::SignalSide::Buy ? buys : sells) += 1;
            }
        }
        std::size_t ignored = 0, warming = 0, evaluated = 0, unavailable = 0;
        std::map<std::string, std::size_t> reasons;
        for (const EventResult& event : replay.events) {
            const auto& diagnostics = event.per_strategy[s].diagnostics;
            if (!diagnostics.has_value()) {
                ++unavailable;
                continue;
            }
            ++reasons[diagnostics->reason];
            if (diagnostics->verdict == "ignored") {
                ++ignored;
            } else if (diagnostics->verdict == "warming_up") {
                ++warming;
            } else {
                ++evaluated;
            }
        }
        w.begin_object();
        w.field("strategy_id", replay.strategies[s].strategy_id);
        w.key("signals").begin_object();
        w.field("buy", u64(buys));
        w.field("sell", u64(sells));
        w.end_object();
        w.key("verdicts").begin_object();
        w.field("ignored", u64(ignored));
        w.field("warming_up", u64(warming));
        w.field("evaluated", u64(evaluated));
        w.field("unavailable", u64(unavailable));
        w.end_object();
        w.key("reasons").begin_object();
        for (const auto& [code, count] : reasons) {
            w.key(code).unsigned_integer(u64(count));
        }
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

void write_result(JsonWriter& w, const RunDocument& doc) {
    w.begin_object();
    write_run(w, doc);
    write_input(w, doc.input);
    write_configuration(w, doc);
    write_warnings(w, doc.warnings);
    write_events(w, doc.replay);
    write_signals(w, doc);
    write_engine(w, doc.replay);
    write_summary(w, doc.replay);
    w.end_object();
}

std::string compiler_name() {
#if defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return std::string{"clang "} + __clang_version__;
#elif defined(__GNUC__)
    return std::string{"gcc "} + __VERSION__;
#else
    return "unknown";
#endif
}

}  // namespace

Provenance current_provenance(std::optional<std::string> generated_at) {
    Provenance provenance;
    provenance.tool            = "strategy_lab_replay";
    provenance.project_version = std::string{kVersion};
    provenance.compiler        = compiler_name();
#ifdef NDEBUG
    provenance.build = "ndebug";
#else
    provenance.build = "debug";
#endif
    provenance.generated_at = std::move(generated_at);
    return provenance;
}

std::string make_run_id(const RunDocument& document) {
    JsonWriter w;
    w.begin_object();
    w.field("schema_version", kSchemaVersion);
    w.field("dataset_sha256", document.input.sha256);
    w.field("bus_fault", document.replay.bus.fault);
    w.key("strategies").begin_array();
    for (const StrategyRunInfo& strategy : document.replay.strategies) {
        w.begin_object();
        w.field("kind", strategy.kind);
        w.key("parameters").begin_object();
        for (const auto& [name, value] : strategy.parameters) {
            w.key(name);
            write_param(w, value);
        }
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return sha256_hex(w.str()).substr(0, 16);
}

std::string result_json(const RunDocument& document) {
    JsonWriter w;
    write_result(w, document);
    return w.take();
}

std::string replay_json(const RunDocument& document, const Provenance& provenance, bool pretty) {
    JsonWriter w{pretty};
    w.begin_object();
    w.field("schema", kReplaySchema);
    w.field("schema_version", kSchemaVersion);
    w.key("provenance").begin_object();
    w.field("tool", provenance.tool);
    w.field("project_version", provenance.project_version);
    w.field("compiler", provenance.compiler);
    w.field("build", provenance.build);
    if (provenance.generated_at.has_value()) {
        w.field("generated_at", *provenance.generated_at);
    }
    w.field("result_sha256", sha256_hex(result_json(document)));
    w.end_object();
    w.key("result");
    write_result(w, document);
    w.end_object();
    std::string text = w.take();
    text += '\n';
    return text;
}

std::string catalog_json(bool pretty) {
    JsonWriter w{pretty};
    w.begin_object();
    w.field("schema", kCatalogSchema);
    w.field("schema_version", kSchemaVersion);
    w.key("catalog").begin_object();
    w.field("tool", "strategy_lab_replay");
    w.field("project_version", std::string{kVersion});

    w.key("limits").begin_object();
    w.field("default_max_rows", u64(kDefaultMaxRows));
    w.field("hard_max_rows", u64(kHardMaxRows));
    w.field("max_strategies", u64(kMaxStrategies));
    w.end_object();

    w.key("dataset_format").begin_object();
    w.field("id", kDatasetFormat);
    w.key("row_types").begin_array();
    w.string(domain::to_string(domain::MarketEventType::Bar));
    w.string(domain::to_string(domain::MarketEventType::Trade));
    w.end_array();
    w.key("columns").begin_array();
    for (const DatasetColumn& column : dataset_columns()) {
        w.begin_object();
        w.field("name", column.name);
        w.field("required", column.required);
        w.field("description", column.description);
        w.end_object();
    }
    w.end_array();
    w.end_object();

    w.key("bus_faults").begin_array();
    const std::pair<SynchronousDemoBus::Fault, std::string_view> faults[] = {
        {SynchronousDemoBus::Fault::None, "No fault: every signal is published."},
        {SynchronousDemoBus::Fault::RejectSignals,
         "The bus refuses every Signal event (publish returns false): the engine counts signals_rejected and the "
         "signal is lost, never re-sent."},
        {SynchronousDemoBus::Fault::ThrowOnSignal,
         "publish throws for every Signal event: the engine counts publish_errors and the signal is lost."}};
    for (const auto& [fault, description] : faults) {
        w.begin_object();
        w.field("name", fault_name(fault));
        w.field("description", description);
        w.end_object();
    }
    w.end_array();

    w.key("strategies").begin_array();
    for (const StrategyKind& kind : strategy_catalog()) {
        w.begin_object();
        w.field("kind", kind.kind);
        w.field("title", kind.title);
        w.field("summary", kind.summary);
        w.field("docs", kind.docs);
        w.field("available", kind.available);
        if (!kind.available) {
            w.field("unavailable_reason", kind.unavailable_reason);
        }
        w.key("parameters").begin_array();
        for (const ParamSpec& param : kind.params) {
            w.begin_object();
            w.field("name", param.name);
            w.field("type", type_name(param.type));
            w.field("required", param.required);
            if (param.default_value.has_value()) {
                w.key("default");
                write_param(w, *param.default_value);
            }
            w.field("constraint", param.constraint);
            w.end_object();
        }
        w.end_array();
        w.key("signal_metadata_keys").begin_array();
        for (const std::string& key : kind.signal_metadata_keys) {
            w.string(key);
        }
        w.end_array();
        w.key("decision_reasons").begin_array();
        for (const ReasonSpec& reason : kind.reasons) {
            w.begin_object();
            w.field("code", reason.code);
            w.field("verdict", reason.verdict);
            w.field("text", reason.text);
            w.end_object();
        }
        w.end_array();
        w.key("indicators").begin_array();
        for (const std::string& name : kind.indicators) {
            w.string(name);
        }
        w.end_array();
        w.key("states").begin_array();
        for (const std::string& name : kind.states) {
            w.string(name);
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    w.end_object();
    std::string text = w.take();
    text += '\n';
    return text;
}

std::string error_json(const LabError& error, bool pretty) {
    JsonWriter w{pretty};
    w.begin_object();
    w.field("schema", kErrorSchema);
    w.field("schema_version", kSchemaVersion);
    w.key("error").begin_object();
    w.field("code", code_name(error.code()));
    w.field("exit_status", exit_status(error.code()));
    w.field("message", to_valid_utf8(error.what()));
    w.field("total_problems", u64(error.total_problems()));
    w.key("problems").begin_array();
    for (const Problem& problem : error.problems()) {
        w.begin_object();
        if (problem.line != 0) {
            w.field("line", u64(problem.line));
        }
        if (!problem.column.empty()) {
            w.field("column", to_valid_utf8(problem.column));
        }
        if (!problem.where.empty()) {
            w.field("where", to_valid_utf8(problem.where));
        }
        w.field("message", to_valid_utf8(problem.message));
        w.end_object();
    }
    w.end_array();
    w.end_object();
    w.end_object();
    std::string text = w.take();
    text += '\n';
    return text;
}

}  // namespace trading_engine::lab
