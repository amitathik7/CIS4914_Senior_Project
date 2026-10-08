#include "lab/catalog.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

#include "lab/errors.hpp"
#include "lab/numbers.hpp"
#include "lab/utf8.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

namespace trading_engine::lab {

namespace {

using strategy::BarReason;
using strategy::BarVerdict;

// One configurable field of a config struct: how to set it from parsed text and how to
// read it back (for the defaults and for echoing the resolved configuration).
template <class Config>
struct ParamDef {
    const char* name;
    ParamType   type;
    bool        required;
    const char* constraint;
    void (*set)(Config&, const ParamValue&);
    ParamValue (*get)(const Config&);
};

// Text -> the C++ type of the parameter, or nullopt with `why`. Nothing is rounded: a
// value that does not fit the type is refused, not adjusted.
std::optional<ParamValue> parse_param_text(ParamType type, std::string_view text, std::string& why) {
    switch (type) {
        case ParamType::String:
            return ParamValue{std::string{text}};
        case ParamType::UInt:
        case ParamType::Quantity: {
            std::uint64_t value = 0;
            const char* first   = text.data();
            const char* last    = first + text.size();
            const auto result   = std::from_chars(first, last, value);
            if (text.empty() || result.ec == std::errc::invalid_argument || result.ptr != last) {
                why = "expected an unsigned whole number (digits only), got '" + utf8_excerpt(text, 40) + "'";
                return std::nullopt;
            }
            // A quantity is held as an int64: anything above INT64_MAX is refused here, never wrapped.
            const std::uint64_t largest =
                type == ParamType::Quantity ? static_cast<std::uint64_t>(std::numeric_limits<common::Quantity>::max())
                                            : static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
            if (result.ec == std::errc::result_out_of_range || value > largest) {
                why = "'" + utf8_excerpt(text, 40) + "' is too large";
                return std::nullopt;
            }
            return ParamValue{value};
        }
        case ParamType::Double: {
            double value = 0.0;
            std::string reason;
            if (!parse_double_text(text, value, reason)) {
                why = "'" + utf8_excerpt(text, 40) + "' " + reason;
                return std::nullopt;
            }
            return ParamValue{value};
        }
        case ParamType::StringList: {
            std::vector<std::string> items;
            if (!text.empty()) {   // "" is an empty list; "A,,B" keeps its empty entry for the strategy to reject
                std::size_t start = 0;
                while (true) {
                    const std::size_t comma = text.find(',', start);
                    if (comma == std::string_view::npos) {
                        items.emplace_back(text.substr(start));
                        break;
                    }
                    items.emplace_back(text.substr(start, comma - start));
                    start = comma + 1;
                }
            }
            return ParamValue{std::move(items)};
        }
    }
    why = "unsupported parameter type";
    return std::nullopt;
}

using SmaConfig = strategy::MovingAverageCrossoverConfig;
using MrConfig  = strategy::MeanReversionConfig;

const std::vector<ParamDef<SmaConfig>>& sma_params() {
    static const std::vector<ParamDef<SmaConfig>> defs{
        {"strategy_id", ParamType::String, false,
         "Not empty; unique among the strategies of one run. Becomes TradeSignal::strategy_id.",
         [](SmaConfig& c, const ParamValue& v) { c.strategy_id = std::get<std::string>(v); },
         [](const SmaConfig& c) -> ParamValue { return c.strategy_id; }},
        {"short_window", ParamType::UInt, false, "At least 1 and below long_window; counted in accepted bars.",
         [](SmaConfig& c, const ParamValue& v) { c.short_window = static_cast<std::size_t>(std::get<std::uint64_t>(v)); },
         [](const SmaConfig& c) -> ParamValue { return static_cast<std::uint64_t>(c.short_window); }},
        {"long_window", ParamType::UInt, false, "Greater than short_window; counted in accepted bars.",
         [](SmaConfig& c, const ParamValue& v) { c.long_window = static_cast<std::size_t>(std::get<std::uint64_t>(v)); },
         [](const SmaConfig& c) -> ParamValue { return static_cast<std::uint64_t>(c.long_window); }},
        {"requested_quantity", ParamType::Quantity, false,
         "Shares requested on every signal: a positive whole number (an exact integer count).",
         [](SmaConfig& c, const ParamValue& v) { c.requested_quantity = static_cast<common::Quantity>(std::get<std::uint64_t>(v)); },
         [](const SmaConfig& c) -> ParamValue { return static_cast<std::uint64_t>(c.requested_quantity); }},
        {"symbols", ParamType::StringList, true,
         "Comma-separated allowlist: at least one, no empty entry, no repeats; matched exactly and case-sensitively.",
         [](SmaConfig& c, const ParamValue& v) { c.symbols = std::get<std::vector<std::string>>(v); },
         [](const SmaConfig& c) -> ParamValue { return c.symbols; }},
    };
    return defs;
}

const std::vector<ParamDef<MrConfig>>& mr_params() {
    static const std::vector<ParamDef<MrConfig>> defs{
        {"strategy_id", ParamType::String, false,
         "Not empty; unique among the strategies of one run. Becomes TradeSignal::strategy_id.",
         [](MrConfig& c, const ParamValue& v) { c.strategy_id = std::get<std::string>(v); },
         [](const MrConfig& c) -> ParamValue { return c.strategy_id; }},
        {"lookback", ParamType::UInt, false, "At least 2; counted in accepted bars, the current one included.",
         [](MrConfig& c, const ParamValue& v) { c.lookback = static_cast<std::size_t>(std::get<std::uint64_t>(v)); },
         [](const MrConfig& c) -> ParamValue { return static_cast<std::uint64_t>(c.lookback); }},
        {"entry_threshold", ParamType::Double, false, "Finite and greater than rearm_threshold (population z-score).",
         [](MrConfig& c, const ParamValue& v) { c.entry_threshold = std::get<double>(v); },
         [](const MrConfig& c) -> ParamValue { return c.entry_threshold; }},
        {"rearm_threshold", ParamType::Double, false, "Finite, at least 0 and below entry_threshold.",
         [](MrConfig& c, const ParamValue& v) { c.rearm_threshold = std::get<double>(v); },
         [](const MrConfig& c) -> ParamValue { return c.rearm_threshold; }},
        {"requested_quantity", ParamType::Quantity, false,
         "Shares requested on every signal: a positive whole number (an exact integer count).",
         [](MrConfig& c, const ParamValue& v) { c.requested_quantity = static_cast<common::Quantity>(std::get<std::uint64_t>(v)); },
         [](const MrConfig& c) -> ParamValue { return static_cast<std::uint64_t>(c.requested_quantity); }},
        {"symbols", ParamType::StringList, true,
         "Comma-separated allowlist: at least one, no empty entry, no repeats; matched exactly and case-sensitively.",
         [](MrConfig& c, const ParamValue& v) { c.symbols = std::get<std::vector<std::string>>(v); },
         [](const MrConfig& c) -> ParamValue { return c.symbols; }},
    };
    return defs;
}

template <class Config>
std::vector<ParamSpec> specs_of(const std::vector<ParamDef<Config>>& defs) {
    const Config defaults{};   // the real defaults, straight from the strategy's config struct
    std::vector<ParamSpec> out;
    for (const ParamDef<Config>& def : defs) {
        ParamSpec spec;
        spec.name       = def.name;
        spec.type       = def.type;
        spec.required   = def.required;
        spec.constraint = def.constraint;
        if (!def.required) {
            spec.default_value = def.get(defaults);
        }
        out.push_back(std::move(spec));
    }
    return out;
}

ReasonSpec reason(BarReason code, BarVerdict verdict, std::string text) {
    return ReasonSpec{std::string{strategy::to_string(code)}, std::string{strategy::to_string(verdict)},
                      std::move(text)};
}

// The ignore checks, in the order both strategies apply them.
std::vector<ReasonSpec> ignore_reasons(bool with_price_ceiling) {
    std::vector<ReasonSpec> out;
    out.push_back(reason(BarReason::NotABar, BarVerdict::Ignored,
                         "The event is not a bar (a trade, quote or status); only bars carry a close."));
    out.push_back(reason(BarReason::SymbolNotAllowlisted, BarVerdict::Ignored,
                         "The symbol is not in this strategy's symbols list (matching is exact and case-sensitive)."));
    out.push_back(reason(BarReason::PriceAbsent, BarVerdict::Ignored, "The event has no price."));
    out.push_back(reason(BarReason::PriceInvalid, BarVerdict::Ignored,
                         "The price is zero or negative."));
    if (with_price_ceiling) {
        out.push_back(reason(BarReason::PriceAboveMaxClose, BarVerdict::Ignored,
                             "The price is above the largest close this strategy accepts "
                             "(INT64_MAX / long_window^2, in 1e-6 units), so the exact integer sums could overflow."));
    }
    out.push_back(reason(BarReason::TimeNotAfterLastAccepted, BarVerdict::Ignored,
                         "exchange_time is not later than the last accepted bar's for this symbol "
                         "(a duplicate, a replay or an older bar)."));
    return out;
}

StrategyKind sma_kind() {
    StrategyKind kind;
    kind.kind    = "sma_crossover";
    kind.title   = "Moving-average crossover";
    kind.summary = "Requests a Buy when the short simple moving average crosses above the long one, and a Sell when it "
                   "crosses below. Tracks no position and sizes nothing.";
    kind.docs    = "docs/strategies/moving_average_crossover.md";
    kind.params  = specs_of(sma_params());
    kind.signal_metadata_keys = {"long_sma", "long_window", "short_sma", "short_window", "trigger"};
    kind.reasons = ignore_reasons(true);
    kind.reasons.push_back(reason(BarReason::WarmingUp, BarVerdict::WarmingUp,
                                  "Fewer than long_window bars have been accepted for this symbol, so no averages exist yet."));
    kind.reasons.push_back(reason(BarReason::BaselineEstablished, BarVerdict::Evaluated,
                                  "The first bar after warm-up where the averages differ: its side is recorded as the "
                                  "baseline and never signalled."));
    kind.reasons.push_back(reason(BarReason::AveragesEqual, BarVerdict::Evaluated,
                                  "The averages are exactly equal (compared as integers, no tolerance): no signal, and "
                                  "the last nonzero relation stands."));
    kind.reasons.push_back(reason(BarReason::SameSide, BarVerdict::Evaluated,
                                  "The short average is still on the same side of the long average; a crossover "
                                  "signals once."));
    kind.reasons.push_back(reason(BarReason::CrossoverBuy, BarVerdict::Evaluated,
                                  "The short average crossed above the long one: a Buy request was emitted."));
    kind.reasons.push_back(reason(BarReason::CrossoverSell, BarVerdict::Evaluated,
                                  "The short average crossed below the long one: a Sell request was emitted."));
    kind.indicators = {"short_sma", "long_sma", "short_minus_long"};
    kind.states     = {"relation_before", "relation_now", "relation_after"};
    return kind;
}

StrategyKind mr_kind() {
    StrategyKind kind;
    kind.kind    = "mean_reversion";
    kind.title   = "Mean reversion (rolling z-score)";
    kind.summary = "Requests a Buy when a close is far below the rolling mean and a Sell when far above, once per "
                   "excursion (a latch rearms near the mean). Has no exit logic and tracks no position.";
    kind.docs    = "docs/strategies/mean_reversion.md";
    kind.params  = specs_of(mr_params());
    kind.signal_metadata_keys = {"close",          "entry_threshold", "lookback",       "mean",
                                 "rearm_threshold", "standard_deviation", "trigger", "z_score"};
    kind.reasons = ignore_reasons(false);
    kind.reasons.push_back(reason(BarReason::WarmingUp, BarVerdict::WarmingUp,
                                  "Fewer than lookback bars have been accepted for this symbol, so there is no full window."));
    kind.reasons.push_back(reason(BarReason::MeasurementFailed, BarVerdict::Evaluated,
                                  "A statistic came out non-finite (a guard no known input reaches): nothing emitted and "
                                  "the latch is unchanged."));
    kind.reasons.push_back(reason(BarReason::ConstantWindow, BarVerdict::Evaluated,
                                  "The window's deviation is negligible (at most 1e-12 of the mean), so z is not meaningful: "
                                  "nothing emitted and the latch rearms to neutral."));
    kind.reasons.push_back(reason(BarReason::EntryBuy, BarVerdict::Evaluated,
                                  "z <= -entry_threshold and the lower extreme had not been requested: a Buy request "
                                  "was emitted."));
    kind.reasons.push_back(reason(BarReason::EntrySell, BarVerdict::Evaluated,
                                  "z >= +entry_threshold and the upper extreme had not been requested: a Sell request "
                                  "was emitted."));
    kind.reasons.push_back(reason(BarReason::ExcursionAlreadyRequested, BarVerdict::Evaluated,
                                  "z is beyond an entry threshold on the side already requested; a request repeats only "
                                  "after the latch rearms."));
    kind.reasons.push_back(reason(BarReason::InsideRearmBand, BarVerdict::Evaluated,
                                  "abs(z) <= rearm_threshold: the latch rearms to neutral and nothing is emitted."));
    kind.reasons.push_back(reason(BarReason::BetweenBands, BarVerdict::Evaluated,
                                  "z is between the rearm and entry thresholds: nothing emitted, latch unchanged."));
    kind.indicators = {"mean", "standard_deviation", "z_score"};
    kind.states     = {"latch_before", "latch_after"};
    return kind;
}

StrategyKind ml_kind() {
    StrategyKind kind;
    kind.kind               = "ml";
    kind.title              = "Custom ML strategy";
    kind.summary            = "Not available.";
    kind.docs               = "docs/STRATEGIES.md";
    kind.available          = false;
    kind.unavailable_reason = "Not implemented: the custom ML strategy has not been started "
                              "(docs/STRATEGIES.md, section 7), so the lab offers no ML behaviour and invents none.";
    return kind;
}

std::string known_names(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += out.empty() ? "" : ", ";
        out += name;
    }
    return out;
}

template <class Strategy, class Config>
BuiltStrategy build_kind(const std::string& kind, const std::vector<ParamDef<Config>>& defs,
                         const StrategyRequest& request, std::size_t index,
                         std::size_t (*window_of)(const Config&),
                         std::vector<std::pair<std::string, DerivedValue>> (*derive)(const Config&)) {
    const std::string where = "strategies[" + std::to_string(index) + "]";
    const std::string label = "strategy #" + std::to_string(index + 1) + " (" + kind + ")";

    std::vector<std::string> names;
    for (const ParamDef<Config>& def : defs) {
        names.emplace_back(def.name);
    }

    Config config{};
    std::vector<std::string> seen;
    for (const ParamAssignment& assignment : request.params) {
        if (!is_valid_utf8(assignment.name) || !is_valid_utf8(assignment.text)) {
            throw LabError(ErrorCode::InvalidParameter, "a parameter of " + label + " is not valid UTF-8",
                           {Problem{0, "", where, "not valid UTF-8"}}, 1);
        }
        const auto def = std::find_if(defs.begin(), defs.end(), [&](const ParamDef<Config>& d) {
            return assignment.name == d.name;
        });
        if (def == defs.end()) {
            throw LabError(ErrorCode::InvalidParameter,
                           "unknown parameter '" + utf8_excerpt(assignment.name, 40) + "' for " + label +
                               " (known: " + known_names(names) + ")",
                           {Problem{0, assignment.name, where, "unknown parameter"}}, 1);
        }
        if (std::find(seen.begin(), seen.end(), assignment.name) != seen.end()) {
            throw LabError(ErrorCode::InvalidParameter,
                           "parameter '" + assignment.name + "' was given more than once for " + label,
                           {Problem{0, assignment.name, where, "given more than once"}}, 1);
        }
        seen.push_back(assignment.name);

        std::string why;
        const std::optional<ParamValue> value = parse_param_text(def->type, assignment.text, why);
        if (!value.has_value()) {
            throw LabError(ErrorCode::InvalidParameter,
                           "parameter '" + assignment.name + "' of " + label + ": " + why,
                           {Problem{0, assignment.name, where, why}}, 1);
        }
        def->set(config, *value);
    }

    std::shared_ptr<Strategy> built_strategy;
    try {
        built_strategy = std::make_shared<Strategy>(config);
    } catch (const common::ConfigError& error) {
        // The strategy's own words, unchanged.
        throw LabError(ErrorCode::ConfigError, error.what(), {Problem{0, "", where, error.what()}}, 1);
    }

    BuiltStrategy built;
    built.kind        = kind;
    built.strategy_id = config.strategy_id;
    built.strategy    = built_strategy;
    for (const ParamDef<Config>& def : defs) {
        built.parameters.emplace_back(def.name, def.get(config));
    }
    built.derived     = derive(config);
    built.symbols     = config.symbols;
    built.window_size = window_of(config);
    built.set_observer = [built_strategy](strategy::IStrategyObserver* observer) {
        built_strategy->set_observer(observer);
    };
    built.observer_failures = [built_strategy]() { return built_strategy->observer_failures(); };
    return built;
}

}  // namespace

