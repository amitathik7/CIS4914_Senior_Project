# Machine learning: online logistic regression

> `LogisticRegressionStrategy`, kind `ml_logistic` in the Strategy Lab. The team's third strategy option.
> Header: [`logistic_regression_strategy.hpp`](../../include/trading_engine/strategy/logistic_regression_strategy.hpp);
> model pieces: [`logistic_model.hpp`](../../include/trading_engine/strategy/logistic_model.hpp). Tested against
> test doubles only, like the other two ([STRATEGIES.md](../STRATEGIES.md)). The defaults are local placeholders,
> **not tuned or recommended trading parameters**.

## 1. What it does

For each symbol in its allowlist, the strategy learns from the bars as they arrive whether the close tends to be
higher `horizon` bars later. It turns six price features into a probability `p` of a rise with a logistic
regression, and asks for a Buy when `p` is high enough and a Sell when it is low enough, once per excursion.

It is pure decision logic, like the reference strategies. It tracks no position, sizes nothing and checks no risk
limit. A signal is a request for the configured quantity, so a Sell does not mean "close a long".

It uses no ML library and does no I/O. The model is a seven-number vector per symbol, and every run is
deterministic.

## 2. Configuration

| Field | Default | Rule |
|---|---|---|
| `strategy_id` | `ml_logistic` | Not empty; unique per engine |
| `horizon` | 5 | At least 1; how many accepted bars ahead the label looks |
| `label_dead_band` | 0 | Finite, at least 0; moves with abs(log return) at or below it are not trained on |
| `learning_rate` | 0.05 | Finite, greater than 0 |
| `l2` | 0.001 | Finite, at least 0; penalty on the weights, not the bias |
| `min_training_samples` | 50 | Trained samples a symbol needs before it may emit; 0 emits at once |
| `entry_probability` | 0.60 | Strictly between 0.5 and 1 |
| `rearm_band` | 0.05 | At least 0 and below `entry_probability - 0.5` |
| `initial_parameters` | empty | Empty (all zero), or exactly 7 finite values: six weights then the bias. This is the warm-start hook for weights trained offline. The lab does not offer it |
| `requested_quantity` | 1 | A positive whole number of shares |
| `symbols` | (none) | Required: non-empty, no empty entry, no repeats |

An invalid value throws `common::ConfigError` naming the field, as for the other strategies.

## 3. Features

Close-only, from the last 21 accepted closes `c`, newest `c_t`:

| # | Name | Formula |
|---|---|---|
| 0 | `return_1` | ln(c_t / c_t-1) |
| 1 | `return_5` | ln(c_t / c_t-5) |
| 2 | `return_20` | ln(c_t / c_t-20) |
| 3 | `z_score_20` | (c_t - mean of last 20) / population stddev of last 20; 0 for a constant window |
| 4 | `sma_gap_5_20` | mean of last 5 / mean of last 20 - 1 |
| 5 | `volatility_20` | population stddev of the last 20 one-bar log returns |

Features 3 and 4 are the mean-reversion and crossover indicators, so the learned weights show how much the model
trusts each of them. Open, high, low and volume are not used, because `MarketEvent` makes them optional.

Each feature is standardized per symbol with a running mean and variance (Welford). The statistics used to scale a
bar come from earlier bars only, and the scaled values are clipped to [-5, 5].

## 4. Learning and deciding, per accepted bar

1. The close joins the window.
2. If the oldest queued sample was taken `horizon` accepted bars ago, it is labelled: up if
   ln(close_now / close_then) > `label_dead_band`, down if < -`label_dead_band`, dropped otherwise. A labelled
   sample is scored for `rolling_accuracy` (was its prediction on the right side of 0.5?) and then trains the
   model with one SGD step:

   ```
   p = sigmoid(w . x + b)
   w <- w - learning_rate * ((p - y) * x + l2 * w)
   b <- b - learning_rate * (p - y)
   ```

3. Under 21 closes: warming up, nothing else happens.
4. The bar's features are computed and scaled, then folded into the running statistics.
5. The model predicts `p`, and the scaled features are queued as a sample.
6. Fewer than `min_training_samples` trained samples: `training`, nothing emitted.
7. Otherwise the latch decides. Both comparisons are inclusive:
   - `p >= entry_probability` and the latch is not Up: **Buy**, and the latch becomes Up.
   - `p <= 1 - entry_probability` and the latch is not Down: **Sell**, and the latch becomes Down.
   - `abs(p - 0.5) <= rearm_band`: the latch becomes Neutral, and nothing is emitted.
   - Anything else changes nothing.

A decision at bar t depends only on bars up to t. The test
`ChangingLaterBarsNeverChangesAnEarlierDecision` changes every bar after 250 and checks that bars 1 to 250 decide
exactly as before.

The ignore rules are the same as mean reversion's: non-bars, unlisted symbols, a missing or non-positive close,
and an `exchange_time` not after the last accepted one. None of them changes any state.

`on_start()` resets everything, models included, to `initial_parameters`. A restarted engine replays bit for bit.

## 5. Signals

`side` Buy or Sell, `requested_quantity`, `order_type` Market, and **`confidence` = p for a Buy, 1 - p for a
Sell**. This is the first strategy to set `confidence`. Metadata (text): `trigger`
(`probability_at_or_above_buy_entry` | `probability_at_or_below_sell_entry`), `close`, `probability_up`,
`horizon`, `training_samples`, `entry_probability`, `rearm_band`, `rolling_accuracy` (once a sample has been
scored), `feature_<name>` and `weight_<name>` for each feature, and `bias`.

## 6. Diagnostics and the lab

Reasons: the shared ignore reasons, `warming_up`, `training`, `measurement_failed` (an unreachable guard),
`entry_buy`, `entry_sell`, `excursion_already_requested`, `inside_rearm_band` and `between_bands`. Indicators:
`probability_up`, `probability_edge` (p - 0.5), `training_samples` and `rolling_accuracy`.

In the Strategy Lab and the console, the lower chart pane plots `probability_edge`, with the entry levels at
+-(entry_probability - 0.5) and the re-arm band at +-rearm_band. The built-in dataset `ml_trending` (240 bars,
regimes of 20 bars up 1% then 20 down) shows it learning. With `horizon=1` and `min_training_samples=20`, it
requests at each turn once trained.

To fit it on recorded bars, use the lab's `train` command; to compare configurations, `sweep`. Both report accuracy on a
training period and a held-out period against a majority-class baseline, and `train` prints the learned weights as an
`initial_parameters` warm start ([STRATEGY_LAB_TRAINING.md](../STRATEGY_LAB_TRAINING.md)).

## 7. What the tests show and do not show

`tests/unit/logistic_regression_strategy_test.cpp` and `logistic_regression_engine_test.cpp` cover the following:

- Features, the standardizer and one SGD step against hand-computed values.
- Config checks, warm-up and training gates, the latch, signal fields and the warm start.
- The dead band, ignored events and observer invariance.
- No look-ahead, bit-for-bit restarts and symbol independence.
- An engine run beside the other two strategies.

On the planted trend, rolling accuracy ends above 0.85. On a seeded random walk, it stays near 0.5.

They do **not** show that it makes money. On real bars, a directional accuracy a few points above 50% would be a
normal, honest result. P&L needs the risk, execution and portfolio stages, which do not run end to end yet.

## 8. Local choices (not team decisions)

- One model per symbol. A shared model across symbols would learn faster, but it couples symbols.
- A Sell is a request, as in the other strategies (ADR 0002 and OQ#9 are still open).
- The label is the sign of the `horizon`-bar log return, with a configurable dead band.
- The features are close-only.
