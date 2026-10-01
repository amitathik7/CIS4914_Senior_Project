#include "trading_engine/strategy/mean_reversion_strategy.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include "config_checks.hpp"

namespace trading_engine::strategy {

namespace {

constexpr std::string_view kName = "MeanReversionStrategy";

using detail::format_number;

// A window is CONSTANT when its standard deviation is at most this fraction of
// its mean. That is about 4,500 times a double's rounding step: far above the
// noise of a sum of equal closes, far below any deviation worth acting on. Fixed
// on purpose; see "Floating point" in the header.
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

// What one full window says about its newest close.
struct Measurement {
    double mean{};
    double standard_deviation{};
    double z{};
    bool   constant{};   // negligible variance: z is not meaningful and is left at 0
};

// Two-pass mean, population standard deviation and z of `current` over `closes`
// (which includes `current`). Every close is finite and positive. Empty only if a
// result would be non-finite, which the scaling below is meant to rule out for finite
// input (a guard; see "Failure" in the header).
std::optional<Measurement> measure(const std::vector<common::Price>& closes,
                                   common::Price current) {
    const auto count = static_cast<double>(closes.size());

    // Scale by 2^-exponent so the largest close lies in [0.5, 1). The power of
    // two is exact, so no rounding is added, and every scaled value is in
    // [0, 1): sums stay below `count` and squared deviations below 1, however
    // large the real prices are. (A close more than 2^1000 times smaller than
    // the window's largest underflows towards 0, which is invisible next to it.)
    const double largest = *std::max_element(closes.begin(), closes.end());
    int exponent = 0;
    std::frexp(largest, &exponent);
    const auto scaled = [exponent](double close) { return std::ldexp(close, -exponent); };

    // Pass 1: a first estimate of the mean.
    double sum = 0.0;
    for (const double close : closes) {
        sum += scaled(close);
    }
    const double first_mean = sum / count;

    // Pass 2: deviations from that estimate. Their sum is the error of the
    // estimate (zero in exact arithmetic), and subtracting its square over N
    // removes that error from the sum of squares: the corrected two-pass
    // algorithm, which does not suffer the cancellation of E[x^2] - E[x]^2.
    double deviation_sum         = 0.0;
    double squared_deviation_sum = 0.0;
    for (const double close : closes) {
        const double deviation = scaled(close) - first_mean;
        deviation_sum += deviation;
        squared_deviation_sum += deviation * deviation;
    }
    const double correction = deviation_sum / count;
    // A mean never exceeds the largest value. Rounding could in principle lift the
    // computed one a rounding step above it, and rescaling that past a largest close
    // of DBL_MAX would overflow. So cap it: exact in real arithmetic, and it keeps a
    // constant window of huge closes a constant window rather than a "failure".
    const double mean       = std::min(first_mean + correction, scaled(largest));
    const double variance   = std::max(0.0, (squared_deviation_sum - deviation_sum * correction) / count);
    const double spread     = std::sqrt(variance);
    if (!std::isfinite(mean) || !std::isfinite(spread)) {
        return std::nullopt;
    }

    Measurement result;
    // mean >= 0.5 / count > 0, because the largest scaled close is at least 0.5.
    result.constant = spread <= kNegligibleVariation * mean;
    if (!result.constant) {
        result.z = (scaled(current) - mean) / spread;
        if (!std::isfinite(result.z)) {
            return std::nullopt;
        }
    }
    result.mean               = std::ldexp(mean, exponent);        // <= the largest close
    result.standard_deviation = std::ldexp(spread, exponent);   // < the largest close
    if (!std::isfinite(result.mean) || !std::isfinite(result.standard_deviation)) {
        return std::nullopt;
    }
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
    for (auto& entry : states_) {
        entry.second.reset();
    }
}

void MeanReversionStrategy::on_market_event(const domain::MarketEvent& event, ISignalSink& out) {
    // Every check below runs before any state is touched, so an ignored event
    // leaves the timestamps, histories and latches as they were.
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
    if (!std::isfinite(close) || close <= 0.0) {
        return;
    }
    SymbolState& state = found->second;
    if (state.last_time.has_value() && event.exchange_time <= *state.last_time) {
        return;   // duplicate, replay or out of order: strict increase is required
    }

    accept_close(state, close, config_.lookback);
    state.last_time = event.exchange_time;
    if (state.closes.size() < config_.lookback) {
        return;   // still warming up: the window must be full
    }

    const std::optional<Measurement> measured = measure(state.closes, close);
    if (!measured.has_value()) {
        return;   // numerical failure: not an observation, so the latch stays as it is
    }
    if (measured->constant) {
        state.latch = Latch::Neutral;   // a window with no deviation is back at the mean
        return;
    }

    const double z = measured->z;
    const bool buy_due  = z <= -config_.entry_threshold && state.latch != Latch::LowerExtreme;
    const bool sell_due = z >= config_.entry_threshold && state.latch != Latch::UpperExtreme;
    if (!buy_due && !sell_due) {
        if (std::fabs(z) <= config_.rearm_threshold) {
            state.latch = Latch::Neutral;
        }
        return;   // otherwise inside neither band, or still in an excursion already requested
    }

    // The signal is built before the latch moves (building may throw) and emitted
    // after it (a lost signal is never re-sent).
    const domain::TradeSignal signal =
        make_signal(event, buy_due, measured->mean, measured->standard_deviation, z);
    state.latch = buy_due ? Latch::LowerExtreme : Latch::UpperExtreme;
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
        {"close", format_number(*event.price)},
        {"mean", format_number(mean)},
        {"standard_deviation", format_number(standard_deviation)},
        {"z_score", format_number(z)},
        {"entry_threshold", format_number(config_.entry_threshold)},
        {"rearm_threshold", format_number(config_.rearm_threshold)},
    };
    return signal;
}

}  // namespace trading_engine::strategy