std::string_view type_name(ParamType type) noexcept {
    switch (type) {
        case ParamType::String:     return "string";
        case ParamType::UInt:       return "uint";
        case ParamType::Quantity:   return "uint";
        case ParamType::Double:     return "double";
        case ParamType::StringList: return "string_list";
    }
    return "string";
}

const std::vector<StrategyKind>& strategy_catalog() {
    static const std::vector<StrategyKind> catalog{sma_kind(), mr_kind(), ml_kind()};
    return catalog;
}

const StrategyKind* find_kind(std::string_view kind) {
    const auto& catalog = strategy_catalog();
    const auto found = std::find_if(catalog.begin(), catalog.end(),
                                    [kind](const StrategyKind& k) { return k.kind == kind; });
    return found == catalog.end() ? nullptr : &*found;
}

BuiltStrategy build_strategy(const StrategyRequest& request, std::size_t index) {
    const StrategyKind* kind = find_kind(request.kind);
    if (kind == nullptr) {
        std::string names;
        for (const StrategyKind& each : strategy_catalog()) {
            names += names.empty() ? "" : ", ";
            names += each.kind;
        }
        throw LabError(ErrorCode::UsageError,
                       "unknown strategy '" + utf8_excerpt(request.kind, 40) + "' (known: " + names + ")",
                       {Problem{0, "", "strategies[" + std::to_string(index) + "]", "unknown strategy"}}, 1);
    }
    if (!kind->available) {
        throw LabError(ErrorCode::UnavailableStrategy,
                       "strategy '" + kind->kind + "' is not available: " + kind->unavailable_reason,
                       {Problem{0, "", "strategies[" + std::to_string(index) + "]", kind->unavailable_reason}}, 1);
    }

    if (kind->kind == "sma_crossover") {
        return build_kind<strategy::MovingAverageCrossoverStrategy, SmaConfig>(
            kind->kind, sma_params(), request, index,
            [](const SmaConfig& c) { return c.long_window; },
            [](const SmaConfig& c) {
                return std::vector<std::pair<std::string, DerivedValue>>{
                    {"earliest_signal_accepted_bar", static_cast<std::uint64_t>(c.long_window + 1)}};
            });
    }
    // mean_reversion
    return build_kind<strategy::MeanReversionStrategy, MrConfig>(
        kind->kind, mr_params(), request, index, [](const MrConfig& c) { return c.lookback; },
        [](const MrConfig& c) {
            // |z| <= sqrt(N - 1) always (docs/strategies/mean_reversion.md section 1): a threshold above it
            // is accepted and can never fire.
            const double bound = std::sqrt(static_cast<double>(c.lookback) - 1.0);
            return std::vector<std::pair<std::string, DerivedValue>>{
                {"max_abs_z", bound}, {"entry_threshold_reachable", c.entry_threshold <= bound}};
        });
}

}  // namespace trading_engine::lab
