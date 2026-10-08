# Moving-average crossover

`strategy::MovingAverageCrossoverStrategy`
([header](../../include/trading_engine/strategy/moving_average_crossover_strategy.hpp),
[source](../../src/strategy/moving_average_crossover_strategy.cpp)).

Part of [Strategies](../STRATEGIES.md), which has the contract every strategy shares,
what a signal does and does not guarantee, and what is not yet integrated. The
choices in section 10 are **local, not team decisions**: ADR 0002 is *Proposed* and
`OQ#9` is open.

## 1. What it computes

For one symbol, with `close(k)` the close of its `k`-th accepted bar:

```
SMA_n(k) = ( close(k) + close(k-1) + ... + close(k-n+1) ) / n      (includes the current bar)
short    = SMA_short_window(k)
long     = SMA_long_window(k)
```

`short - long` is positive when the short average is above the long one. Its
sign (with the equality rule of section 6) is the bar's **relationship**. It exists
only once `long_window` bars have been accepted.

## 2. Configuration

`MovingAverageCrossoverConfig`, passed to the constructor. Nothing is read from a
file yet.

| Field | Type | Default | Rule |
|---|---|---|---|
| `strategy_id` | `std::string` | `"sma_crossover"` | Not empty. Becomes `TradeSignal::strategy_id`. Two instances in one engine need two ids. |
| `short_window` | `std::size_t` | `5` | At least 1. Counted in accepted bars. |
| `long_window` | `std::size_t` | `20` | Greater than `short_window`. |
| `requested_quantity` | `common::Quantity` (`std::int64_t`) | `1` | Positive: a **whole number** of shares (an exact integer count; a fraction cannot be written). |
| `symbols` | `std::vector<std::string>` | none | Not empty, no empty entry, no symbol twice. Matched exactly (case-sensitive) against `MarketEvent::symbol`. |

An invalid value throws `common::ConfigError` from the constructor, naming the
field and showing the value (`requested_quantity must be a positive whole number
of shares (got 0)`). Nothing is rounded, clamped or defaulted.

## 3. Input assumptions

- **Only `MarketEventType::Bar`.** `event.price` is read as the bar's **close**.
- **The caller supplies finalized bars of one consistent interval per symbol.**
  `MarketEvent` has no interval or "final" field, so a half-formed bar or a mix
  of intervals looks exactly like a good one. The strategy cannot check this and
  does not try; feeding it bad bars gives meaningless averages.
- **`exchange_time` increases strictly, per symbol.** Symbols are independent, so
  two symbols may share a timestamp, and one symbol's clock never constrains
  another's.
- **The windows count accepted bars.** A gap in time is not filled in: bars an
  hour apart and bars a minute apart are equally "consecutive" to the strategy.

Everything else is **ignored, and an ignored event changes nothing**: not the
last timestamp, not the history, not the averages, not the crossover state.

| Ignored event | Why |
|---|---|
| A symbol not on the allowlist, or only matching case-insensitively | It is not this strategy's instrument. |
| Any type but `Bar` (`Trade`, `Quote`, `Status`, `Unknown`) | Only bars carry a close. |
| No `price`, or one that is zero or negative | Not a usable close. (`Price` is an exact `Decimal`: it has no NaN or infinity.) |
| A close above `max_close` (section 6) | Summing it could overflow. |
| An `exchange_time` not later than the last accepted bar's for that symbol | A duplicate, a replay, an out-of-order bar, or a corrected print (there is no revision marker to tell them apart). |

## 4. Warm-up, baseline and equal averages

1. **Warm-up.** Nothing is emitted until `long_window` bars have been accepted
   (the short window fills sooner, since it is smaller).
2. **Baseline.** The first nonzero relationship after warm-up is recorded and
   never signalled. A strategy that starts in the middle of a trend does not
   fire on its first bar.
3. **Crossover.** After that, a change of side is a crossover and emits one
   signal, on the bar where the relationship changes.
4. **Equal averages emit nothing and keep the last nonzero relationship.**
5. **Staying on one side never repeats a signal.**

| Last nonzero relationship | This bar | Result |
|---|---|---|
| none yet | short above or below long | Recorded as the baseline. No signal. |
| none yet | equal | Nothing. Still no baseline. |
| short **below** long | short **above** long | **Buy.** Now "above". |
| short **above** long | short **below** long | **Sell.** Now "below". |
| same side | same side | Nothing. |
| either | equal | Nothing. The last nonzero relationship stands. |

So *below, equal, below* is not a crossover, while *below, equal, above* is one,
signalled on the bar that is above. A series that starts flat (equal averages)
stays silent until it moves, and the first move is only a baseline.

## 5. Worked example

`short_window = 2`, `long_window = 3`, closes `3 2 1 2 3 4 3 2 1`, one bar per
minute. The expected result was derived by hand and independently checked, and
the unit test `SmaCrossoverCrossovers.TheSpecFixtureGivesABuyAtBarFiveAndASellAtBarEight`
asserts it.

