#include "trading_engine/strategy/logistic_regression_strategy.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "config_checks.hpp"
#include "trading_engine/common/decimal_text.hpp"

namespace trading_engine::strategy {

namespace {

constexpr std::string_view kName = "LogisticRegressionStrategy";

using detail::format_number;

[[noreturn]] void reject(const std::string& problem) {
    detail::reject(kName, problem);
}

LogisticRegressionConfig validated(LogisticRegressionConfig config) {
    if (config.strategy_id.empty()) {
        reject("strategy_id must not be empty");
    }
    if (config.horizon < 1) {
        reject("horizon must be at least 1 (got 0)");
    }
    if (!std::isfinite(config.label_dead_band) || config.label_dead_band < 0.0) {
        reject("label_dead_band must be finite and at least 0 (got " +
               format_number(config.label_dead_band) + ")");
    }
    if (!std::isfinite(config.learning_rate) || config.learning_rate <= 0.0) {
        reject("learning_rate must be finite and greater than 0 (got " +
               format_number(config.learning_rate) + ")");
    }
    if (!std::isfinite(config.l2) || config.l2 < 0.0) {
        reject("l2 must be finite and at least 0 (got " + format_number(config.l2) + ")");
    }
    if (!std::isfinite(config.entry_probability) || config.entry_probability <= 0.5 ||
        config.entry_probability >= 1.0) {
        reject("entry_probability must be finite and strictly between 0.5 and 1 (got " +
               format_number(config.entry_probability) + ")");
    }
    if (!std::isfinite(config.rearm_band) || config.rearm_band < 0.0) {
        reject("rearm_band must be finite and at least 0 (got " + format_number(config.rearm_band) + ")");
    }
    if (config.rearm_band >= config.entry_probability - 0.5) {
        reject("rearm_band must be less than entry_probability - 0.5 (got rearm_band " +
               format_number(config.rearm_band) + ", entry_probability " +
               format_number(config.entry_probability) + ")");
    }
    if (!config.initial_parameters.empty()) {
        if (config.initial_parameters.size() != ml::kParameterCount) {
            reject("initial_parameters must be empty or hold exactly " +
                   std::to_string(ml::kParameterCount) + " values, the weights then the bias (got " +
                   std::to_string(config.initial_parameters.size()) + ")");
        }
        for (const double value : config.initial_parameters) {
            if (!std::isfinite(value)) {
                reject("initial_parameters must all be finite (got " + format_number(value) + ")");
            }
        }
    }

    detail::require_whole_share_quantity(kName, config.requested_quantity);
    detail::require_symbol_allowlist(kName, config.symbols);
    return config;
}

ml::Parameters initial_parameters(const LogisticRegressionConfig& config) {
    ml::Parameters result{};
    std::copy(config.initial_parameters.begin(), config.initial_parameters.end(), result.begin());
    return result;
}

}  // namespace

// --- SymbolState ------------------------------------------------------------------

LogisticRegressionStrategy::SymbolState::SymbolState(const LogisticRegressionConfig& config,
                                                     const ml::Parameters& initial)
    : model{config.learning_rate, config.l2, initial} {
    closes.reserve(ml::kFeatureWindow);
    hits.reserve(ml::kAccuracyWindow);
}

void LogisticRegressionStrategy::SymbolState::reset() noexcept {
    closes.clear();
    pending.clear();
    standardizer.reset();
    model.reset();
    last_time.reset();
    accepted = 0;
    trained  = 0;
    hits.clear();
    next_hit = 0;
    totals   = {};
    latch    = Latch::Neutral;
}

void LogisticRegressionStrategy::SymbolState::score(bool right) {
    const std::uint8_t hit = right ? 1 : 0;
    if (hits.size() < ml::kAccuracyWindow) {
        hits.push_back(hit);
        return;
    }
    hits[next_hit] = hit;
    next_hit       = (next_hit + 1) % ml::kAccuracyWindow;
}

std::optional<double> LogisticRegressionStrategy::SymbolState::accuracy() const noexcept {
    if (hits.empty()) {
        return std::nullopt;
    }
    std::size_t right = 0;
    for (const std::uint8_t hit : hits) {
        right += hit;
    }
    return static_cast<double>(right) / static_cast<double>(hits.size());
}

// --- Strategy ---------------------------------------------------------------------

LogisticRegressionStrategy::LogisticRegressionStrategy(LogisticRegressionConfig config)
    : config_{validated(std::move(config))}, initial_{initial_parameters(config_)} {
    states_.reserve(config_.symbols.size());
    for (const common::Symbol& symbol : config_.symbols) {
        states_.emplace(symbol, SymbolState{config_, initial_});
    }
}

void LogisticRegressionStrategy::on_start() {
    diagnostics_.refuse_inside_callback("on_start");
    for (auto& entry : states_) {
        entry.second.reset();
    }
}

void LogisticRegressionStrategy::on_market_event(const domain::MarketEvent& event, ISignalSink& out) {
    diagnostics_.refuse_inside_callback("on_market_event");

    // Every check runs before any state is touched, so an ignored event changes nothing.
    if (event.type != domain::MarketEventType::Bar) {
        report(event, nullptr, BarVerdict::Ignored, BarReason::NotABar, BarAction::None,
               Latch::Neutral, std::nullopt);
        return;
    }
    const auto found = states_.find(event.symbol);
    if (found == states_.end()) {
        report(event, nullptr, BarVerdict::Ignored, BarReason::SymbolNotAllowlisted, BarAction::None,
               Latch::Neutral, std::nullopt);
        return;
    }
    SymbolState& state = found->second;
    const Latch before = state.latch;
    if (!event.price.has_value()) {
        report(event, &state, BarVerdict::Ignored, BarReason::PriceAbsent, BarAction::None, before,
               std::nullopt);
        return;
    }
    const common::Price close = *event.price;
    if (close <= common::Price{}) {
        report(event, &state, BarVerdict::Ignored, BarReason::PriceInvalid, BarAction::None, before,
               std::nullopt);
        return;
    }
    if (state.last_time.has_value() && event.exchange_time <= *state.last_time) {
        report(event, &state, BarVerdict::Ignored, BarReason::TimeNotAfterLastAccepted,
               BarAction::None, before, std::nullopt);
        return;
    }

    // 1. The close joins the window (no allocation once it is full).
    if (state.closes.size() == ml::kFeatureWindow) {
        state.closes.erase(state.closes.begin());
    }
    state.closes.push_back(close);
    state.last_time = event.exchange_time;
    const std::uint64_t index = state.accepted++;

    // 2. Train on the sample whose outcome is now known.
    train_due(state, close);

    if (state.closes.size() < ml::kFeatureWindow) {
        report(event, &state, BarVerdict::WarmingUp, BarReason::WarmingUp, BarAction::None, before,
               std::nullopt);
        return;
    }

    // 3. Features, scaled with the statistics of earlier bars only.
    const std::optional<ml::FeatureVector> raw = ml::compute_features(state.closes);
    if (!raw.has_value()) {
        // A guard no positive close can reach; not an observation, so the latch stays.
        report(event, &state, BarVerdict::Evaluated, BarReason::MeasurementFailed, BarAction::None,
               before, std::nullopt);
        return;
    }
    const ml::FeatureVector features = state.standardizer.scale(*raw);
    state.standardizer.observe(*raw);

    // 4. Predict, and queue the sample until its outcome is known.
    const double probability = state.model.predict(features);
    state.pending.push_back(Pending{index, close, probability, features});

    // 5. Decide.
    if (state.trained < config_.min_training_samples) {
        report(event, &state, BarVerdict::WarmingUp, BarReason::Training, BarAction::None, before,
               probability);
        return;
    }
    const bool buy_entry  = probability >= config_.entry_probability;
    const bool sell_entry = probability <= 1.0 - config_.entry_probability;
    const bool buy_due    = buy_entry && state.latch != Latch::Up;
    const bool sell_due   = sell_entry && state.latch != Latch::Down;
    if (!buy_due && !sell_due) {
        BarReason reason = BarReason::BetweenBands;
        if (std::fabs(probability - 0.5) <= config_.rearm_band) {
            state.latch = Latch::Neutral;
            reason      = BarReason::InsideRearmBand;
        } else if (buy_entry || sell_entry) {
            reason = BarReason::ExcursionAlreadyRequested;
        }
        report(event, &state, BarVerdict::Evaluated, reason, BarAction::None, before, probability);
        return;
    }

    // Built before the latch moves (building may throw), emitted after it (a lost
    // signal is never re-sent).
    const domain::TradeSignal signal = make_signal(event, buy_due, state, probability, features);
    state.latch = buy_due ? Latch::Up : Latch::Down;
    report(event, &state, BarVerdict::Evaluated, buy_due ? BarReason::EntryBuy : BarReason::EntrySell,
           buy_due ? BarAction::Buy : BarAction::Sell, before, probability);
    out.emit(signal);
}

void LogisticRegressionStrategy::train_due(SymbolState& state, common::Price close) const {
    const std::uint64_t now = state.accepted - 1;
    while (!state.pending.empty() && now - state.pending.front().bar_index >= config_.horizon) {
        const Pending sample = state.pending.front();
        state.pending.pop_front();
        const double move = std::log(static_cast<double>(close.micros()) /
                                     static_cast<double>(sample.close.micros()));
        if (std::fabs(move) <= config_.label_dead_band) {
            continue;   // too small to call: not a training sample
        }
        const bool up    = move > 0.0;
        const bool right = (sample.probability >= 0.5) == up;
        state.score(right);
        ++state.totals.scored;
        state.totals.correct += right ? 1 : 0;
        state.totals.up_labels += up ? 1 : 0;
        state.model.train(sample.features, up);
        ++state.trained;
    }
}

domain::TradeSignal LogisticRegressionStrategy::make_signal(const domain::MarketEvent& event, bool buy,
                                                            const SymbolState& state,
                                                            double probability,
                                                            const ml::FeatureVector& features) const {
    domain::TradeSignal signal;
    signal.symbol             = event.symbol;
    signal.side               = buy ? domain::SignalSide::Buy : domain::SignalSide::Sell;
    signal.requested_quantity = config_.requested_quantity;
    signal.order_type         = domain::OrderType::Market;
    signal.confidence         = buy ? probability : 1.0 - probability;
    signal.metadata = {
        {"trigger", buy ? "probability_at_or_above_buy_entry" : "probability_at_or_below_sell_entry"},
        {"close", common::format_decimal(*event.price)},
        {"probability_up", format_number(probability)},
        {"horizon", std::to_string(config_.horizon)},
        {"training_samples", std::to_string(state.trained)},
        {"entry_probability", format_number(config_.entry_probability)},
        {"rearm_band", format_number(config_.rearm_band)},
    };
    if (const std::optional<double> accuracy = state.accuracy(); accuracy.has_value()) {
        signal.metadata.emplace("rolling_accuracy", format_number(*accuracy));
    }
    const ml::Parameters& parameters = state.model.parameters();
    for (std::size_t i = 0; i < ml::kFeatureCount; ++i) {
        const std::string name{ml::feature_names()[i]};
        signal.metadata.emplace("feature_" + name, format_number(features[i]));
        signal.metadata.emplace("weight_" + name, format_number(parameters[i]));
    }
    signal.metadata.emplace("bias", format_number(parameters[ml::kFeatureCount]));
    return signal;
}

std::optional<ml::Parameters> LogisticRegressionStrategy::parameters(std::string_view symbol) const {
    const auto found = states_.find(common::Symbol{symbol});
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second.model.parameters();
}

std::optional<std::size_t> LogisticRegressionStrategy::training_samples(std::string_view symbol) const {
    const auto found = states_.find(common::Symbol{symbol});
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second.trained;
}

std::optional<double> LogisticRegressionStrategy::rolling_accuracy(std::string_view symbol) const {
    const auto found = states_.find(common::Symbol{symbol});
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second.accuracy();
}

std::optional<ml::Scoreboard> LogisticRegressionStrategy::scoreboard(std::string_view symbol) const {
    const auto found = states_.find(common::Symbol{symbol});
    if (found == states_.end()) {
        return std::nullopt;
    }
    return found->second.totals;
}

// --- Diagnostics ----------------------------------------------------------------

std::string_view LogisticRegressionStrategy::latch_label(Latch latch) noexcept {
    switch (latch) {
        case Latch::Neutral: return "neutral";
        case Latch::Up:      return "up";
        case Latch::Down:    return "down";
    }
    return "invalid";
}

void LogisticRegressionStrategy::report(const domain::MarketEvent& event, const SymbolState* state,
                                        BarVerdict verdict, BarReason reason, BarAction action,
                                        Latch before, std::optional<double> probability) noexcept {
    diagnostics_.notify(event, [&] {
        BarSnapshot snapshot;
        snapshot.strategy_id = config_.strategy_id;
        snapshot.verdict     = verdict;
        snapshot.reason      = reason;
        snapshot.action      = action;
        snapshot.window_size = ml::kFeatureWindow;
        snapshot.add_indicator("probability_up", probability);
        snapshot.add_indicator("probability_edge",
                               probability.has_value() ? std::optional<double>{*probability - 0.5}
                                                       : std::nullopt);
        if (state != nullptr) {
            snapshot.window_fill = state->closes.size();
            snapshot.add_indicator("training_samples", static_cast<double>(state->trained));
            snapshot.add_indicator("rolling_accuracy", state->accuracy());
            snapshot.add_state("latch_before", latch_label(before));
            snapshot.add_state("latch_after", latch_label(state->latch));
        } else {
            snapshot.add_indicator("training_samples", std::nullopt);
            snapshot.add_indicator("rolling_accuracy", std::nullopt);
        }
        return snapshot;
    });
}

}  // namespace trading_engine::strategy
