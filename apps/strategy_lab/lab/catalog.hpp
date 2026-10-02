#pragma once

// -----------------------------------------------------------------------------
//  The strategy catalog: THE single authoritative mapping from a strategy kind and its
//  parameters to the real C++ configuration structs.
//
//  One table per kind (catalog.cpp) drives all three of: parsing "--param name=value"
//  into the config struct, building the strategy, and the `describe` document the UI
//  builds its forms from. Defaults are read from the DEFAULT-CONSTRUCTED config struct,
//  so they cannot drift from the strategies. Adding a parameter means adding one table
//  entry.
//
//  Validation has one source of truth: the strategy's constructor. The lab only parses
//  text into the right C++ type (and refuses text that is not that type); it never
//  re-states a range rule. The constructor's common::ConfigError text is passed through
//  verbatim, so "short_window=0" is rejected with the strategy's own words.
//
//  The custom ML strategy is listed as UNAVAILABLE (docs/STRATEGIES.md section 7) and
//  cannot be built.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "trading_engine/strategy/strategy.hpp"
#include "trading_engine/strategy/strategy_diagnostics.hpp"

namespace trading_engine::lab {

// At most this many strategies in one run (each is delivered every event).
inline constexpr std::size_t kMaxStrategies = 16;

enum class ParamType { String, UInt, Double, StringList };

[[nodiscard]] std::string_view type_name(ParamType type) noexcept;

using ParamValue   = std::variant<std::string, std::uint64_t, double, std::vector<std::string>>;
using DerivedValue = std::variant<bool, std::uint64_t, double>;

struct ParamSpec {
    std::string                name;
    ParamType                  type{ParamType::String};
    bool                       required{false};       // true: the strategy needs it (no usable default)
    std::string                constraint;            // human-readable; the constructor is the real check
    std::optional<ParamValue>  default_value;         // from the default-constructed config
};

// A decision code a strategy can report, with a plain-language meaning.
struct ReasonSpec {
    std::string code;      // equals strategy::to_string(BarReason)
    std::string verdict;   // equals strategy::to_string(BarVerdict)
    std::string text;
};

struct StrategyKind {
    std::string kind;
    std::string title;
    std::string summary;
    std::string docs;                              // repository-relative path
    bool        available{true};
    std::string unavailable_reason;                // set when !available
    std::vector<ParamSpec>   params;
    std::vector<std::string> signal_metadata_keys;
    std::vector<ReasonSpec>  reasons;
    std::vector<std::string> indicators;           // names a snapshot carries
    std::vector<std::string> states;
};

[[nodiscard]] const std::vector<StrategyKind>& strategy_catalog();
[[nodiscard]] const StrategyKind* find_kind(std::string_view kind);

// What the command line says about one strategy: its kind and "name=value" texts in order.
struct ParamAssignment {
    std::string name;
    std::string text;
};
struct StrategyRequest {
    std::string                  kind;
    std::vector<ParamAssignment> params;
};

// A constructed strategy and what the output needs to say about it.
struct BuiltStrategy {
    std::string kind;
    std::string strategy_id;
    std::shared_ptr<strategy::IStrategy> strategy;

    // EVERY parameter, in table order, defaults filled in.
    std::vector<std::pair<std::string, ParamValue>>   parameters;
    // Facts that follow from the parameters (documented formulas), for the UI.
    std::vector<std::pair<std::string, DerivedValue>> derived;

    std::vector<std::string> symbols;       // the allowlist
    std::size_t              window_size{0};   // bars needed before the first decision

    std::function<void(strategy::IStrategyObserver*)> set_observer;
    std::function<std::uint64_t()>                    observer_failures;
};

// `index` is the strategy's position on the command line, used only to locate errors.
// Throws LabError: InvalidParameter (unknown, repeated or unparsable parameter),
// UnavailableStrategy, or ConfigError (the strategy's own rejection, text verbatim).
[[nodiscard]] BuiltStrategy build_strategy(const StrategyRequest& request, std::size_t index = 0);

}  // namespace trading_engine::lab
