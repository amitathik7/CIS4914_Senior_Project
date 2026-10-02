#pragma once

// -----------------------------------------------------------------------------
//  MovingAverageCrossoverStrategy -- simple-moving-average (SMA) crossover.
//
//  Responsibility: watch the closing prices of an allowlist of symbols and ask
//  for a Buy when the short SMA crosses above the long SMA, and a Sell when it
//  crosses below. Pure decision logic, as strategy.hpp requires: it tracks no
//  position, sizes nothing against the portfolio and checks no risk limit. A
//  signal is a REQUEST for the configured quantity. A Buy does not mean a
//  position is open and a Sell does not mean "close it": what happens to a
//  request is decided downstream. Behaviour, worked examples and the local
//  design choices are in docs/strategies/moving_average_crossover.md (the contract
//  every strategy shares is in docs/STRATEGIES.md).
//
//  Input
//   * Only MarketEventType::Bar is read, and event.price is taken as the bar's
//     CLOSE. The caller must supply FINALIZED bars of ONE consistent interval
//     per symbol. MarketEvent has no interval or "final" field, so a partial bar
//     or a mix of intervals looks exactly like a good bar and would corrupt the
//     averages without any sign of it.
//   * exchange_time must increase strictly, per symbol (symbols are independent
//     of one another). The windows count ACCEPTED bars: a gap in the data is
//     never filled in, so a missing bar simply is not in the average.
//   * Everything else is IGNORED, and an ignored event changes nothing: not the
//     last timestamp, not the history, not the averages, not the crossover
//     state. That covers a symbol outside the allowlist, any other event type, a
//     missing / non-finite / non-positive close, a close above max_close (see
//     "Floating point") and an exchange_time that is not later than the last
//     accepted bar's for that symbol (a duplicate, a replay, an older bar).
//
//  Crossover rules. short and long are the averages of the last short_window
//  and long_window accepted closes, each including the current bar.
//   1. Nothing is emitted until the long window is full (the short one fills
//      sooner, since short_window < long_window).
//   2. The sign of (short - long) is the RELATIONSHIP of that bar. The first
//      nonzero relationship after warm-up is a baseline: it is recorded and
//      never signalled, so a strategy started mid-trend does not fire at once.
//   3. Buy when the last nonzero relationship was negative and is now positive;
//      Sell for the reverse. The signal is emitted on the bar where the
//      relationship changes.
//   4. Equal averages emit nothing and leave the last nonzero relationship in
//      place: below -> equal -> below is not a crossover, below -> equal ->
//      above is one (signalled on the bar that is above).
//   5. Staying on one side never repeats a signal.
//
//  Floating point (common::Price is a provisional double, OQ#11)
//   * The two rolling sums are Neumaier-compensated, so the rounding error of
//     one update does not pile up over the thousands of updates of a long run.
//     Do not build this file with -ffast-math or /fp:fast: reassociation
//     would silently cancel the compensation.
//   * The averages are EQUAL when |short - long| <= 1e-12 * max(short, long).
//     Mathematically equal averages are rarely bit-equal (0.1 + 0.2 differs from
//     0.15 + 0.15), and without this a tie would be read as a one-ulp "cross"
//     and fire a false signal. A gap below one part in 10^12 of the price is
//     rounding noise; any real move is far larger. The tolerance is fixed, not a
//     configuration option.
//   * No non-finite value can enter the state. A close larger than
//     max_close = DBL_MAX / (2 * (long_window + 1)) is ignored like any other
//     invalid bar, which keeps every rolling sum, average and gap finite.
//
//  Signals. Each one carries the event's symbol, side Buy or Sell, the
//  configured requested_quantity, order_type Market, and metadata:
//    trigger      "short_crossed_above_long" (Buy) | "short_crossed_below_long" (Sell)
//    short_window, long_window   the configured sizes, in bars
//    short_sma, long_sma         both averages on the signalling bar (shortest
//                                text that reads back as the exact double)
//  target_exposure, limit_price and confidence stay unset. id, created_at and
//  strategy_id are left for StrategyEngine, which owns them (see ISignalSink);
//  the engine takes strategy_id from id(). The crossover state is committed
//  BEFORE the signal is emitted, so a signal the engine or bus then loses is
//  lost, never re-sent on a later bar -- the same rule the engine applies.
//
//  Lifecycle and threading. on_start() clears every symbol's state, so each run
//  starts cold; a fresh object is already cold, so it can also be driven
//  directly, without an engine. Callbacks never overlap (see strategy.hpp), so
//  there is no locking. Per-event cost is O(1) with no allocation except the
//  warm-up growth of each history (bounded by long_window) and the emitted
//  signal itself.
//
//  Diagnostics (optional). set_observer() attaches a read-only observer that is
//  told, once per on_market_event() call, what the strategy decided and from which
//  numbers (strategy_diagnostics.hpp has the full contract, including what an
//  observer must not do and what happens if it throws). It never changes a
//  decision, the state or a signal. Reasons reported, in the order checked: the
//  ignore reasons of "Input" above (not_a_bar, symbol_not_allowlisted, price_absent,
//  price_invalid, price_above_max_close, time_not_after_last_accepted), then
//  warming_up, averages_equal, baseline_established, same_side, crossover_buy and
//  crossover_sell. Indicators: short_sma, long_sma, short_minus_long (unavailable
//  until the long window is full). States: relation_before, relation_now,
//  relation_after.
//
//  TODO: a symbol-interest declaration (see IStrategy) so the engine can skip
//        this strategy for symbols it ignores; counters or logging for ignored
//        events once a logging facade exists (OQ#8); a bar-interval / "final"
//        flag on MarketEvent (OQ#1) so the input assumption can be checked
//        instead of trusted.
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
struct MovingAverageCrossoverConfig {
    // Stable instance id: becomes TradeSignal::strategy_id and must be unique
    // among the strategies registered with one engine. Two instances (say, a
    // fast and a slow one) need two ids; the default suits a single instance.
    // Must not be empty.
    std::string strategy_id{"sma_crossover"};

