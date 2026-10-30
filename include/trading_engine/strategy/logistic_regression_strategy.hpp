#pragma once

// -----------------------------------------------------------------------------
//  LogisticRegressionStrategy -- an online, walk-forward machine-learning strategy.
//
//  Responsibility: for each symbol in an allowlist, learn from the bars as they
//  arrive whether the close tends to be higher `horizon` bars later, and ask for a
//  Buy when the model's probability of a rise is high enough and a Sell when it is
//  low enough. Pure decision logic, as strategy.hpp requires: it tracks no
//  position, sizes nothing against the portfolio and checks no risk limit. A
//  signal is a REQUEST for the configured quantity, exactly as for the other two
//  reference strategies. Behaviour, a worked example and the local choices are in
//  docs/strategies/logistic_regression.md.
//
//  Input: the same policy as MeanReversionStrategy. Only finalized Bars of one
//  interval per symbol are read, event.price is the close, exchange_time must
//  increase strictly per symbol, and everything else is ignored without changing
//  any state.
//
//  On every accepted bar, in this order (the order is what rules out look-ahead):
//   1. The close joins the symbol's window (the last ml::kFeatureWindow closes).
//   2. The oldest pending sample, if it was taken `horizon` accepted bars ago, is
//      labelled from its close and this close: up when ln(now / then) > dead band,
//      down when < -dead band, and dropped untrained otherwise. A labelled sample
//      is scored for rolling_accuracy (was its prediction on the right side of
//      0.5?) and then trains the model with one SGD step.
//   3. The features of this bar are computed, standardized with the running
//      statistics as they stood BEFORE this bar, and then folded into them.
//   4. The model predicts p = P(up) and the scaled features are queued as a
//      pending sample.
//   5. Until min_training_samples samples have trained the model, nothing is
//      emitted. After that the latch decides:
//        p >= entry_probability and the latch is not Up     -> Buy,  latch Up
//        p <= 1 - entry_probability and the latch is not Down -> Sell, latch Down
//        |p - 0.5| <= rearm_band                            -> latch Neutral
//      Anything else changes nothing. Both comparisons are inclusive.
//  So a decision at bar t depends only on bars up to t: changing any later bar
//  cannot change it (pinned by a test).
//
//  Each symbol has its own model, standardizer, window, queue and latch, so symbols
//  are independent of one another, as in the other strategies. on_start() resets
//  all of it, models included, to the configured initial weights: every run starts
//  cold and is deterministic.
//
//  Signals carry the event's symbol, Buy or Sell, the configured
//  requested_quantity, order_type Market and confidence = p for a Buy, 1 - p for a
//  Sell (the probability of the direction requested). Metadata (all text):
//    trigger              "probability_at_or_above_buy_entry" | "probability_at_or_below_sell_entry"
//    close                exact decimal text of the signalling close
//    probability_up       p
//    horizon, training_samples, entry_probability, rearm_band
//    rolling_accuracy     share of the last ml::kAccuracyWindow scored samples that were
//                         right; absent until one has been scored
//    feature_<name>       the six standardized features the prediction used
//    weight_<name>, bias  the model the prediction used
//  id, created_at and strategy_id are left for StrategyEngine. The latch is
//  committed before the signal is emitted, so a lost signal is never re-sent.
//
//  Diagnostics (optional, strategy_diagnostics.hpp): the ignore reasons of the
//  other strategies, warming_up (window not yet full), training (window full but
//  too few trained samples), measurement_failed, entry_buy, entry_sell,
//  excursion_already_requested, inside_rearm_band and between_bands. Indicators:
//  probability_up, probability_edge (p - 0.5, centred on zero so a chart can draw
//  the entry and rearm bands as +-(entry_probability - 0.5) and +-rearm_band),
//  training_samples, rolling_accuracy. States: latch_before, latch_after.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/strategy/logistic_model.hpp"
#include "trading_engine/strategy/strategy.hpp"
#include "trading_engine/strategy/strategy_diagnostics.hpp"

namespace trading_engine::strategy {

namespace ml {
// Scored samples that rolling_accuracy looks back over.
inline constexpr std::size_t kAccuracyWindow = 100;
}  // namespace ml

// Everything the strategy needs, validated once by the constructor. An invalid
// value is rejected with common::ConfigError naming the field. The defaults are
// local placeholders, NOT tuned or recommended trading parameters.
struct LogisticRegressionConfig {
    // Unique among the strategies registered with one engine. Must not be empty.
    std::string strategy_id{"ml_logistic"};

