# Strategies

> Status: the **StrategyEngine** and two reference strategies, a **moving-average
> crossover** and a **rolling z-score mean reversion**, are implemented and tested
> **against test doubles only** (`RecordingEventBus`, `ManualClock`). The production
> event bus, market-data service, risk manager, execution simulator, portfolio and
> application run loops are not implemented, so nothing here has run in a pipeline.
> The custom ML strategy is not started.
>
> **Nothing in the "local choices" tables is a team decision.**
> [ADR 0002](adr/0002-strategy-risk-signal-contract.md) is still *Proposed* and
> `OQ#9` in [`OPEN_QUESTIONS.md`](OPEN_QUESTIONS.md) is still open. Each strategy
> page records the choices that implementation made so they can be confirmed or
> overturned when those are ratified. The defaults are local placeholders, **not
> tuned or recommended trading parameters**.

| Page | Contents |
|---|---|
| This page | The shared contract, what a signal does and does not guarantee, running several strategies, threading and bus assumptions, what is not integrated, and what the other component owners must provide. |
| [Moving-average crossover](strategies/moving_average_crossover.md) | Configuration, input rules, baseline and equality rules, floating point, signals, worked example, local choices. |
| [Mean reversion](strategies/mean_reversion.md) | Configuration, the z-score and its bounds, the latch, numerical policy, signals, worked example, local choices. |
| [Strategy Lab](STRATEGY_LAB.md) | An optional developer tool that replays bars through the real engine and shows each bar's decision (see section 7). |

## 1. What every strategy gets, and owes

A strategy implements `strategy::IStrategy`
([`strategy.hpp`](../include/trading_engine/strategy/strategy.hpp)) and is run
by the `StrategyEngine`
([`strategy_engine.hpp`](../include/trading_engine/strategy/strategy_engine.hpp)).
The parts that shape every strategy:

- **Every strategy sees every event.** The engine does not route by symbol or
  event type, so a strategy ignores what it does not trade.
- **The engine owns identity.** `emit()` copies the signal and overwrites
  `id`, `created_at` (the injected `IClock`) and `strategy_id` (the strategy's
  `id()`, read once at registration). Ids must be non-empty and unique per
  engine.
- **A signal is a request.** A strategy decides *when*; sizing against the
  portfolio, risk limits and execution are downstream stages.
- **`on_start()` is where a run's state is reset.** A restarted engine calls it
  again on the same object.
- **Callbacks never overlap**, so a strategy needs no locking.

## 2. What emitting a signal does and does not guarantee

`ISignalSink::emit()` returns nothing. After a strategy calls it, exactly one of
these happened, and **only the engine's counters record which** (`stats()`,
`strategy_stats()`):

| Outcome | Counter | Meaning |
|---|---|---|
| `bus.publish()` returned `true` | `signals_published` | The bus **accepted** the event. |
| `bus.publish()` returned `false` | `signals_rejected` | The bus refused it (full or shut down). The signal is **lost**. |
| `bus.publish()` threw | `publish_errors` | A bus fault, absorbed. The signal is **lost**, and the strategy is not blamed. |
| `emit()` called outside its callback | `signals_dropped` | Never offered to the bus, and no id consumed. |

What follows, and what does **not**:

- **An attempted emission is not guaranteed delivery.** The engine neither
  retries nor buffers. A refused signal is gone, and the id it consumed is skipped,
  never reused.
- **Accepted is not delivered, approved or executed.** `signals_published` means the
  bus took the event. Whether a `RiskManager` ever sees it depends on the production
  bus, which does not exist yet; the test double delivers synchronously and models
  nothing else. Risk approval and execution are separate stages with their own
  outcomes, and none of them is reported back.
- **The strategy never learns the outcome**, nor the `SignalId` it was given, so it
  cannot retry, cancel or correlate (ADR 0002 section 7).
- **Both reference strategies commit their own state when they emit**, before
  anything is known about the signal: the crossover's last relationship, and the
  mean reversion's latch. A refused signal is therefore **not re-offered** on later
  bars of the same condition (until the next crossover, or until the latch rearms).
  This is deliberate: the alternative, re-emitting every bar until the bus accepts,
  would flood a bus that is already refusing. It is pinned by
  `SmaCrossoverEngineTest.ASignalTheBusRefusesIsLostNotResent`,
  `MeanReversionEngineTest.ASignalTheBusRefusesIsLostNotResentWhileTheExcursionLasts`
  and `CombinedStrategies.ARefusedSignalIsCountedLostAndNeverRetriedAndProcessingContinues`.
- **Failures are isolated.** An exception from one strategy is counted against that
  strategy (`errors` per strategy, `strategy_errors` in total, with the text in
  `last_error`), and the others still receive the event
  (`CombinedStrategies.AThrowingNeighbourCostsTheRealStrategiesNothing`). A strategy that
  throws after emitting keeps the signals it already emitted.

