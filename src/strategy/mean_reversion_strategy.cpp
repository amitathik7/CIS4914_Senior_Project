#include "trading_engine/strategy/mean_reversion_strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "config_checks.hpp"
#include "trading_engine/common/decimal.hpp"
#include "trading_engine/common/decimal_text.hpp"

namespace trading_engine::strategy {

namespace {

constexpr std::string_view kName = "MeanReversionStrategy";

using detail::format_number;

// A window is CONSTANT when its standard deviation is at most this fraction of
// its mean: a spread under one part in 10^12 (one micro at a price of about a
// million currency units) is treated as no deviation. Fixed on purpose; see
// "Numbers" in the header.
constexpr double kNegligibleVariation = 1e-12;

[[noreturn]] void reject(const std::string& problem) {
    detail::reject(kName, problem);
}

// Every check runs before anything is built, so a rejected config constructs
// nothing.
MeanReversionConfig validated(MeanReversionConfig config) {
    if (config.strategy_id.empty()) {
        reject("strategy_id must not be empty");
    }
    if (config.lookback < 2) {
        reject("lookback must be at least 2 (got " + std::to_string(config.lookback) + ")");
    }
    if (!std::isfinite(config.entry_threshold)) {
        reject("entry_threshold must be finite (got " + format_number(config.entry_threshold) + ")");
    }
    if (!std::isfinite(config.rearm_threshold) || config.rearm_threshold < 0.0) {
        reject("rearm_threshold must be finite and at least 0 (got " +
               format_number(config.rearm_threshold) + ")");
    }
    if (config.rearm_threshold >= config.entry_threshold) {
        reject("rearm_threshold must be less than entry_threshold (got rearm_threshold " +
               format_number(config.rearm_threshold) + ", entry_threshold " +
               format_number(config.entry_threshold) + ")");
    }

    detail::require_whole_share_quantity(kName, config.requested_quantity);
    detail::require_symbol_allowlist(kName, config.symbols);
    return config;
}

// What one full window says about its newest close. All DERIVED real numbers: mean and
// standard_deviation are in currency units, z is a pure ratio.
struct Measurement {
    double mean{};
    double standard_deviation{};
    double z{};
    bool   constant{};   // negligible variance: z is not meaningful and is left at 0
};

// Two-pass mean, population standard deviation and z of `current` over `closes`
// (which includes `current`). Every close is a positive Decimal, an integer count of millionths.
// Empty only if a result would be non-finite, which no int64 input can cause (a
// guard; see "Failure" in the header).
//
// Everything is measured as an OFFSET from `current`, and each offset is taken in exact int64
// arithmetic BEFORE it becomes a double. Converting the closes themselves would round every
// close above 2^53 millionths (about 9 * 10^9 currency units) to a multiple of 2, 4, ... 1024,
// so distinct prices could land on the same double and z would carry an error of up to ~10^-5.
// Both closes are positive int64, so their difference never overflows, and an offset is exact
// in a double up to 2^53 whatever the price level; the precision is relative to the spread, not
// to the price. z does not depend on the origin, so nothing is lost by moving it.
std::optional<Measurement> measure(const std::vector<common::Price>& closes,
                                   common::Price current) {
    const auto count = static_cast<double>(closes.size());
    const std::int64_t origin = current.micros();

    // Pass 1: a first estimate of the mean offset, in millionths.
    double sum = 0.0;
    for (const common::Price close : closes) {
        sum += static_cast<double>(close.micros() - origin);
    }
    const double first_mean = sum / count;

    // Pass 2: deviations from that estimate. Their sum is the error of the
    // estimate (zero in exact arithmetic), and subtracting its square over N
    // removes that error from the sum of squares: the corrected two-pass
    // algorithm, which does not suffer the cancellation of E[x^2] - E[x]^2.
    double deviation_sum         = 0.0;
    double squared_deviation_sum = 0.0;
    for (const common::Price close : closes) {
        const double deviation = static_cast<double>(close.micros() - origin) - first_mean;
        deviation_sum += deviation;
        squared_deviation_sum += deviation * deviation;
    }
    const double correction  = deviation_sum / count;
    const double mean_offset = first_mean + correction;   // the mean's distance from `current`
    const double variance    = std::max(0.0, (squared_deviation_sum - deviation_sum * correction) / count);
    const double spread      = std::sqrt(variance);
    const double mean        = static_cast<double>(origin) + mean_offset;   // for reporting and the tolerance
    if (!std::isfinite(mean) || !std::isfinite(spread)) {
        return std::nullopt;
    }

    Measurement result;
    // mean > 0: every close is a positive integer.
    result.constant = spread <= kNegligibleVariation * mean;
    if (!result.constant) {
        result.z = (0.0 - mean_offset) / spread;   // current's own offset is exactly 0; (0.0 - x) keeps +0 for a mean on the close
        if (!std::isfinite(result.z)) {
            return std::nullopt;
        }
    }
    result.mean               = mean / static_cast<double>(common::Decimal::kScale);
    result.standard_deviation = spread / static_cast<double>(common::Decimal::kScale);
    return result;
}

}  // namespace

// --- SymbolState ------------------------------------------------------------------

void MeanReversionStrategy::SymbolState::reset() noexcept {
    closes.clear();   // keeps the capacity, so a restart does not reallocate
    next = 0;
    last_time.reset();
    latch = Latch::Neutral;
}

// --- Strategy ---------------------------------------------------------------------

MeanReversionStrategy::MeanReversionStrategy(MeanReversionConfig config)
    : config_{validated(std::move(config))} {
    states_.reserve(config_.symbols.size());
    for (const common::Symbol& symbol : config_.symbols) {
        states_.emplace(symbol, SymbolState{});
    }
}

void MeanReversionStrategy::on_start() {
    diagnostics_.refuse_inside_callback("on_start");
    for (auto& entry : states_) {
        entry.second.reset();
    }
}

void MeanReversionStrategy::on_market_event(const domain::MarketEvent& event, ISignalSink& out) {
    diagnostics_.refuse_inside_callback("on_market_event");

    // Every check below runs before any state is touched, so an ignored event
    // leaves the timestamps, histories and latches as they were.
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
    if (close <= common::Price{}) {
        report_ignored(event, &state, BarReason::PriceInvalid);
        return;
    }
    if (state.last_time.has_value() && event.exchange_time <= *state.last_time) {
        // duplicate, replay or out of order: strict increase is required
        report_ignored(event, &state, BarReason::TimeNotAfterLastAccepted);
        return;
    }

    accept_close(state, close, config_.lookback);
    state.last_time = event.exchange_time;
    if (state.closes.size() < config_.lookback) {
        report_warming_up(event, state);   // still warming up: the window must be full
        return;
    }

    const Latch before = state.latch;
    const std::optional<Measurement> measured = measure(state.closes, close);
    if (!measured.has_value()) {
        // numerical failure: not an observation, so the latch stays as it is
        report_evaluated(event, state, BarReason::MeasurementFailed, BarAction::None, before,
                         std::nullopt, std::nullopt, std::nullopt);
        return;
    }
    if (measured->constant) {
        state.latch = Latch::Neutral;   // a window with no deviation is back at the mean
        report_evaluated(event, state, BarReason::ConstantWindow, BarAction::None, before,
                         measured->mean, measured->standard_deviation, std::nullopt);
        return;
    }

    const double z = measured->z;
    const bool buy_due  = z <= -config_.entry_threshold && state.latch != Latch::LowerExtreme;
    const bool sell_due = z >= config_.entry_threshold && state.latch != Latch::UpperExtreme;
    if (!buy_due && !sell_due) {
        BarReason reason = BarReason::BetweenBands;
        if (std::fabs(z) <= config_.rearm_threshold) {
            state.latch = Latch::Neutral;
            reason      = BarReason::InsideRearmBand;
        } else if (z <= -config_.entry_threshold || z >= config_.entry_threshold) {
            reason = BarReason::ExcursionAlreadyRequested;
        }
        // otherwise inside neither band, or still in an excursion already requested
        report_evaluated(event, state, reason, BarAction::None, before, measured->mean,
                         measured->standard_deviation, z);
        return;
    }

    // The signal is built before the latch moves (building may throw) and emitted
    // after it (a lost signal is never re-sent).
    const domain::TradeSignal signal =
        make_signal(event, buy_due, measured->mean, measured->standard_deviation, z);
    state.latch = buy_due ? Latch::LowerExtreme : Latch::UpperExtreme;
    report_evaluated(event, state, buy_due ? BarReason::EntryBuy : BarReason::EntrySell,
                     buy_due ? BarAction::Buy : BarAction::Sell, before, measured->mean,
                     measured->standard_deviation, z);
    out.emit(signal);
}

void MeanReversionStrategy::accept_close(SymbolState& state, common::Price close,
                                         std::size_t lookback) {
    std::vector<common::Price>& closes = state.closes;
    if (closes.size() < lookback) {
        closes.push_back(close);   // warming up: may throw, nothing has been modified so far
        return;
    }
    // Full: closes[next] is the oldest close, the one this bar replaces.
    closes[state.next] = close;
    state.next         = (state.next + 1) % lookback;
}

domain::TradeSignal MeanReversionStrategy::make_signal(const domain::MarketEvent& event, bool buy,
                                                       double mean, double standard_deviation,
                                                       double z) const {
    // id, created_at and strategy_id are the engine's to set (see ISignalSink).
    domain::TradeSignal signal;
    signal.symbol             = event.symbol;
    signal.side               = buy ? domain::SignalSide::Buy : domain::SignalSide::Sell;
    signal.requested_quantity = config_.requested_quantity;
    signal.order_type         = domain::OrderType::Market;
    signal.metadata = {
        {"trigger", buy ? "z_score_at_or_below_lower_entry" : "z_score_at_or_above_upper_entry"},
        {"lookback", std::to_string(config_.lookback)},
        {"close", common::format_decimal(*event.price)},
        {"mean", format_number(mean)},
        {"standard_deviation", format_number(standard_deviation)},
        {"z_score", format_number(z)},
        {"entry_threshold", format_number(config_.entry_threshold)},
        {"rearm_threshold", format_number(config_.rearm_threshold)},
    };
    return signal;
}

// --- Diagnostics ----------------------------------------------------------------
// Read-only: each builds a snapshot from values the decision already holds, and only
// when an observer is attached. None changes strategy state.

std::string_view MeanReversionStrategy::latch_label(Latch latch) noexcept {
    switch (latch) {
        case Latch::Neutral:      return "neutral";
        case Latch::LowerExtreme: return "lower_extreme";
        case Latch::UpperExtreme: return "upper_extreme";
    }
    return "invalid";
}

void MeanReversionStrategy::report_ignored(const domain::MarketEvent& event,
                                           const SymbolState* state, BarReason reason) noexcept {
    diagnostics_.notify(event, [&] {
        BarSnapshot snapshot;
        snapshot.strategy_id = config_.strategy_id;
        snapshot.verdict     = BarVerdict::Ignored;
        snapshot.reason      = reason;
        snapshot.window_size = config_.lookback;
        snapshot.add_indicator("mean", std::nullopt);
        snapshot.add_indicator("standard_deviation", std::nullopt);
        snapshot.add_indicator("z_score", std::nullopt);
        if (state != nullptr) {
            snapshot.window_fill = state->closes.size();
            snapshot.add_state("latch_before", latch_label(state->latch));
            snapshot.add_state("latch_after", latch_label(state->latch));
        }
        return snapshot;
    });
}

void MeanReversionStrategy::report_warming_up(const domain::MarketEvent& event,
                                              const SymbolState& state) noexcept {
    diagnostics_.notify(event, [&] {
        BarSnapshot snapshot;
        snapshot.strategy_id = config_.strategy_id;
        snapshot.verdict     = BarVerdict::WarmingUp;
        snapshot.reason      = BarReason::WarmingUp;
        snapshot.window_fill = state.closes.size();
        snapshot.window_size = config_.lookback;
        snapshot.add_indicator("mean", std::nullopt);
        snapshot.add_indicator("standard_deviation", std::nullopt);
        snapshot.add_indicator("z_score", std::nullopt);
        snapshot.add_state("latch_before", latch_label(state.latch));
        snapshot.add_state("latch_after", latch_label(state.latch));
        return snapshot;
    });
}

void MeanReversionStrategy::report_evaluated(const domain::MarketEvent& event,
                                             const SymbolState& state, BarReason reason,
                                             BarAction action, Latch before,
                                             std::optional<double> mean,
                                             std::optional<double> standard_deviation,
                                             std::optional<double> z) noexcept {
    diagnostics_.notify(event, [&] {
        BarSnapshot snapshot;
        snapshot.strategy_id = config_.strategy_id;
        snapshot.verdict     = BarVerdict::Evaluated;
        snapshot.reason      = reason;
        snapshot.action      = action;
        snapshot.window_fill = state.closes.size();
        snapshot.window_size = config_.lookback;
        snapshot.add_indicator("mean", mean);
        snapshot.add_indicator("standard_deviation", standard_deviation);
        snapshot.add_indicator("z_score", z);
        snapshot.add_state("latch_before", latch_label(before));
        snapshot.add_state("latch_after", latch_label(state.latch));
        return snapshot;
    });
}

}  // namespace trading_engine::strategy
