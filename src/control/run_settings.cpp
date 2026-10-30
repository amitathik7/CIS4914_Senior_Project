#include "trading_engine/control/run_settings.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

namespace trading_engine::control {

std::string_view to_string(StrategyKind kind) noexcept {
    switch (kind) {
        case StrategyKind::SmaCrossover:  return "sma_crossover";
        case StrategyKind::MeanReversion: return "mean_reversion";
    }
    return "invalid";
}

std::vector<common::Symbol> RunSettings::symbols() const {
    std::set<common::Symbol> all;
    for (const auto& s : strategies) {
        all.insert(s.symbols.begin(), s.symbols.end());
    }
    return {all.begin(), all.end()};
}

namespace {

std::string at(std::size_t index, std::string_view member = {}) {
    std::string path = "strategies." + std::to_string(index);
    if (!member.empty()) {
        path += '.';
        path += member;
    }
    return path;
}

void positive_money(const common::Money& value, const char* field, const char* what) {
    if (value <= common::Money{}) {
        throw SettingsError(field, std::string(what) + " must be more than zero.");
    }
}

}  // namespace

void validate(const RunSettings& s) {
    if (s.mode != config::RunMode::Backtest) {
        throw SettingsError("mode", "Only backtests can be started; live runs need the Alpaca feed.");
    }
    if (s.period_start && s.period_end && *s.period_end < *s.period_start) {
        throw SettingsError("period", "The last session is before the first one.");
    }
    if (s.strategies.empty()) {
        throw SettingsError("strategies", "Add at least one strategy.");
    }
    std::set<std::string> ids;
    for (std::size_t i = 0; i < s.strategies.size(); ++i) {
        const auto& st = s.strategies[i];
        if (st.strategy_id.empty()) {
            throw SettingsError(at(i, "strategy_id"), "Give the strategy an id.");
        }
        if (!ids.insert(st.strategy_id).second) {
            throw SettingsError(at(i, "strategy_id"), "Another strategy already uses the id '" + st.strategy_id + "'.");
        }
        if (st.symbols.empty()) {
            throw SettingsError(at(i, "symbols"), "Choose at least one symbol.");
        }
        if (st.requested_quantity <= 0) {
            throw SettingsError(at(i, "requested_quantity"), "The order size must be at least one share.");
        }
        if (st.kind == StrategyKind::SmaCrossover &&
            (st.short_window == 0 || st.short_window >= st.long_window)) {
            throw SettingsError(at(i, "long_window"), "The long window must be longer than the short one, and both above zero.");
        }
        if (st.kind == StrategyKind::MeanReversion && st.lookback < 2) {
            throw SettingsError(at(i, "lookback"), "The lookback must be at least 2 bars.");
        }
    }
    if (s.risk.max_order_quantity <= 0) {
        throw SettingsError("risk.max_order_quantity", "The maximum order size must be at least one share.");
    }
    positive_money(s.risk.max_position_notional, "risk.max_position_notional", "The position limit");
    positive_money(s.risk.max_gross_exposure, "risk.max_gross_exposure", "The gross exposure limit");
    positive_money(s.risk.max_daily_loss, "risk.max_daily_loss", "The daily loss limit");
    if (s.risk.max_open_orders == 0) {
        throw SettingsError("risk.max_open_orders", "Allow at least one open order.");
    }
    positive_money(s.simulation.starting_cash, "simulation.starting_cash", "Starting cash");
    if (!std::isfinite(s.simulation.slippage_bps) || s.simulation.slippage_bps < 0.0) {
        throw SettingsError("simulation.slippage_bps", "Slippage cannot be negative.");
    }
    if (s.simulation.fees.per_share < common::Money{}) {
        throw SettingsError("simulation.fees.per_share", "Fees cannot be negative.");
    }
    if (s.simulation.fees.per_trade < common::Money{} ||
        !std::isfinite(s.simulation.fees.bps) || s.simulation.fees.bps < 0.0) {
        throw SettingsError("simulation.fees", "Fees cannot be negative.");
    }
    (void)make_strategies(s);   // whatever else a strategy refuses
}

std::vector<std::shared_ptr<strategy::IStrategy>> make_strategies(const RunSettings& s) {
    std::vector<std::shared_ptr<strategy::IStrategy>> out;
    for (std::size_t i = 0; i < s.strategies.size(); ++i) {
        const auto& st = s.strategies[i];
        try {
            if (st.kind == StrategyKind::SmaCrossover) {
                strategy::MovingAverageCrossoverConfig c;
                c.strategy_id        = st.strategy_id;
                c.short_window       = st.short_window;
                c.long_window        = st.long_window;
                c.requested_quantity = st.requested_quantity;
                c.symbols            = st.symbols;
                out.push_back(std::make_shared<strategy::MovingAverageCrossoverStrategy>(std::move(c)));
            } else {
                strategy::MeanReversionConfig c;
                c.strategy_id        = st.strategy_id;
                c.lookback           = st.lookback;
                c.entry_threshold    = st.entry_threshold;
                c.rearm_threshold    = st.rearm_threshold;
                c.requested_quantity = st.requested_quantity;
                c.symbols            = st.symbols;
                out.push_back(std::make_shared<strategy::MeanReversionStrategy>(std::move(c)));
            }
        } catch (const common::ConfigError& e) {
            std::string message = e.what();
            constexpr std::string_view prefix = "configuration error: ";
            if (message.rfind(prefix, 0) == 0) {
                message.erase(0, prefix.size());
            }
            throw SettingsError(at(i), message);
        }
    }
    return out;
}

}  // namespace trading_engine::control
