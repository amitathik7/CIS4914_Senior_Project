#pragma once

// -----------------------------------------------------------------------------
//  Strategy diagnostics -- an OPTIONAL, READ-ONLY account of what a strategy did
//  with each market event it was handed.
//
//  Why it exists. A strategy that stays silent leaves no trace: warming up, an
//  ignored event, an equal-averages bar and a suppressed repeat all look the same
//  from outside. The statistics behind a decision (the averages, the z-score) are
//  private state. This header lets a developer tool ask "what did you decide on
//  this bar, why, and from which numbers" without recomputing anything: the
//  strategy reports the values its own decision used.
//
//  What it is NOT. It is not part of IStrategy or ISignalSink (those, TradeSignal,
//  MarketEvent and the event bus are untouched), it is not a logging facade, and
//  it never feeds back into the strategy. A strategy never depends on it for a
//  decision. Only the concrete reference strategies implement it today.
//
//  Contract for an IStrategyObserver (a strategy implementer relies on all of it):
//   * on_bar() is called synchronously, on the thread running on_market_event(),
//     exactly ONCE per on_market_event() call that returns normally, after the
//     strategy's state has been updated and BEFORE any signal is handed to
//     emit(). A call that throws (allocation failure while building a signal)
//     reports nothing.
//   * It receives const references to values built for the call. Nothing in them
//     points into the strategy's state, so an observer cannot change it. The
//     string_views are valid only for the duration of the call: copy what you keep.
//   * It must be quick and must NOT call back into anything that drives the
//     strategy: no on_market_event(), on_start() or set_observer() on the same
//     strategy, and no StrategyEngine or event-bus operation (start, stop,
//     register_strategy, publish, subscribe, unsubscribe). Re-entering the SAME
//     strategy is refused with std::logic_error (thrown to the observer, state
//     untouched). Re-entering the engine is not defended against beyond the engine's
//     own rules (a nested market event is dropped and counted; nested start() and
//     stop() are unsupported).
//   * It should not throw. If it does, the strategy swallows the exception, counts
//     it in observer_failures() and carries on: the decision, the strategy's state,
//     the emitted signals and the engine's counters are exactly what they would
//     have been with no observer. (The failure count is a diagnostics statistic; it
//     is not forwarded to the engine.)
//   * set_observer() may be called only between callbacks (never from on_bar()),
//     and the observer must outlive the strategy or be detached with
//     set_observer(nullptr) first. on_start() does not detach it.
//
//  Cost when no observer is set: one pointer test per event, before any work.
//
//  Thread-safety: none, like the strategies themselves.
// -----------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "trading_engine/domain/market_event.hpp"

namespace trading_engine::strategy {

enum class BarVerdict : std::uint8_t {
    Ignored,     // the event changed nothing (see BarReason)
    WarmingUp,   // accepted, but the window is not yet full
    Evaluated    // accepted, window full, the decision rules ran
};

// What the strategy handed to emit() on this call. None is NOT a "hold" signal:
// no TradeSignal exists for it.
enum class BarAction : std::uint8_t { None, Buy, Sell };

// Stable machine-readable reasons. The first six are the ignore checks, in the
// order the strategies apply them; the rest describe an accepted bar.
enum class BarReason : std::uint8_t {
    NotABar,
    SymbolNotAllowlisted,
    PriceAbsent,
    PriceInvalid,               // not finite, zero or negative
    PriceAboveMaxClose,         // finite and positive, but above the strategy's input limit
    TimeNotAfterLastAccepted,   // duplicate, replay or older bar for that symbol

    WarmingUp,

    // Moving-average crossover
    BaselineEstablished,
    AveragesEqual,
    SameSide,
    CrossoverBuy,
    CrossoverSell,

