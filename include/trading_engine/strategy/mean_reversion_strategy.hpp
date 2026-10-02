#pragma once

// -----------------------------------------------------------------------------
//  MeanReversionStrategy -- rolling z-score mean reversion.
//
//  Responsibility: watch the closing prices of an allowlist of symbols and ask for
//  a Buy when a close is unusually far BELOW its recent mean, and a Sell when it
//  is unusually far ABOVE it. Pure decision logic, as strategy.hpp requires: it
//  tracks no position, sizes nothing against the portfolio and checks no risk
//  limit. A signal is a REQUEST for the configured quantity. A Buy does not mean a
//  position is open and a Sell does not mean "close it"; the strategy has no exit
//  logic at all, and what happens to a request is decided downstream. Behaviour,
//  worked examples and the local design choices are in docs/strategies/mean_reversion.md
//  (the contract every strategy shares is in docs/STRATEGIES.md).
//
//  Input (the same policy as MovingAverageCrossoverStrategy)
//   * Only MarketEventType::Bar is read, and event.price is taken as the bar's
//     CLOSE. The caller must supply FINALIZED bars of ONE consistent interval per
//     symbol. MarketEvent has no interval or "final" field, so a partial bar or a
//     mix of intervals looks exactly like a good bar and would corrupt the
//     statistics without any sign of it.
//   * exchange_time must increase strictly, per symbol (symbols are independent
//     of one another). The window counts ACCEPTED bars: a gap in the data is never
//     filled in, so a missing bar simply is not in the window.
//   * Everything else is IGNORED, and an ignored event changes nothing: not the
//     last timestamp, not the history, not the latch. That covers a symbol outside
//     the allowlist, any other event type, a missing / non-finite / non-positive
//     close and an exchange_time that is not later than the last accepted bar's
//     for that symbol (a duplicate, a replay, an older bar). Any finite positive
//     close is accepted, however large or small (see "Floating point").
//
//  The statistic. N is `lookback`. After the current accepted close is added to
//  the window, and once N closes have been accepted:
//      mean               = sum(close_i) / N
//      population_stddev  = sqrt( sum((close_i - mean)^2) / N )      (divides by N, not N-1)
//      z                  = (current_close - mean) / population_stddev
//  The CURRENT close is part of the window, so it pulls its own mean and widens its
//  own deviation: |z| can never exceed sqrt(N - 1). An entry_threshold above that
//  is accepted but can never fire (lookback 2 caps |z| at 1; the default 2.0 needs
//  lookback 5 or more). Each bar costs O(N) work and each symbol keeps O(N) closes.
//
//  The latch (a per-symbol indicator state, NOT a position and NOT a record of
//  fills). Neutral at the start of every run:
//   1. z <= -entry_threshold and the latch is not LowerExtreme -> one Buy
//      request; the latch becomes LowerExtreme.
//   2. z >= +entry_threshold and the latch is not UpperExtreme -> one Sell
//      request; the latch becomes UpperExtreme.
//   3. |z| <= rearm_threshold -> the latch becomes Neutral; nothing is emitted.
//   4. Anything else leaves the latch as it was and emits nothing.
//  Both comparisons are inclusive. Staying beyond a threshold never repeats a
//  request; a request is repeated only after a bar inside the rearm band. A direct
//  jump from one extreme to the other emits the opposite request at once, with no
//  Neutral bar between. The first warmed-up window may emit (unlike the SMA
//  strategy, there is no silent baseline). Rearming closes nothing.
//
//  Floating point (common::Price is a provisional double, OQ#11)
//   * The window is re-measured from its raw closes on every bar. No running sum
//     or sum of squares is kept, so no error accumulates over a long run and a bad
//     window cannot poison a later one: once its closes have left the window it
//     is gone. This is the same two-pass computation whatever the price level.
//   * Overflow. Squaring a deviation of 1e200 overflows, so the closes are first
//     scaled by an exact power of two (the one that brings the window's largest
//     close into [0.5, 1)). Scaling by 2^k is exact, z does not depend on the scale,
//     and the mean and standard deviation are scaled back at the end, with the mean
//     capped at the largest close so that rescaling cannot overflow. Every finite
//     positive close up to DBL_MAX is accepted. (A close more than 2^1022 times
//     smaller than the window's largest loses precision when scaled and is 0 beyond
//     about 2^1074; it is invisible next to that largest close, whose scale z is
//     measured in.) The variance itself is never reported, because it can exceed
//     DBL_MAX when the deviation does not.
//   * Cancellation. The deviations are taken from a first-pass mean (not the
//     E[x^2] - E[x]^2 form) and the mean is then corrected by the mean of those
//     deviations, so rounding in the first sum does not leak into the variance.
//   * Negligible variance. A window is CONSTANT when
//     standard_deviation <= 1e-12 * mean. Mathematically equal closes rarely sum
//     back to exactly their common value (0.1 added thirty times does not), and
//     dividing noise by noise would give an arbitrary z. A constant window emits
//     nothing and sets the latch to Neutral: it is a genuine observation of "no
//     deviation". The tolerance is fixed, not a configuration option.
//   * Failure. A window whose mean, deviation or z would be non-finite emits
//     nothing and leaves the latch exactly as it was: an unrelated numerical
//     failure is not evidence the price is back to normal, so it never rearms.
//     The bar itself was accepted, so the next bar measures its own window
//     afresh. This is a guard, not a path any known input takes, and it has no
//     direct test. After scaling every value is in [0, 1), so every sum, square
//     and |z| stays finite (|z| < about 2 * lookback * 1e12), which holds for
//     IEEE-754 binary64 round-to-nearest, no fast-math and a lookback far below
//     2^53. That is an argument plus a randomized search that found no trigger,
//     not a proof; see docs/strategies/mean_reversion.md, section 5.
//   * Do not build this file with -ffast-math or /fp:fast: reassociation would
//     undo the exact scaling and the correction step.
//
//  Signals. Each one carries the event's symbol, side Buy or Sell, the configured
//  requested_quantity, order_type Market, and metadata (all values text, the
//  numbers as the shortest text that reads back as the exact double, never
//  non-finite):
//    trigger              "z_score_at_or_below_lower_entry" (Buy)
//                         | "z_score_at_or_above_upper_entry" (Sell)
//    lookback             the configured window size, in bars
//    close                the signalling bar's close
//    mean, standard_deviation, z_score   the window statistics, current close included
//    entry_threshold, rearm_threshold    the configured thresholds
//  target_exposure, limit_price and confidence stay unset. id, created_at and
//  strategy_id are left for StrategyEngine, which owns them (see ISignalSink). The
//  latch is committed BEFORE the signal is emitted, so a signal the engine or bus
//  then loses is lost, never re-sent on a later bar -- the same rule the engine
//  applies.
//
//  Lifecycle and threading. on_start() clears every symbol's state, so each run
//  starts cold; a fresh object is already cold, so it can also be driven directly,
//  without an engine. Callbacks never overlap (see strategy.hpp), so there is no
//  locking. No allocation per event except the warm-up growth of each history
//  (bounded by lookback) and the emitted signal itself.
//
//  Diagnostics (optional). set_observer() attaches a read-only observer that is
//  told, once per on_market_event() call, what the strategy decided and from which
//  numbers (strategy_diagnostics.hpp has the full contract, including what an
//  observer must not do and what happens if it throws). It never changes a
//  decision, the latch or a signal. Reasons reported: the ignore reasons of "Input"
//  above (not_a_bar, symbol_not_allowlisted, price_absent, price_invalid,
//  time_not_after_last_accepted; price_above_max_close never occurs here), then
//  warming_up, measurement_failed, constant_window, entry_buy, entry_sell,
//  excursion_already_requested, inside_rearm_band and between_bands. Indicators:
//  mean, standard_deviation, z_score (unavailable when warming up or ignored; z_score
//  is also unavailable for a constant window, where the statistics exist but z does not
//  mean anything, and all three for a failed measurement). States: latch_before,
//  latch_after.
//
//  TODO: a symbol-interest declaration (see IStrategy); counters or logging for
//        ignored events once a logging facade exists (OQ#8); a bar-interval /
//        "final" flag on MarketEvent (OQ#1) so the input assumption can be
//        checked instead of trusted; O(1) rolling statistics if a very large
//        lookback ever makes the O(N) pass a cost worth removing.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/strategy/strategy.hpp"
#include "trading_engine/strategy/strategy_diagnostics.hpp"

