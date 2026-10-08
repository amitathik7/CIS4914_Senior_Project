# Mean reversion (rolling z-score)

`strategy::MeanReversionStrategy`
([header](../../include/trading_engine/strategy/mean_reversion_strategy.hpp),
[source](../../src/strategy/mean_reversion_strategy.cpp)).

Part of [Strategies](../STRATEGIES.md), which has the contract every strategy shares,
what a signal does and does not guarantee, and what is not yet integrated. The
choices in section 10 are **local, not team decisions**: ADR 0002 is *Proposed* and
`OQ#9` is open.

It asks for a **Buy** when a close is unusually far *below* the recent mean of
closes and a **Sell** when it is unusually far *above* it. It produces
**directional requests only**: it keeps no position, has no exit logic and does
not size against the portfolio (see section 7).

## 1. What it computes

For one symbol, with `N = lookback` and `c` the current accepted close, once `N`
closes have been accepted:

```
mean                = sum(close_i) / N
population_variance = sum((close_i - mean)^2) / N          (divides by N, not N-1)
standard_deviation  = sqrt(population_variance)
z                   = (c - mean) / standard_deviation
```

The sums run over the last `N` accepted closes **including the current one**.
The current close therefore pulls its own mean towards itself and widens its own
deviation. Two consequences worth knowing:

- `|z| <= sqrt(N - 1)` always. `lookback 2` can only ever give `|z| = 1`; the
  default `entry_threshold` of 2.0 needs `lookback >= 5` to be reachable. A
  threshold above `sqrt(N - 1)` is **accepted but can never fire**; the
  constructor does not reject it.
