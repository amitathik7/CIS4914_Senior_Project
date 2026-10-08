#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "config_checks.hpp"
#include "trading_engine/common/decimal.hpp"

namespace trading_engine::strategy {

namespace {

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

// The largest close for which every sum and both cross-products stay inside int64:
// a sum is at most long_window closes and a product multiplies it by a window, so
// the bound is INT64_MAX / long_window^2 (divided twice: the square itself could
// overflow for an absurd window). The division is UNSIGNED on purpose: a long_window of
// 2^63 or more cast to int64 would be negative and give a huge, wrong bound. A window of
// about 3.04 million or more gives a bound under one currency unit and is refused by the
// constructor.
common::Price max_close_for(std::size_t long_window) noexcept {
    const auto window  = static_cast<std::uint64_t>(long_window);
    const auto largest = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return common::Price::from_micros(static_cast<std::int64_t>(largest / window / window));
}

}  // namespace

// --- SymbolState ----------------------------------------------------------------

void MovingAverageCrossoverStrategy::SymbolState::reset() noexcept {
    closes.clear();   // keeps the capacity, so a restart does not reallocate
    next       = 0;
    short_sum  = common::Price{};
    long_sum   = common::Price{};
    last_time.reset();
    relation.reset();
}

// --- Strategy -------------------------------------------------------------------

MovingAverageCrossoverStrategy::MovingAverageCrossoverStrategy(MovingAverageCrossoverConfig config)
    : config_{validated(std::move(config))},
      max_close_{max_close_for(config_.long_window)} {
    if (max_close_ < common::Price::from_units(1)) {
        reject("long_window " + std::to_string(config_.long_window) +
               " is too large for exact arithmetic: the sums would overflow for any close of 1 or more");
    }
    states_.reserve(config_.symbols.size());
    for (const common::Symbol& symbol : config_.symbols) {
        states_.emplace(symbol, SymbolState{});
    }
}

void MovingAverageCrossoverStrategy::on_start() {
    diagnostics_.refuse_inside_callback("on_start");
    for (auto& entry : states_) {
        entry.second.reset();
    }
}

void MovingAverageCrossoverStrategy::on_market_event(const domain::MarketEvent& event,
                                                     ISignalSink& out) {
    diagnostics_.refuse_inside_callback("on_market_event");

    // Every check below runs before any state is touched, so an ignored event
    // leaves the timestamps, histories, averages and crossover state as they were.
    if (event.type != domain::MarketEventType::Bar) {
        report_ignored(event, nullptr, BarReason::NotABar);
        return;
    }
    const auto found = states_.find(event.symbol);
    if (found == states_.end()) {
        report_ignored(event, nullptr, BarReason::SymbolNotAllowlisted);
        return;
    }
    SymbolState& state = found->second;
    if (!event.price.has_value()) {
        report_ignored(event, &state, BarReason::PriceAbsent);
        return;
    }
    const common::Price close = *event.price;
    if (close <= common::Price{} || close > max_close_) {
        report_ignored(event, &state,
                       close > common::Price{} ? BarReason::PriceAboveMaxClose : BarReason::PriceInvalid);
        return;
    }
    if (state.last_time.has_value() && event.exchange_time <= *state.last_time) {
        // duplicate, replay or out of order: strict increase is required
        report_ignored(event, &state, BarReason::TimeNotAfterLastAccepted);
        return;
    }

    accept_close(state, close);
    state.last_time = event.exchange_time;
    if (state.closes.size() < config_.long_window) {
        report_warming_up(event, state);   // still warming up: both windows must be full
        return;
    }

    // The decision is exact integer arithmetic; the averages are derived numbers for the report.
    const double short_average = average(state.short_sum, config_.short_window);
    const double long_average  = average(state.long_sum, config_.long_window);
    const std::optional<Relation> now = relate(state);
    if (!now.has_value()) {
        // equal: no signal, and the last nonzero relationship stands
        report_evaluated(event, state, BarReason::AveragesEqual, BarAction::None, state.relation,
                         now, short_average, long_average);
        return;
    }

    // The first nonzero relationship is only a baseline; after that, a change of
    // side is a crossover. The signal is built before the state moves (building
    // may throw) and emitted after it (a lost signal is never re-sent).
    std::optional<domain::TradeSignal> signal;
    if (state.relation.has_value() && *state.relation != *now) {
        signal = make_signal(event, *now, short_average, long_average);
    }
    const std::optional<Relation> before = state.relation;
    state.relation = now;

    BarReason reason = BarReason::SameSide;
    BarAction action = BarAction::None;
    if (!before.has_value()) {
        reason = BarReason::BaselineEstablished;
    } else if (signal.has_value()) {
        const bool buy = *now == Relation::ShortAboveLong;
        reason = buy ? BarReason::CrossoverBuy : BarReason::CrossoverSell;
        action = buy ? BarAction::Buy : BarAction::Sell;
    }
    report_evaluated(event, state, reason, action, before, now, short_average, long_average);
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
        state.long_sum += close;
        state.short_sum += close;
        if (count > short_window) {
            // The close short_window bars back has just left the short window.
            state.short_sum -= closes[count - 1 - short_window];
        }
        return;
    }