namespace trading_engine::strategy {

// Everything the strategy needs, validated once by the constructor. Nothing is
// rounded, clamped or defaulted on the caller's behalf: an invalid value is
// rejected with common::ConfigError, and the message names the field.
struct MeanReversionConfig {
    // Stable instance id: becomes TradeSignal::strategy_id and must be unique
    // among the strategies registered with one engine. Two instances (say, a
    // short and a long lookback) need two ids; the default suits a single
    // instance. Must not be empty.
    std::string strategy_id{"mean_reversion"};

    // Window size, in ACCEPTED bars, current bar included. At least 2.
    std::size_t lookback{20};

    // How far from the mean, in population standard deviations, a close must be
    // to trigger a request. Finite, and 0 <= rearm_threshold < entry_threshold.
    // A local default, not a tuned or recommended trading parameter.
    double entry_threshold{2.0};

    // |z| at or below this puts the latch back to Neutral. Finite, >= 0 and
    // strictly below entry_threshold (so a rearm band always separates the two
    // extremes). 0 rearms only on a constant window or a close exactly at the mean.
    double rearm_threshold{0.5};

    // Shares requested on every signal. A whole number: finite and positive,
    // never rounded (1.5 is rejected, not turned into 1 or 2). Whole shares are
    // this strategy's local choice, not a project-wide numeric policy (see
    // docs/adr/0002-strategy-risk-signal-contract.md, section 8, still Proposed).
    common::Quantity requested_quantity{1.0};