| Bar | Close | short | long | short - long | Result |
|---|---|---|---|---|---|
| 1 | 3 | | | | warming up |
| 2 | 2 | | | | warming up |
| 3 | 1 | 1.5 | 2 | -0.5 | below: the baseline, silent |
| 4 | 2 | 1.5 | 1.667 | -0.167 | below |
| 5 | 3 | 2.5 | 2 | +0.5 | above: **Buy** (`short_sma` 2.5, `long_sma` 2) |
| 6 | 4 | 3.5 | 3 | +0.5 | above |
| 7 | 3 | 3.5 | 3.333 | +0.167 | above |
| 8 | 2 | 2.5 | 3 | -0.5 | below: **Sell** (`short_sma` 2.5, `long_sma` 3) |
| 9 | 1 | 1.5 | 2 | -0.5 | below |

## 6. Numbers

`common::Price` is `common::Decimal`: an exact `int64` count of **millionths** of the
account currency (`common/decimal.hpp`; 150.02 is 150'020'000). Nothing in a decision
passes through a `double`.

- **Rolling sums are exact.** Each window keeps a running sum updated in O(1) per bar
  (add the new close, subtract the one leaving). They are integers, so nothing is
  compensated, nothing drifts over a long run, and a close of 1 millionth is never lost
  next to a close of billions.
- **Equality is exact, with no tolerance.** The averages are compared **without
  dividing**, by cross-multiplication:
  `short_sum * long_window` versus `long_sum * short_window`. Equal means equal, so a
  tie such as `(0.4 + 0.2) / 2` against `(0.3 + 0.4 + 0.2) / 3` is a tie, never a
  one-ulp "crossing" (a floating-point version needs a fudge factor for that; here
  there is none to tune).
- **Overflow is prevented, not detected.** `Decimal` arithmetic is unchecked, so the
  bound lives here: a close above `max_close = INT64_MAX / long_window^2` millionths is
  ignored like any other invalid bar (reason `price_above_max_close`), which keeps every
  sum and both products inside `int64`. For the default 5/20 windows that is about
  2.3 * 10^16 millionths, i.e. 23 billion currency units. Construction is refused
  (`ConfigError`) if `long_window` is so large that `max_close` would be under one
  currency unit: more than 3,037,000. The bound is computed in unsigned arithmetic, so
  a `long_window` of 2^63 or more cannot wrap into a huge accepted bound. Why the bound
  suffices: with `S < L` and every close at most `M = INT64_MAX / L^2`, a window sum is
  at most `L * M` (and the transient `S + 1` closes of the warm-up at most the same),
  and both products are at most `L * S * M <= INT64_MAX * S / L < INT64_MAX`; the
  difference `close - leaving close` of two values in `[1, M]` is within `+-M`. The
  boundary is exercised against an independent 128-bit reference
  (`SmaCrossoverOverflowBound`).
- **`short_sma` and `long_sma` are derived.** They are `sum / (window * 10^6)`: one
  division of two exact values, in currency units, written as the shortest text that
  reads back as the same `double`. They are for reporting and for the signal metadata
  only; they never feed back into a decision and are never turned back into a `Price`.