- Population (not sample) deviation is used on purpose, and is pinned by a test
  (the fixture's bar 4 gives -1.7320508, where sample deviation would give -1.5).

Cost: `O(N)` work per accepted bar and `O(N)` history per allowlisted symbol.

## 2. Configuration

`MeanReversionConfig`, passed to the constructor. Nothing is read from a file yet.

| Field | Type | Default | Rule |
|---|---|---|---|
| `strategy_id` | `std::string` | `"mean_reversion"` | Not empty. Becomes `TradeSignal::strategy_id`. Two instances in one engine need two ids. |
| `lookback` | `std::size_t` | `20` | At least 2. Counted in accepted bars, current bar included. |
| `entry_threshold` | `double` | `2.0` | Finite, and greater than `rearm_threshold`. |
| `rearm_threshold` | `double` | `0.5` | Finite, at least 0, strictly less than `entry_threshold`. |
| `requested_quantity` | `common::Quantity` (`std::int64_t`) | `1` | Positive: a **whole number** of shares (an exact integer count; a fraction cannot be written). |
| `symbols` | `std::vector<std::string>` | none | Not empty, no empty entry, no symbol twice. Matched exactly (case-sensitive) against `MarketEvent::symbol`. |

An invalid value throws `common::ConfigError` from the constructor, prefixed with
`MeanReversionStrategy:`, naming the field and showing the value
(`rearm_threshold must be less than entry_threshold (got rearm_threshold 2,
entry_threshold 1.5)`). Nothing is rounded, clamped or defaulted. The quantity and
symbol rules are the same code, with the same wording, as the crossover's.

## 3. Input assumptions

The same policy as the [crossover](moving_average_crossover.md#3-input-assumptions):

- **Only `MarketEventType::Bar`.** `event.price` is read as the bar's **close**.
- **The caller supplies finalized bars of one consistent interval per symbol.**
  `MarketEvent` has no interval or "final" field, so a half-formed bar or a mix of
  intervals looks exactly like a good one and silently corrupts the statistics.
- **`exchange_time` increases strictly, per symbol.** Symbols are independent.
- **The window counts accepted bars.** Gaps are never filled.

Everything else is **ignored, and an ignored event changes nothing**: not the last
timestamp, not the history, not the latch. That is a symbol not on the allowlist
(or matching only case-insensitively), any type but `Bar`, a missing, zero or
negative price (`Price` is an exact `Decimal`: it has no NaN or infinity), and an
`exchange_time` not later than the last accepted bar's for that symbol. There is no
upper limit on a close: any positive price is accepted (see section 5). A bad bar is not read as a neutral
observation either: it cannot rearm a latch.

## 4. The latch

Each symbol carries a latch: `Neutral`, `LowerExtreme` or `UpperExtreme`. It is an
**indicator state**: "which extreme has already been requested". It is **not a
position and not a record of fills**, and nothing here learns whether a request was
executed. It starts `Neutral` on every run.

On each accepted bar with a full window, the first row that applies wins:

| Condition | Action | Latch afterwards |
|---|---|---|
| window is **constant** (section 5) | nothing | `Neutral` |
| `z <= -entry_threshold` and latch is not `LowerExtreme` | emit one **Buy** | `LowerExtreme` |
| `z >= +entry_threshold` and latch is not `UpperExtreme` | emit one **Sell** | `UpperExtreme` |
| `abs(z) <= rearm_threshold` | nothing | `Neutral` |
| anything else | nothing | unchanged |

A window whose measurement fails (section 5, *Failure*) skips the table: nothing is
emitted and the latch is unchanged. The entry and rearm comparisons are **inclusive**.
Consequences:

- **No repeats.** Staying beyond a threshold, or drifting between the two bands,
  never repeats a request. A request repeats only after a bar inside the rearm band.
- **Direct flips.** `LowerExtreme` to `UpperExtreme` (or back) emits the opposite
  request at once, with no `Neutral` bar between.
- **No silent baseline.** The first full window may emit if it meets an entry
  threshold. This intentionally differs from the crossover, which swallows its first
  comparison.
- **Rearming closes nothing.** Entering the rearm band emits no order.

## 5. Numbers

`common::Price` is `common::Decimal`: an exact `int64` count of **millionths** of the
account currency (`common/decimal.hpp`; 150.02 is 150'020'000). The closes are kept
exactly and the **window is re-measured from them on every bar**; no running sum or
sum of squares is kept. So no error accumulates over a long run, and a bad window
cannot poison a later one: once its closes have left the window, they no longer
affect anything.

The statistics themselves are **derived real numbers, never money**. Each close is
first expressed as an **offset from the newest close, in exact `int64` arithmetic** (the
difference of two positive `int64` never overflows), and only the offset is converted to
a `double`: exact up to 2^53 millionths *whatever the price level*. Converting the
closes themselves would round every close above 2^53 millionths (about 9 * 10^9
currency units) to a multiple of 2, 4, ... 1024 millionths, merging distinct prices and
putting an error of up to about 2 * 10^-5 into `z`; with offsets the error is about
10^-16, relative to the spread rather than to the price (`MeanReversionNumerics.
DistinctPricesFarAbove2To53MicrosAreNotCollapsedOntoOneDouble`). The mean, standard
deviation and `z` are computed in `double`; the mean and standard deviation are reported
in currency units (divided by 10^6), and `z` is a dimensionless ratio, so neither the
scale nor the origin enters it. Nothing derived is ever turned back into a `Price`.

- **Overflow cannot happen.** A close is at most `INT64_MAX` (about 9.2 * 10^18), so no
  sum, square or `|z|` can overflow a `double`. Every positive close is accepted, and
  the reason `price_above_max_close` never occurs here (it is the crossover's, whose
  integer sums need a bound).
- **Cancellation.** The deviations are taken from a first-pass mean, and the mean is
  then corrected by the mean of those deviations (the corrected two-pass
  algorithm). The cancellation-prone `E[x^2] - E[x]^2` is not used, so a spread of 1
  millionth on a level of 1e9 millionths is still measured accurately.
- **Negligible variance.** A window is **constant** when
  `standard_deviation <= 1e-12 * mean`. Equal integer closes have a deviation of
  exactly 0; the tolerance only widens that to a spread under one part in 10^12 of the
  mean (one millionth at a price above about 1,000,000 currency units), which is
  treated as no deviation rather than as an extreme `z`. A constant window emits
  nothing and sets the latch to `Neutral`: it is a genuine observation of "no
  deviation". The tolerance is fixed, not a configuration option. Consequence: a step
  of one millionth on a level of 100 is a real deviation, and on a level of 2,000,000
  is below the tolerance.
- **Failure.** A window whose mean, deviation or `z` comes out non-finite emits
  nothing and leaves the latch **exactly as it was**: an unrelated numerical failure
  is not evidence that the price returned to normal, so it never rearms. With integer
  closes up to `INT64_MAX` every intermediate value is finite, so **this path is a guard
  that no input can reach, kept as a cheap defence; it has no direct test.**
- Do not build with `-ffast-math` or `/fp:fast`: reassociation would undo the
  correction step.

Threshold comparisons are exact in floating point. When `z` is mathematically
*equal* to a threshold, the computed `z` is exact only if the window's arithmetic is
exact in binary (the tests use `[10 10 10 10 15]` at exactly 2.0, where every number
is a small integer count of millionths); for an inexact window it may land an ulp
either side. Pick thresholds with that in mind.

**Status of the representation.** Scaled `int64` money at 10^6 with whole-share quantities is the direction
*recommended* in ADR 0004 section 12 (Proposed) and implemented as `common::Decimal` on the `adam/portfolio-manager` branch.
`OQ#11` is **still open**: `docs/OPEN_QUESTIONS.md` requires all four members to sign off, and no ADR or decision record
here shows that they did (a comment in that branch's `types.hpp` calls it agreed, but that is not recorded). Treat these
types as a **proposal in use**, not a ratified policy. Only `common/types.hpp` and the lines of section 5 that name
`Decimal` depend on it.

## 6. Worked example

`lookback = 4`, `entry_threshold = 1.5`, `rearm_threshold = 0.5`, closes
`10 10 10 6 9 2 30`, one bar per minute. The expected values were derived by hand
and checked independently with exact fractions; the tests assert them.

| Bar | Window (current in bold) | mean | population variance | z | Latch | Result |
|---|---|---|---|---|---|---|
| 1-3 | | | | | Neutral | warming up |
| 4 | 10 10 10 **6** | 9 | 3 | -1.7320508 | Neutral → Lower | **Buy** (`z <= -1.5`) |
| 5 | 10 10 6 **9** | 8.75 | 2.6875 | +0.1524986 | Lower → Neutral | rearm, no signal (`abs(z) <= 0.5`) |
| 6 | 10 6 9 **2** | 6.75 | 9.6875 | -1.5261167 | Neutral → Lower | **Buy** |
| 7 | 6 9 2 **30** | 11.75 | 117.1875 | +1.6858628 | Lower → Upper | **Sell**, a direct flip with no neutral bar |

Bar 4 is the first full window and already signals. Bar 5 shows the rearm; without
it (`rearm_threshold 0.1`) bar 6's Buy would be suppressed. Bar 7 is the direct
`LowerExtreme` to `UpperExtreme` transition. A repeat-suppression series,
`10 10 10 6 2 1 1 1 1 10`, buys at bar 4, stays silent through bars 5-8 (still
beyond the entry, then between the bands) and bar 9 (a constant window rearms),
and sells at bar 10.

## 7. Signals

One signal per trigger:

| Field | Value |
|---|---|
| `symbol` | The event's symbol. |
| `side` | `Buy` for the lower extreme, `Sell` for the upper. |
| `requested_quantity` | The configured whole-share quantity. |
| `order_type` | `OrderType::Market`. |
| `target_exposure`, `limit_price`, `confidence` | Unset. |
| `metadata` | See below. |
| `id`, `created_at`, `strategy_id` | Left unset by the strategy: the engine stamps them (see [Strategies](../STRATEGIES.md#1-what-every-strategy-gets-and-owes)). |

Metadata (all values are text; the derived statistics are the shortest text that
reads back as the exact `double`, never non-finite; the close is the exact decimal
text of the `Price`):

| Key | Value |
|---|---|
| `trigger` | `z_score_at_or_below_lower_entry` (Buy) or `z_score_at_or_above_upper_entry` (Sell). |
| `lookback` | The configured window size. |
| `close` | The signalling bar's close, exactly (`150.02`, `100.000001`: the decimal text of the integer, never a float). |
| `mean`, `standard_deviation`, `z_score` | The window statistics, current close included. |
| `entry_threshold`, `rearm_threshold` | The configured thresholds. |

**What a signal does not mean.** The strategy tracks no position and has **no
portfolio-aware exits**, so:

- A **Buy is a request**, not a statement that a position is open, and a **Sell is
  a request**, not an instruction to close one. Whether a Sell with nothing to sell
  is rejected, closes a long, or opens a short is for the stages downstream (see
  `OQ#11`). Mean reversion would normally exit when price returns to the mean;
  this strategy does not, and rearming emits nothing.
- It is **not sized against the portfolio**: the quantity is the configured number
  of shares on every signal.
- It **bypasses no risk check**: by design `RiskManager` is the next stage and sees it
  first. That stage is not implemented yet.
- A request is **not a trade**. The latch moves when a signal is *emitted*, before
  anything is known about it. If the engine or bus then loses it (the engine counts
  it as rejected or failed), it is **lost, not re-sent** on a later bar of the same
  excursion. A publication rejection is not an execution outcome, and the strategy
  never hears of it (ADR 0002 section 7; see
  [Strategies](../STRATEGIES.md#2-what-emitting-a-signal-does-and-does-not-guarantee)).

## 8. Lifecycle

`on_start()` clears every symbol's history, last timestamp and latch, so each run
starts cold: after a restart the same bars at the same timestamps are accepted
again and reproduce the same signals. A freshly constructed strategy is already
cold, so it can be driven directly, without an engine, which is how most of its
tests work. Memory is one `Decimal` (8 bytes) per accepted bar up to `lookback`, per
allowlisted symbol; the per-event path allocates nothing except while a history is
growing and when a signal is built.

## 9. Constructing and registering it

```cpp
#include <memory>

#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

using trading_engine::strategy::MeanReversionConfig;
using trading_engine::strategy::MeanReversionStrategy;

// `engine` is a trading_engine::strategy::StrategyEngine, still stopped.
engine.register_strategy(std::make_shared<MeanReversionStrategy>(
    MeanReversionConfig{
        .strategy_id        = "mean_reversion_20",    // unique among the engine's strategies
        .lookback           = 20,
        .entry_threshold    = 2.0,
        .rearm_threshold    = 0.5,
        .requested_quantity = 10,                     // shares per signal; a whole number
        .symbols            = {"AAPL", "MSFT"},
    }));
engine.start();   // calls on_start(), then subscribes to market data
```

`register_strategy()` throws `common::ValidationError` for a duplicate id, so a second
instance (say `lookback = 60`) needs its own `strategy_id`. Registering it next to the
crossover strategy, with unique ids, is shown in
[`../STRATEGIES.md`](../STRATEGIES.md#3-running-several-strategies); the
`MeanReversionEngineTest.TheDocumentedExampleBuildsRegistersAndTrades` engine test builds
that code.

## 10. Local implementation choices (not team decisions)

The whole-share quantity, market-order shape, exact symbol matching, strict timestamps
and `ConfigError` rules are the same choices as the
[crossover's](moving_average_crossover.md#10-local-implementation-choices-not-team-decisions),
and are not repeated.

| Choice | Why it was made | Status |
|---|---|---|
| Defaults `lookback 20`, `entry_threshold 2.0`, `rearm_threshold 0.5`. | Placeholders so a config is easy to write. **Not tuned, optimised or recommended.** | Local. |
| **Population** deviation, **current close included** in the window. | Simple and standard, and `\|z\| <= sqrt(N-1)` is then a hard bound. | Local. |
| An `entry_threshold` above `sqrt(lookback - 1)` is accepted and never fires. | The configuration rules were specified up front; this is documented (section 1) rather than rejected. | Local. Revisit if it bites. |
| A **latch** per symbol, not a position: a request repeats only after a rearm. | Stops a long excursion repeating the same order every bar, without inventing holdings. | Local. Position-aware exits wait on `OQ#9` / `OQ#11`. |
| The first full window may signal: **no silent baseline**. | A window already beyond the threshold is an observation; the crossover, by contrast, needs a previous side to compare. | Local. |
| **No exit** when price returns to the mean; rearming emits nothing. | The strategy cannot know whether a request filled, and a Sell is not "close". | Local. Blocked on signal semantics (`OQ#9`). |
| Constant window: emit nothing, latch to `Neutral`, tolerance `1e-12` relative to the mean, fixed. | A spread far below the mean's last digits is not a deviation, and dividing it by itself gives an arbitrary `z`. | Local. |
| No input bound: every positive `int64` close is accepted (section 5). | The statistics are taken in `double`, which cannot overflow on `int64` inputs. | Local. |
| A non-finite measurement leaves the latch untouched. | An unrelated failure is not a neutral reading. Not reachable for `int64` input (section 5). | Local. |
| `z` is compared **exactly** to the thresholds. | No fudge factor on a trading threshold. A tie is inclusive but can land an ulp either way for an inexact window. | Local. |
| Metadata keys (section 7). | Informational; nothing consumes them yet. | Local. |

## 11. Diagnostics (optional)

`set_observer()` attaches a read-only observer ([contract](../STRATEGIES.md#7-diagnostics-optional-for-developer-tools)).
Each `on_market_event()` call reports one snapshot. Reasons: the ignore reasons of section 3 in the order they are
checked (`not_a_bar`, `symbol_not_allowlisted`, `price_absent`, `price_invalid`, `time_not_after_last_accepted`;
there is no price ceiling, so `price_above_max_close` never occurs here), then `warming_up`, `measurement_failed`,
`constant_window`, `entry_buy`, `entry_sell`, `excursion_already_requested`, `inside_rearm_band` and `between_bands`.
Indicators: `mean`, `standard_deviation`, `z_score` (unavailable while warming up or ignored; `z_score` is also
unavailable for a constant window, where the statistics exist but z means nothing, and all three for a failed
measurement). States: `latch_before`, `latch_after` (`neutral`, `lower_extreme`, `upper_extreme`). The values are the
ones the decision used: on a signalling bar they equal the signal's `mean`, `standard_deviation` and `z_score`.