    // Window sizes, in ACCEPTED bars. Need 0 < short_window < long_window.
    std::size_t short_window{5};
    std::size_t long_window{20};

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

class MovingAverageCrossoverStrategy final : public IStrategy {
public:
    // Throws common::ConfigError if `config` is invalid; nothing is constructed.
    explicit MovingAverageCrossoverStrategy(MovingAverageCrossoverConfig config);

    // Not copyable: an instance holds the state of a run, and a copy would carry
    // it (and the same id) into a second strategy. Held by shared_ptr.
    MovingAverageCrossoverStrategy(const MovingAverageCrossoverStrategy&)            = delete;
    MovingAverageCrossoverStrategy& operator=(const MovingAverageCrossoverStrategy&) = delete;

    [[nodiscard]] std::string_view id() const override { return config_.strategy_id; }

    // Clears every symbol's history, timestamp and crossover state. The observer, if
    // any, stays attached.
    void on_start() override;

    void on_market_event(const domain::MarketEvent& event, ISignalSink& out) override;

    // Optional read-only diagnostics; see strategy_diagnostics.hpp. Not owned: the
    // observer must outlive this strategy or be detached with nullptr first. Call it
    // between events, never from inside the observer.
    void set_observer(IStrategyObserver* observer) { diagnostics_.attach(observer); }

    // Exceptions the observer threw that were swallowed (never forwarded to the engine).
    [[nodiscard]] std::uint64_t observer_failures() const noexcept { return diagnostics_.failures(); }

private:
    // The last established nonzero sign of (short - long).
    enum class Relation : std::uint8_t { ShortBelowLong, ShortAboveLong };

    // A running total plus the low-order bits each addition dropped (Neumaier's
    // variant of Kahan summation). value() is the total, accurate to a few ulps
    // however many terms were added and removed.
    struct RollingSum {
        double total{0.0};
        double lost{0.0};

        void add(double term) noexcept;
        [[nodiscard]] double value() const noexcept { return total + lost; }
    };

    // Per-symbol state, created for every allowlisted symbol up front.
    struct SymbolState {
        // The last min(accepted, long_window) closes. Oldest first while it
        // fills; once full a ring in which closes[next] is the oldest close,
        // the one the next bar replaces.
        std::vector<common::Price> closes{};
        std::size_t                next{0};

        RollingSum short_sum{};   // sum of the last short_window closes
        RollingSum long_sum{};    // sum of the last long_window closes

        std::optional<common::Timestamp> last_time{};   // exchange_time of the last accepted bar
        std::optional<Relation>          relation{};    // empty until a baseline is established

        void reset() noexcept;
    };

    // Adds an accepted close to the history and both sums. The only step that can
    // throw (history growth during warm-up) runs first, so a failure leaves
    // `state` untouched.
    void accept_close(SymbolState& state, common::Price close);

    // Empty when the averages are equal within the tolerance.
    [[nodiscard]] static std::optional<Relation> relate(double short_average,
                                                        double long_average) noexcept;

    [[nodiscard]] domain::TradeSignal make_signal(const domain::MarketEvent& event, Relation now,
                                                  double short_average, double long_average) const;

    // Diagnostics reporting. Each only builds a snapshot when an observer is attached
    // and never throws; none reads anything the decision did not already compute.
    [[nodiscard]] static std::string_view relation_label(std::optional<Relation> relation) noexcept;
    void report_ignored(const domain::MarketEvent& event, const SymbolState* state,
                        BarReason reason) noexcept;
    void report_warming_up(const domain::MarketEvent& event, const SymbolState& state) noexcept;
    // `now` is empty for equal averages; `before` is the stored relation before this bar.
    void report_evaluated(const domain::MarketEvent& event, const SymbolState& state,
                          BarReason reason, BarAction action, std::optional<Relation> before,
                          std::optional<Relation> now, double short_average,
                          double long_average) noexcept;

    MovingAverageCrossoverConfig config_;
    double max_close_;   // largest close accepted; see "Floating point"
    std::unordered_map<common::Symbol, SymbolState> states_{};
    ObserverSlot diagnostics_{};
};

}  // namespace trading_engine::strategy