    // How many accepted bars ahead the label looks. At least 1.
    std::size_t horizon{5};

    // Moves with |ln(close_t+h / close_t)| <= this are not trained on. Finite, >= 0.
    double label_dead_band{0.0};

    // SGD step size: finite and > 0. L2 penalty on the weights: finite and >= 0.
    double learning_rate{0.05};
    double l2{0.001};

    // Trained samples a symbol needs before it may emit. 0 lets it emit at once.
    std::size_t min_training_samples{50};

    // p at or above this asks for a Buy, at or below 1 - this for a Sell.
    // Finite, strictly between 0.5 and 1.
    double entry_probability{0.60};

    // |p - 0.5| at or below this rearms the latch. Finite, >= 0 and strictly below
    // entry_probability - 0.5.
    double rearm_band{0.05};

    // Starting weights (in ml::feature_names() order) then the bias: exactly
    // ml::kParameterCount finite values, for a warm start trained offline. Empty
    // starts every model at zero, so its first prediction is exactly 0.5.
    std::vector<double> initial_parameters{};

    // Shares requested on every signal: a positive whole number.
    common::Quantity requested_quantity{1};

    // Non-empty, no empty entry, no symbol twice; matched exactly.
    std::vector<common::Symbol> symbols{};
};

class LogisticRegressionStrategy final : public IStrategy {
public:
    // Throws common::ConfigError if `config` is invalid; nothing is constructed.
    explicit LogisticRegressionStrategy(LogisticRegressionConfig config);

    LogisticRegressionStrategy(const LogisticRegressionStrategy&)            = delete;
    LogisticRegressionStrategy& operator=(const LogisticRegressionStrategy&) = delete;

    [[nodiscard]] std::string_view id() const override { return config_.strategy_id; }

    // Resets every symbol: window, queue, standardizer, model, latch, counters.
    void on_start() override;

    void on_market_event(const domain::MarketEvent& event, ISignalSink& out) override;

    void set_observer(IStrategyObserver* observer) { diagnostics_.attach(observer); }
    [[nodiscard]] std::uint64_t observer_failures() const noexcept { return diagnostics_.failures(); }

    // Read-only views for tests and tools. Empty for a symbol outside the allowlist.
    [[nodiscard]] std::optional<ml::Parameters> parameters(std::string_view symbol) const;
    [[nodiscard]] std::optional<std::size_t>    training_samples(std::string_view symbol) const;
    [[nodiscard]] std::optional<double>         rolling_accuracy(std::string_view symbol) const;

private:
    enum class Latch : std::uint8_t { Neutral, Up, Down };

    struct Pending {
        std::uint64_t     bar_index{};   // accepted bars before this one, for this symbol
        common::Price     close{};
        double            probability{};
        ml::FeatureVector features{};    // standardized, as the model saw them
    };

    struct SymbolState {
        explicit SymbolState(const LogisticRegressionConfig& config, const ml::Parameters& initial);

        std::vector<common::Price>       closes{};   // oldest first, at most ml::kFeatureWindow
        std::deque<Pending>              pending{};
        ml::RunningStandardizer          standardizer{};
        ml::LogisticModel                model;
        std::optional<common::Timestamp> last_time{};
        std::uint64_t                    accepted{0};
        std::size_t                      trained{0};
        std::vector<std::uint8_t>        hits{};     // ring of the last scored samples, 1 = right
        std::size_t                      next_hit{0};
        Latch                            latch{Latch::Neutral};

        void reset() noexcept;
        void score(bool right);
        [[nodiscard]] std::optional<double> accuracy() const noexcept;
    };

    // Steps 1 and 2 of the header: label and train on what is due.
    void train_due(SymbolState& state, common::Price close) const;

    [[nodiscard]] domain::TradeSignal make_signal(const domain::MarketEvent& event, bool buy,
                                                  const SymbolState& state, double probability,
                                                  const ml::FeatureVector& features) const;

    [[nodiscard]] static std::string_view latch_label(Latch latch) noexcept;
    void report(const domain::MarketEvent& event, const SymbolState* state, BarVerdict verdict,
                BarReason reason, BarAction action, Latch before,
                std::optional<double> probability) noexcept;

    LogisticRegressionConfig config_;
    ml::Parameters           initial_{};
    std::unordered_map<common::Symbol, SymbolState> states_{};
    ObserverSlot diagnostics_{};
};

}  // namespace trading_engine::strategy