    // The symbols to trade, matched exactly (case-sensitive) against
    // MarketEvent::symbol. No default: it must be non-empty, contain no empty
    // entry and list no symbol twice.
    std::vector<common::Symbol> symbols{};
};

class MeanReversionStrategy final : public IStrategy {
public:
    // Throws common::ConfigError if `config` is invalid; nothing is constructed.
    explicit MeanReversionStrategy(MeanReversionConfig config);

    // Not copyable: an instance holds the state of a run, and a copy would carry
    // it (and the same id) into a second strategy. Held by shared_ptr.
    MeanReversionStrategy(const MeanReversionStrategy&)            = delete;
    MeanReversionStrategy& operator=(const MeanReversionStrategy&) = delete;

    [[nodiscard]] std::string_view id() const override { return config_.strategy_id; }

    // Clears every symbol's history, timestamp and latch. The observer, if any, stays
    // attached.
    void on_start() override;

    void on_market_event(const domain::MarketEvent& event, ISignalSink& out) override;

    // Optional read-only diagnostics; see strategy_diagnostics.hpp. Not owned: the
    // observer must outlive this strategy or be detached with nullptr first. Call it
    // between events, never from inside the observer.
    void set_observer(IStrategyObserver* observer) { diagnostics_.attach(observer); }

    // Exceptions the observer threw that were swallowed (never forwarded to the engine).
    [[nodiscard]] std::uint64_t observer_failures() const noexcept { return diagnostics_.failures(); }

private:
    // The indicator latch: which extreme, if any, has already been requested.
    enum class Latch : std::uint8_t { Neutral, LowerExtreme, UpperExtreme };

    // Per-symbol state, created for every allowlisted symbol up front.
    struct SymbolState {
        // The last min(accepted, lookback) closes. Oldest first while it fills;
        // once full a ring in which closes[next] is the oldest close, the one the
        // next bar replaces. The statistics do not depend on the order.
        std::vector<common::Price> closes{};
        std::size_t                next{0};

        std::optional<common::Timestamp> last_time{};   // exchange_time of the last accepted bar
        Latch                            latch{Latch::Neutral};

        void reset() noexcept;
    };

    // Adds an accepted close to the history. The only step that can throw
    // (history growth during warm-up) runs first, so a failure leaves `state`
    // untouched.
    static void accept_close(SymbolState& state, common::Price close, std::size_t lookback);

    [[nodiscard]] domain::TradeSignal make_signal(const domain::MarketEvent& event, bool buy,
                                                  double mean, double standard_deviation,
                                                  double z) const;

    // Diagnostics reporting. Each only builds a snapshot when an observer is attached
    // and never throws; none reads anything the decision did not already compute.
    [[nodiscard]] static std::string_view latch_label(Latch latch) noexcept;
    void report_ignored(const domain::MarketEvent& event, const SymbolState* state,
                        BarReason reason) noexcept;
    void report_warming_up(const domain::MarketEvent& event, const SymbolState& state) noexcept;
    // Empty statistics were not available for this bar. `after` is the latch now.
    void report_evaluated(const domain::MarketEvent& event, const SymbolState& state,
                          BarReason reason, BarAction action, Latch before,
                          std::optional<double> mean, std::optional<double> standard_deviation,
                          std::optional<double> z) noexcept;

    MeanReversionConfig config_;
    std::unordered_map<common::Symbol, SymbolState> states_{};
    ObserverSlot diagnostics_{};
};

}  // namespace trading_engine::strategy