**Production follow-up (not done here, and not designed here):** a delivery path
needs a decision on what a strategy is told (`emit()` returning the assigned id, or an
acknowledgement callback; ADR 0002 section 7), whether a refused signal is retried,
buffered or surfaced, and a backpressure policy for the bus (`DATA_FLOW.md` records it
as undecided). Until then a refused signal is silently lost to the strategy and visible
only in the engine's counters.

## 3. Running several strategies

Strategies in one engine only need **different `strategy_id` values**. The defaults
already differ (`sma_crossover` and `mean_reversion`), but name instances explicitly
as soon as there is more than one of a kind. Registration happens while the engine is
stopped, and registration order is delivery order, and so also the order in which
signals from one event are published.

```cpp
#include <memory>

#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

using trading_engine::strategy::MeanReversionConfig;
using trading_engine::strategy::MeanReversionStrategy;
using trading_engine::strategy::MovingAverageCrossoverConfig;
using trading_engine::strategy::MovingAverageCrossoverStrategy;

// `engine` is a trading_engine::strategy::StrategyEngine, still stopped.
engine.register_strategy(std::make_shared<MovingAverageCrossoverStrategy>(
    MovingAverageCrossoverConfig{
        .strategy_id        = "sma_5_20",             // unique among the engine's strategies
        .short_window       = 5,
        .long_window        = 20,
        .requested_quantity = 10,
        .symbols            = {"AAPL", "MSFT"},
    }));
engine.register_strategy(std::make_shared<MeanReversionStrategy>(
    MeanReversionConfig{
        .strategy_id        = "mean_reversion_20",    // a different id from the one above
        .lookback           = 20,
        .entry_threshold    = 2.0,
        .rearm_threshold    = 0.5,
        .requested_quantity = 10,                     // shares per signal; a whole number
        .symbols            = {"AAPL", "MSFT"},
    }));
engine.start();   // calls on_start() on both, then subscribes to market data
```

`register_strategy()` throws `common::ValidationError` for a duplicate id, so a
second instance of either kind (say `lookback = 60`) needs its own `strategy_id`. The
strategies do not coordinate: they may disagree on direction on the same symbol at the
same bar, and nothing deduplicates or nets their requests (the engine tests show
exactly that). `MeanReversionEngineTest.TheDocumentedExampleBuildsRegistersAndTrades`
builds the code above, and `CombinedStrategies.*` run both on a deterministic
two-symbol stream with independently derived expectations: the published order, unique
ids, clock-stamped times, independence between symbols and between strategies, replay
with fresh instances, a restart, a refused signal, and a throwing neighbour.

## 4. Threading, shutdown and bus assumptions

The engine adds no threads and no locks. It relies on the single-threaded callback
model of `strategy.hpp`; the full list is in the
[`strategy_engine.hpp`](../include/trading_engine/strategy/strategy_engine.hpp)
header. The assumptions that are **not** guaranteed by the engine itself, and that the
event-bus owner must supply:

1. **`unsubscribe()` is a barrier.** When it returns, no invocation of the market-data
   handler is running or will begin. `stop()` changes engine state only after that.
   The handler holds a token that `stop()` invalidates first, which makes a *late
   start* of a stale handler harmless; it cannot make an invocation already inside the
   engine safe.
2. **Handler invocations are serialized**, with each other and with `start()`,
   `stop()` and `register_strategy()`. The engine's state, the active-sink pointer and
   its counters are plain (non-atomic) fields.
3. **A `subscribe()` that throws leaves no live subscription.**
4. **The bus outlives the engine**, and is drained before the engine is stopped.
5. **The bus lets a handler publish.** The engine publishes `Signal` events from inside
   its `MarketData` handler, and does not expect a synchronous bus to feed market
   events back into it (a nested event is dropped and counted).

**What the tests do and do not show.** The engine tests use a synchronous, single-
threaded double, so they check the *ordering* that makes assumption 1 sufficient
(withdraw first, then change state) and the behaviour of stale handlers, but they
cannot show race freedom. Neither can a sanitizer pass: AddressSanitizer finds memory
errors, not data races, and no ThreadSanitizer run exists here (only MSVC is
available). No concrete defect was found, and none of this is proven for a threaded
bus. It has to be re-examined against the production bus's actual delivery and
unsubscribe behaviour when that bus exists.

## 5. Not yet integrated

- **Run only against a test bus.** The production `InProcessEventBus`,
  `MarketDataService`, `RiskManager`, execution, portfolio accounting and the
  application run loops are not implemented. Nothing here has run in a full pipeline.
- **No routing index.** The engine delivers every event to every strategy, and each
  discards most of them. A symbol-interest declaration on `IStrategy` is a TODO.
- **Silent ignoring.** An ignored event leaves no counter or log line. That waits
  on a logging facade (`OQ#8`) and the `SystemMetrics` wiring. The likeliest way
  to hit it is a producer that never sets `exchange_time`: every bar then carries
  the epoch, so only each symbol's first bar is accepted and the rest are dropped
  as duplicates, with nothing to show for it. (An optional, read-only diagnostics
  observer, section 7, can report the reason for each event to a developer tool; it
  is not a counter, a log line or a metric.)