**Status of the representation.** Scaled `int64` money at 10^6 with whole-share quantities is the direction
*recommended* in ADR 0004 section 12 (Proposed) and implemented as `common::Decimal` on the `adam/portfolio-manager` branch.
`OQ#11` is **still open**: `docs/OPEN_QUESTIONS.md` requires all four members to sign off, and no ADR or decision record
here shows that they did (a comment in that branch's `types.hpp` calls it agreed, but that is not recorded). Treat these
types as a **proposal in use**, not a ratified policy. Only `common/types.hpp` and the lines of this section that name
`Decimal` depend on it.

## 7. Signals

One signal per crossover:

| Field | Value |
|---|---|
| `symbol` | The event's symbol. |
| `side` | `Buy` when short crosses above long, `Sell` when it crosses below. |
| `requested_quantity` | The configured whole-share quantity. |
| `order_type` | `OrderType::Market`. |
| `target_exposure`, `limit_price`, `confidence` | Unset. |
| `metadata` | See below. |
| `id`, `created_at`, `strategy_id` | Left unset by the strategy: the engine stamps them. `created_at` is the engine clock at emit time (a replay advances it to the bar's time), not the bar's `exchange_time`. |

Metadata (all values are text):

| Key | Value |
|---|---|
| `trigger` | `short_crossed_above_long` (Buy) or `short_crossed_below_long` (Sell). |
| `short_window`, `long_window` | The configured window sizes. |
| `short_sma`, `long_sma` | Both averages on the signalling bar, in currency units, as the shortest text that reads back as exactly the same `double` (`2.5`, `2`, `1.3333333333333333`). Derived numbers, not money (section 6). |

**What a signal does not mean.** The strategy tracks no position, so:

- A **Buy is a request**, not a statement that a position is open, and a **Sell
  is a request**, not an instruction to close one. Whether a Sell with nothing to
  sell is rejected, closes a long, or opens a short is for the stages
  downstream (see `OQ#11`).
- It is **not sized against the portfolio**: the quantity is the configured
  number of shares on every signal.
- It **bypasses no risk check**: by design `RiskManager` is the next stage and sees it
  first. That stage is not implemented yet.

The crossover state is committed *before* the signal is emitted. If the engine
or bus then loses the signal (the engine counts it as rejected or failed), it is
**lost, not re-sent** on a later bar: the next signal is the next crossover. That is
the rule the engine itself applies, and an emission is not a delivery, an approval or
a fill (see [`../STRATEGIES.md`](../STRATEGIES.md#2-what-emitting-a-signal-does-and-does-not-guarantee)).

## 8. Lifecycle

`on_start()` clears every symbol's history, sums, last timestamp and baseline, so
each run starts cold: after a restart the same bars at the same timestamps are
accepted again and reproduce the same signals. A freshly constructed strategy is
already cold, so it can also be driven directly, without an engine, which is how
most of its tests work. Memory is one `Decimal` (8 bytes) per accepted bar up to
`long_window`, per allowlisted symbol; the per-event path allocates nothing
except while a history is still growing and when a signal is built.

## 9. Constructing and registering it

```cpp
#include <memory>

#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

using trading_engine::strategy::MovingAverageCrossoverConfig;
using trading_engine::strategy::MovingAverageCrossoverStrategy;

// `engine` is a trading_engine::strategy::StrategyEngine, still stopped.
engine.register_strategy(std::make_shared<MovingAverageCrossoverStrategy>(
    MovingAverageCrossoverConfig{
        .strategy_id        = "sma_5_20",   // unique among the engine's strategies
        .short_window       = 5,
        .long_window        = 20,
        .requested_quantity = 10,           // shares per signal; a whole number
        .symbols            = {"AAPL", "MSFT"},
    }));
engine.start();   // calls on_start(), then subscribes to market data
```

`register_strategy()` throws `common::ValidationError` for a duplicate id, so a
second instance (say a faster pair) needs its own `strategy_id`. The
`SmaCrossoverEngineTest.TheDocumentedExampleBuildsRegistersAndTrades` engine test builds
exactly this. Registering it next to the mean-reversion strategy is shown in
[`../STRATEGIES.md`](../STRATEGIES.md#3-running-several-strategies).

## 10. Local implementation choices (not team decisions)

| Choice | Why it was made | Status |
|---|---|---|
| Quantity is a **whole number of shares** (`int64`); a fraction cannot be written. | Matches the direction proposed in ADR 0002 section 8, and `Quantity` is an `int64` share count in the fixed-point types (see the status in section 6). | ADR 0002 is *Proposed*; `OQ#11` is open. |
| Signals are **market orders**: `order_type = Market`, `limit_price` unset. | The order-shape fields ADR 0002 proposes, used as proposed. | Fields are *Proposed*. No component reads them yet. |
| An explicit **quantity** on every signal, not `target_exposure`. | The requirement for this strategy: a fixed request, with no sizing against portfolio equity. | `OQ#9` (quantity vs target exposure vs delta) is open. |
| The first nonzero relationship is a **silent baseline**. | A strategy started mid-trend should not fire on its first bar. | Local. |
| Equal averages keep the last nonzero relationship. | Avoids a false crossover on a plateau. | Local. |
| Averages are compared **exactly** (integer cross-multiplication); there is no tolerance. | See section 6. | Local. |
| Strict timestamp increase; duplicates and older bars are **ignored**. | `MarketEvent` has no revision marker, so a corrected print cannot be told from a replay. | Local; revisit with `OQ#1`. |
| A close above `max_close` is ignored. | Keeps every sum and product inside `int64` by construction (section 6). | Local. |
| Bad configuration throws **`common::ConfigError`**; a repeated symbol is an error. | The error type reserved for configuration; a repeat is almost certainly a mistake. | Local. |
| Symbols match **exactly**. | Normalising symbols is `MarketDataService`'s job. | Local. |
| Signal metadata keys (section 7). | Informational; nothing consumes them yet. | Local. |

## 11. Diagnostics (optional)

`set_observer()` attaches a read-only observer ([contract](../STRATEGIES.md#7-diagnostics-optional-for-developer-tools)).
Each `on_market_event()` call reports one snapshot. Reasons: the ignore reasons of section 3 in the order
they are checked (`not_a_bar`, `symbol_not_allowlisted`, `price_absent`, `price_invalid`, `price_above_max_close`,
`time_not_after_last_accepted`), then `warming_up`, `averages_equal`, `baseline_established`, `same_side`,
`crossover_buy` and `crossover_sell`. Indicators: `short_sma`, `long_sma`, `short_minus_long` (unavailable until
the long window is full). States: `relation_before`, `relation_now`, `relation_after` (`none`, `short_above_long`,
`short_below_long`, `equal`, `not_evaluated`). The values are the ones the decision used: on a signalling bar they
equal the signal's `short_sma` and `long_sma`.
