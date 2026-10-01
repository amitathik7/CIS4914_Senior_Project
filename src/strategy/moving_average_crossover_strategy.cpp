#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "config_checks.hpp"

namespace trading_engine::strategy {

namespace {

// Two averages whose gap is at most this fraction of the larger one are EQUAL.
// That is about 4,500 times a double's rounding step: far above the few ulps the
// compensated sums leave behind, far below any gap worth acting on. Fixed on
// purpose; see "Floating point" in the header.
constexpr double kEqualityTolerance = 1e-12;

constexpr std::string_view kName = "MovingAverageCrossoverStrategy";

using detail::format_number;

[[noreturn]] void reject(const std::string& problem) {
    detail::reject(kName, problem);
}

// Every check runs before anything is built, so a rejected config constructs
// nothing. Nothing is rounded, clamped or defaulted on the caller's behalf.
MovingAverageCrossoverConfig validated(MovingAverageCrossoverConfig config) {
    if (config.strategy_id.empty()) {
        reject("strategy_id must not be empty");
    }
    if (config.short_window == 0) {
        reject("short_window must be at least 1 (got 0)");
    }
    if (config.long_window <= config.short_window) {
        reject("long_window must be greater than short_window (got long_window " +
               std::to_string(config.long_window) + ", short_window " +
               std::to_string(config.short_window) + ")");
    }

    detail::require_whole_share_quantity(kName, config.requested_quantity);
    detail::require_symbol_allowlist(kName, config.symbols);
    return config;
}

}  // namespace

// --- RollingSum / SymbolState ---------------------------------------------------

void MovingAverageCrossoverStrategy::RollingSum::add(double term) noexcept {
    const double next_total = total + term;
    // The operand with the larger magnitude keeps its low-order bits in the sum;
    // the smaller one loses some. Recover exactly what was lost.
    if (std::fabs(total) >= std::fabs(term)) {
        lost += (total - next_total) + term;
    } else {
        lost += (term - next_total) + total;
    }
    total = next_total;
}

void MovingAverageCrossoverStrategy::SymbolState::reset() noexcept {
    closes.clear();   // keeps the capacity, so a restart does not reallocate
    next       = 0;
    short_sum  = RollingSum{};
    long_sum   = RollingSum{};
    last_time.reset();
    relation.reset();
}

// --- Strategy -------------------------------------------------------------------

MovingAverageCrossoverStrategy::MovingAverageCrossoverStrategy(MovingAverageCrossoverConfig config)
    : config_{validated(std::move(config))},
      max_close_{std::numeric_limits<double>::max() /
                 (2.0 * (static_cast<double>(config_.long_window) + 1.0))} {
    states_.reserve(config_.symbols.size());
    for (const common::Symbol& symbol : config_.symbols) {
        states_.emplace(symbol, SymbolState{});
    }
}

void MovingAverageCrossoverStrategy::on_start() {
    for (auto& entry : states_) {
        entry.second.reset();
    }
}

void MovingAverageCrossoverStrategy::on_market_event(const domain::MarketEvent& event,
                                                     ISignalSink& out) {
    // Every check below runs before any state is touched, so an ignored event
    // leaves the timestamps, histories, averages and crossover state as they were.
    if (event.type != domain::MarketEventType::Bar) {
        return;
    }
    const auto found = states_.find(event.symbol);
    if (found == states_.end()) {
        return;
    }
    if (!event.price.has_value()) {
        return;
    }
    const double close = *event.price;
    if (!std::isfinite(close) || close <= 0.0 || close > max_close_) {
        return;
    }
    SymbolState& state = found->second;
    if (state.last_time.has_value() && event.exchange_time <= *state.last_time) {
        return;   // duplicate, replay or out of order: strict increase is required
    }

    accept_close(state, close);
    state.last_time = event.exchange_time;
    if (state.closes.size() < config_.long_window) {
        return;   // still warming up: both windows must be full
    }

    const double short_average = state.short_sum.value() / static_cast<double>(config_.short_window);
    const double long_average  = state.long_sum.value() / static_cast<double>(config_.long_window);
    const std::optional<Relation> now = relate(short_average, long_average);
    if (!now.has_value()) {
        return;   // equal: no signal, and the last nonzero relationship stands
    }

    // The first nonzero relationship is only a baseline; after that, a change of
    // side is a crossover. The signal is built before the state moves (building
    // may throw) and emitted after it (a lost signal is never re-sent).
    std::optional<domain::TradeSignal> signal;
    if (state.relation.has_value() && *state.relation != *now) {
        signal = make_signal(event, *now, short_average, long_average);
    }
    state.relation = now;
    if (signal.has_value()) {
        out.emit(*signal);
    }
}

void MovingAverageCrossoverStrategy::accept_close(SymbolState& state, common::Price close) {
    const std::size_t short_window = config_.short_window;
    const std::size_t long_window  = config_.long_window;
    std::vector<common::Price>& closes = state.closes;

    if (closes.size() < long_window) {
        // Warming up: the history is a plain oldest-first list and nothing has
        // left the long window yet.
        closes.push_back(close);   // may throw; nothing has been modified so far
        const std::size_t count = closes.size();
        state.long_sum.add(close);
        state.short_sum.add(close);
        if (count > short_window) {
            // The close short_window bars back has just left the short window.
            state.short_sum.add(-closes[count - 1 - short_window]);
        }
        return;
    }

    // Full: closes is a ring and closes[next] is the oldest close, the one this
    // bar replaces. The close leaving the short window is the short_window-th
    // most recent one before this bar, which sits short_window slots behind the
    // slot being written.
    const double leaves_long  = closes[state.next];
    const double leaves_short = closes[(state.next + long_window - short_window) % long_window];
    closes[state.next] = close;
    state.next = (state.next + 1) % long_window;

    state.long_sum.add(close);
    state.long_sum.add(-leaves_long);
    state.short_sum.add(close);
    state.short_sum.add(-leaves_short);
}

std::optional<MovingAverageCrossoverStrategy::Relation>
MovingAverageCrossoverStrategy::relate(double short_average, double long_average) noexcept {
    const double gap   = short_average - long_average;
    const double scale = std::max(std::fabs(short_average), std::fabs(long_average));
    if (std::fabs(gap) <= kEqualityTolerance * scale) {
        return std::nullopt;
    }
    return gap > 0.0 ? Relation::ShortAboveLong : Relation::ShortBelowLong;
}

domain::TradeSignal MovingAverageCrossoverStrategy::make_signal(const domain::MarketEvent& event,
                                                                Relation now,
                                                                double short_average,
                                                                double long_average) const {
    const bool buy = now == Relation::ShortAboveLong;

    // id, created_at and strategy_id are the engine's to set (see ISignalSink).
    domain::TradeSignal signal;
    signal.symbol             = event.symbol;
    signal.side               = buy ? domain::SignalSide::Buy : domain::SignalSide::Sell;
    signal.requested_quantity = config_.requested_quantity;
    signal.order_type         = domain::OrderType::Market;
    signal.metadata = {
        {"trigger", buy ? "short_crossed_above_long" : "short_crossed_below_long"},
        {"short_window", std::to_string(config_.short_window)},
        {"long_window", std::to_string(config_.long_window)},
        {"short_sma", format_number(short_average)},
        {"long_sma", format_number(long_average)},
    };
    return signal;
}

}  // namespace trading_engine::strategy