- **The input contract is trusted, not checked.** Bars must be finalized, of one
  interval per symbol, with strictly increasing `exchange_time`; `MarketEvent` has no
  field to verify any of that (`OQ#1`).
- **No feedback.** `ISignalSink::emit()` returns nothing: no `SignalId`, no outcome, no
  cancellation, no fills (section 2 and ADR 0002 section 7).
- **No bar time on the signal.** `created_at` is the engine clock at emit time, and the
  metadata does not carry the triggering bar's `exchange_time`.
- **No exits, no sizing.** Neither strategy tracks a position. Requests are a fixed
  whole-share quantity, which `RiskManager` and execution must treat as a request.
- **Configuration is built in code.** There is no file or environment loading.
- **Engine counters are not exported.** `Stats` is not yet forwarded to
  `analytics::SystemMetrics`, and failure text goes nowhere (`OQ#8`).

## 6. What the other component owners need to provide

**Market data (`MarketDataService`, historical replay).**

- Publish `EventType::MarketData` events whose payload is a `domain::MarketEvent`; any
  other payload is dropped and counted (`events_dropped`).
- For the strategies to act, deliver `MarketEventType::Bar` events with `price` set to
  the **finalized close**, one consistent interval per symbol. A partial or mixed-
  interval bar is indistinguishable from a good one and silently corrupts both
  strategies' windows.
- Set `exchange_time`, strictly increasing per symbol. Duplicates, replays and
  corrected prints are ignored (there is no revision marker), and an unset time makes
  every bar after the first look like a duplicate.
- Normalise symbols to one canonical spelling. Strategies match exactly,
  case-sensitively, against their configured allowlist.
- In a replay, advance the injected clock to the bar's time before delivery if
  `created_at` should equal the bar's time (the tests do).

**Event bus (`InProcessEventBus`).**

- Meet the five assumptions in section 4, in particular the `unsubscribe()` barrier and
  serialized handler invocation, and document them in `event_bus.hpp`.
- `publish()` returns `false` when the event is refused and may throw; both are absorbed
  and counted, and the signal is lost (section 2), so decide and document the full-queue
  policy deliberately.
- Own `Event::sequence` and `enqueued_at`; the engine leaves them unset.
- Allow publishing from inside a handler without deadlock.

**Risk and execution (`RiskManager`, `ExecutionSimulator`).**

- Consume `EventType::Signal` events carrying a `domain::TradeSignal`. From these
  strategies it has `side` `Buy` or `Sell` (never `Flat`), `requested_quantity` a whole
  number of shares, `order_type` `Market`, and no `limit_price`, `target_exposure` or
  `confidence`. `metadata` is informational text. These fields use the *Proposed*
  shape of ADR 0002.
- A signal is a **request**. A Sell may arrive with no position to sell, and two
  strategies may request opposite sides of one symbol at the same bar. What each means
  (reject, close a long, open a short) is for these stages, and is unresolved
  (`OQ#9`, `OQ#11`).
- `SignalId` is unique **per engine object**, counts from 1 each process, and skips the
  ids of refused signals. It is not globally unique, so anything that persists or
  correlates signals across runs needs run scoping.
- Report outcomes visibly. The strategies will never hear about a rejection or a fill.

## 7. Diagnostics (optional, for developer tools)

Both reference strategies can report, to an optional read-only observer, what they
decided on each event and from which numbers
([`strategy_diagnostics.hpp`](../include/trading_engine/strategy/strategy_diagnostics.hpp)):
the verdict (`ignored`, `warming_up`, `evaluated`), a stable reason code, the action taken
(`none` is **not** a hold signal: no `TradeSignal` exists for it), the window fill, the
indicator values (empty when unavailable, never zero) and the state before and after. It is
attached with `set_observer()` on the concrete strategy; `IStrategy`, `ISignalSink`,
`TradeSignal`, `MarketEvent` and the bus are unchanged.

- **It changes nothing a strategy decides.** With an observer attached, the signals (every
  field), the engine's counters and the strategy's later behaviour are identical to a run
  without one. This is checked on seeded random streams of bad prices, repeated timestamps,
  unlisted symbols and non-bar events, directly and through the real engine; it is not only argued.
- **An observer that throws is swallowed and counted** in `observer_failures()`. The exception
  never reaches the engine and is not a strategy error.
- **An observer must not re-enter** the strategy, the engine or the bus. Calling
  `on_market_event()`, `on_start()` or `set_observer()` on the same strategy from inside the
  callback throws `std::logic_error` to the observer and touches no state.
- With no observer attached the cost is one pointer test per event.

The [Strategy Lab](STRATEGY_LAB.md) is its first user. It is not a logging facade (`OQ#8`).

## 8. Planned

The custom ML strategy is not implemented. It gets a page under `docs/strategies/`, and
a test file in the `reference_strategies` component, when it lands.