    // Full: closes is a ring and closes[next] is the oldest close, the one this
    // bar replaces. The close leaving the short window is the short_window-th
    // most recent one before this bar, which sits short_window slots behind the
    // slot being written.
    const common::Price leaves_long  = closes[state.next];
    const common::Price leaves_short = closes[(state.next + long_window - short_window) % long_window];
    closes[state.next] = close;
    state.next = (state.next + 1) % long_window;

    state.long_sum += close - leaves_long;
    state.short_sum += close - leaves_short;
}

std::optional<MovingAverageCrossoverStrategy::Relation>
MovingAverageCrossoverStrategy::relate(const SymbolState& state) const noexcept {
    // short_sum / short_window  versus  long_sum / long_window, without dividing: both
    // sides are multiplied by the product of the windows. Both products fit int64 (see
    // max_close_), so this is exact.
    const common::Price above = state.short_sum * static_cast<std::int64_t>(config_.long_window);
    const common::Price below = state.long_sum * static_cast<std::int64_t>(config_.short_window);
    if (above == below) {
        return std::nullopt;
    }
    return above > below ? Relation::ShortAboveLong : Relation::ShortBelowLong;
}

double MovingAverageCrossoverStrategy::average(common::Price sum, std::size_t window) noexcept {
    // One division of two exact values (the integer sum of millionths, and window * 10^6), so the
    // result is the double nearest the true average: no second rounding from a premature /10^6.
    return static_cast<double>(sum.micros()) /
           (static_cast<double>(window) * static_cast<double>(common::Decimal::kScale));
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

// --- Diagnostics ----------------------------------------------------------------
// Read-only: each builds a snapshot from values the decision already holds, and only
// when an observer is attached. None changes strategy state.

std::string_view MovingAverageCrossoverStrategy::relation_label(
    std::optional<Relation> relation) noexcept {
    if (!relation.has_value()) {
        return "none";
    }
    return *relation == Relation::ShortAboveLong ? "short_above_long" : "short_below_long";
}

void MovingAverageCrossoverStrategy::report_ignored(const domain::MarketEvent& event,
                                                    const SymbolState* state,
                                                    BarReason reason) noexcept {
    diagnostics_.notify(event, [&] {
        BarSnapshot snapshot;
        snapshot.strategy_id = config_.strategy_id;
        snapshot.verdict     = BarVerdict::Ignored;
        snapshot.reason      = reason;
        snapshot.window_size = config_.long_window;
        snapshot.add_indicator("short_sma", std::nullopt);
        snapshot.add_indicator("long_sma", std::nullopt);
        snapshot.add_indicator("short_minus_long", std::nullopt);
        if (state != nullptr) {
            snapshot.window_fill = state->closes.size();
            snapshot.add_state("relation_before", relation_label(state->relation));
            snapshot.add_state("relation_now", "not_evaluated");
            snapshot.add_state("relation_after", relation_label(state->relation));
        }
        return snapshot;
    });
}

void MovingAverageCrossoverStrategy::report_warming_up(const domain::MarketEvent& event,
                                                       const SymbolState& state) noexcept {
    diagnostics_.notify(event, [&] {
        BarSnapshot snapshot;
        snapshot.strategy_id = config_.strategy_id;
        snapshot.verdict     = BarVerdict::WarmingUp;
        snapshot.reason      = BarReason::WarmingUp;
        snapshot.window_fill = state.closes.size();
        snapshot.window_size = config_.long_window;
        snapshot.add_indicator("short_sma", std::nullopt);
        snapshot.add_indicator("long_sma", std::nullopt);
        snapshot.add_indicator("short_minus_long", std::nullopt);
        snapshot.add_state("relation_before", relation_label(state.relation));
        snapshot.add_state("relation_now", "not_evaluated");
        snapshot.add_state("relation_after", relation_label(state.relation));
        return snapshot;
    });
}

void MovingAverageCrossoverStrategy::report_evaluated(
    const domain::MarketEvent& event, const SymbolState& state, BarReason reason,
    BarAction action, std::optional<Relation> before, std::optional<Relation> now,
    double short_average, double long_average) noexcept {
    diagnostics_.notify(event, [&] {
        BarSnapshot snapshot;
        snapshot.strategy_id = config_.strategy_id;
        snapshot.verdict     = BarVerdict::Evaluated;
        snapshot.reason      = reason;
        snapshot.action      = action;
        snapshot.window_fill = state.closes.size();
        snapshot.window_size = config_.long_window;
        snapshot.add_indicator("short_sma", short_average);
        snapshot.add_indicator("long_sma", long_average);
        snapshot.add_indicator("short_minus_long", short_average - long_average);
        snapshot.add_state("relation_before", relation_label(before));
        snapshot.add_state("relation_now", now.has_value() ? relation_label(now) : "equal");
        snapshot.add_state("relation_after", relation_label(state.relation));
        return snapshot;
    });
}

}  // namespace trading_engine::strategy