    // Mean reversion
    MeasurementFailed,
    ConstantWindow,
    EntryBuy,
    EntrySell,
    ExcursionAlreadyRequested,
    InsideRearmBand,
    BetweenBands
};

[[nodiscard]] constexpr std::string_view to_string(BarVerdict verdict) noexcept {
    switch (verdict) {
        case BarVerdict::Ignored:   return "ignored";
        case BarVerdict::WarmingUp: return "warming_up";
        case BarVerdict::Evaluated: return "evaluated";
    }
    return "invalid";
}

[[nodiscard]] constexpr std::string_view to_string(BarAction action) noexcept {
    switch (action) {
        case BarAction::None: return "none";
        case BarAction::Buy:  return "buy";
        case BarAction::Sell: return "sell";
    }
    return "invalid";
}

[[nodiscard]] constexpr std::string_view to_string(BarReason reason) noexcept {
    switch (reason) {
        case BarReason::NotABar:                   return "not_a_bar";
        case BarReason::SymbolNotAllowlisted:      return "symbol_not_allowlisted";
        case BarReason::PriceAbsent:               return "price_absent";
        case BarReason::PriceInvalid:              return "price_invalid";
        case BarReason::PriceAboveMaxClose:        return "price_above_max_close";
        case BarReason::TimeNotAfterLastAccepted:  return "time_not_after_last_accepted";
        case BarReason::WarmingUp:                 return "warming_up";
        case BarReason::BaselineEstablished:       return "baseline_established";
        case BarReason::AveragesEqual:             return "averages_equal";
        case BarReason::SameSide:                  return "same_side";
        case BarReason::CrossoverBuy:              return "crossover_buy";
        case BarReason::CrossoverSell:             return "crossover_sell";
        case BarReason::MeasurementFailed:         return "measurement_failed";
        case BarReason::ConstantWindow:            return "constant_window";
        case BarReason::EntryBuy:                  return "entry_buy";
        case BarReason::EntrySell:                 return "entry_sell";
        case BarReason::ExcursionAlreadyRequested: return "excursion_already_requested";
        case BarReason::InsideRearmBand:           return "inside_rearm_band";
        case BarReason::BetweenBands:              return "between_bands";
    }
    return "invalid";
}

// One named number. `value` is empty when the strategy did not compute it for this
// event (warming up, ignored, a constant window...): unavailable, never zero.
struct Indicator {
    std::string_view     name{};
    std::optional<double> value{};
};

// One named state label ("short_above_long", "neutral", ...).
struct StateLabel {
    std::string_view name{};
    std::string_view value{};
};

// A self-contained value: the decision, the numbers it used and the state before
// and after. Built only when an observer is attached.
struct BarSnapshot {
    static constexpr std::size_t kMaxIndicators = 4;
    static constexpr std::size_t kMaxStates     = 3;

    std::string_view strategy_id{};
    BarVerdict       verdict{BarVerdict::Ignored};
    BarReason        reason{BarReason::NotABar};
    BarAction        action{BarAction::None};

    // Closes now held for the event's symbol (at most window_size), unchanged by an
    // ignored event. Empty when the event never reached that symbol's state (not a
    // bar, or a symbol the strategy does not trade).
    std::optional<std::size_t> window_fill{};
    std::size_t                window_size{0};   // bars needed before the first decision

    std::array<Indicator, kMaxIndicators> indicators{};
    std::size_t                           indicator_count{0};
    std::array<StateLabel, kMaxStates>    states{};
    std::size_t                           state_count{0};

    // Silently ignored when full: the capacities fit the reference strategies, and
    // the tests pin their counts.
    void add_indicator(std::string_view name, std::optional<double> value) noexcept {
        if (indicator_count < kMaxIndicators) {
            indicators[indicator_count++] = Indicator{name, value};
        }
    }
    void add_state(std::string_view name, std::string_view value) noexcept {
        if (state_count < kMaxStates) {
            states[state_count++] = StateLabel{name, value};
        }
    }

    [[nodiscard]] std::span<const Indicator> indicator_list() const noexcept {
        return {indicators.data(), indicator_count};
    }
    [[nodiscard]] std::span<const StateLabel> state_list() const noexcept {
        return {states.data(), state_count};
    }
};

class IStrategyObserver {
public:
    virtual ~IStrategyObserver();

    // See the contract at the top of this file.
    virtual void on_bar(const domain::MarketEvent& event, const BarSnapshot& snapshot) = 0;
};

// The observer plumbing a strategy embeds, so the exception and re-entry policy is
// written once. Not for use by an observer.
class ObserverSlot {
public:
    // nullptr detaches. Throws std::logic_error when called from inside on_bar().
    void attach(IStrategyObserver* observer) {
        refuse_inside_callback("set_observer");
        observer_ = observer;
    }

    [[nodiscard]] bool attached() const noexcept { return observer_ != nullptr; }

    // Exceptions an observer threw and the strategy swallowed.
    [[nodiscard]] std::uint64_t failures() const noexcept { return failures_; }

    // Throws std::logic_error when an observer is running: used by the strategy's
    // own entry points, which an observer must not call.
    void refuse_inside_callback(std::string_view what) const {
        if (in_callback_) {
            throw std::logic_error("strategy observer contract violated: " + std::string{what} +
                                   " called from inside IStrategyObserver::on_bar()");
        }
    }

    // `build` returns the BarSnapshot and runs only when an observer is attached.
    // Never throws: whatever the observer throws is counted and dropped.
    template <class BuildSnapshot>
    void notify(const domain::MarketEvent& event, BuildSnapshot&& build) noexcept {
        if (observer_ == nullptr) {
            return;
        }
        try {
            const BarSnapshot snapshot = build();
            in_callback_ = true;
            observer_->on_bar(event, snapshot);
        } catch (...) {
            ++failures_;
        }
        in_callback_ = false;
    }

private:
    IStrategyObserver* observer_{nullptr};
    std::uint64_t      failures_{0};
    bool               in_callback_{false};
};

}  // namespace trading_engine::strategy
