#pragma once

// -----------------------------------------------------------------------------
//  RunSettings -- everything that decides how one run behaves.
//
//  Responsibility: the one description of a run's configuration that travels
//  over the command channel (docs/adr/0007-engine-command-channel.md). The
//  console sends it to start a backtest; the engine publishes the settings it
//  actually used on <prefix>.run, so the console shows real limits and
//  strategy parameters instead of defaults. Its JSON form is the console's
//  RunConfig (web/src/api/contract.ts); the codec is control_codec.hpp.
//
//  `trading_engine run` builds one from its flags; `trading_engine serve`
//  receives one in a start_backtest command. Either way make_strategies()
//  and the configs below turn it into components.
//
//  Plain value type.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/errors.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/configuration/engine_config.hpp"

namespace trading_engine::strategy { class IStrategy; }

namespace trading_engine::control {

enum class StrategyKind : std::uint8_t { SmaCrossover = 0, MeanReversion };

[[nodiscard]] std::string_view to_string(StrategyKind kind) noexcept;   // "sma_crossover", ...

struct StrategySettings {
    StrategyKind                kind{StrategyKind::SmaCrossover};
    std::string                 strategy_id{};
    std::vector<common::Symbol> symbols{};
    common::Quantity            requested_quantity{1};
    // sma_crossover
    std::size_t short_window{5};
    std::size_t long_window{20};
    // mean_reversion
    std::size_t lookback{20};
    double      entry_threshold{2.0};
    double      rearm_threshold{0.5};
};

struct RunSettings {
    std::string                      name{};
    config::RunMode                  mode{config::RunMode::Backtest};
    std::optional<common::Timestamp> period_start{};   // unset: from the first bar
    std::optional<common::Timestamp> period_end{};     // unset: to the last bar
    std::vector<StrategySettings>    strategies{};
    config::RiskLimits               risk{};
    bool                             risk_policies{true};   // false: buying power / no-short only
    config::SimulationConfig         simulation{};
    double                           participation_cap{1.0};   // echoed; the fill model ignores it

    // Every symbol any strategy trades, sorted, without duplicates.
    [[nodiscard]] std::vector<common::Symbol> symbols() const;
};

// A setting the engine cannot run with. `field` is a dotted path into the
// RunConfig JSON ("strategies.0.long_window"), as the console's form expects.
class SettingsError : public common::ValidationError {
public:
    SettingsError(std::string field, std::string message)
        : common::ValidationError(field + ": " + message), field_{std::move(field)},
          message_{std::move(message)} {}
    [[nodiscard]] const std::string& field() const noexcept { return field_; }
    // Written for the person filling in the form, without the field path.
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    std::string field_;
    std::string message_;
};

// Checks what the components would refuse, and what they cannot do yet
// (short selling, live runs). Throws SettingsError naming the first problem.
void validate(const RunSettings& settings);

// One strategy instance per entry, in order. Throws SettingsError
// ("strategies.N") when a strategy refuses its configuration.
[[nodiscard]] std::vector<std::shared_ptr<strategy::IStrategy>> make_strategies(const RunSettings& settings);

}  // namespace trading_engine::control
