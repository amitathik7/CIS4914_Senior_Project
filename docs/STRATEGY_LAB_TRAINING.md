# Strategy Lab: training and tuning the ML strategy

> The `train` and `sweep` commands of `strategy_lab_replay`. Code: [`apps/strategy_lab/lab/training.hpp`](../apps/strategy_lab/lab/training.hpp).
> Tests: `tests/unit/strategy_lab_training_test.cpp` and `TrainingContract` in `tests/strategy_lab/test_cli_contract.py`.
> The strategy itself: [strategies/logistic_regression.md](strategies/logistic_regression.md).

## 1. What they do

`ml_logistic` learns while it runs, so "training" it means replaying recorded bars through it and looking at what it
learned and how well it predicted. Both commands drive the **real** `LogisticRegressionStrategy`, built through the lab's
catalog, so a weight reported here is exactly the weight the strategy would reach live on the same bars.

- **`train`** runs one configuration. It reports the accuracy in each period against a majority-class baseline, the
  signals in each period, and the final weights per symbol, including as an `initial_parameters` warm-start string.
- **`sweep`** runs a grid of configurations, ranks them on the training period only, and reports each one's holdout
  beside it.

Neither command places orders, sizes positions or measures profit. Profit needs the backtest, which is separate work.

## 2. The split

The dataset's rows are split by position. The first `floor(rows * (1 - holdout))` rows are the **training** period and
the rest are the **holdout**. `--holdout` defaults to 0.3 and must be in [0, 1). With `--holdout 0`, the holdout is
empty and its accuracy is `null`, not 0.

The model keeps learning through both periods, exactly as it would live (walk-forward), so the split changes no weight.
It only decides which period a scored prediction is counted in: the period of the row on which its label became known.
Each prediction is made before its label exists, so both periods are out-of-sample predictions. What makes the holdout
an honest check in a **sweep** is that it is never used to choose.

## 3. Command line

```
strategy_lab_replay train --dataset <csv> [--param name=value]... [--holdout F]
                          [--max-rows N] [--pretty] [--no-wall-clock]
strategy_lab_replay sweep --dataset <csv> [--param name=value]...
                          --grid name=v1,v2,... [--grid ...]... [--holdout F]
                          [--max-rows N] [--pretty] [--no-wall-clock]
```

- `--param` configures `ml_logistic` exactly as `run --param` does (`describe` lists the names, types and defaults).
  Without `symbols`, every symbol that has a bar in the dataset is used.
- `--grid` (sweep only) names one parameter and its comma-separated values. The grid is every combination of the axes,
  at most 256. An axis may not repeat, and may not also be a `--param`.
- A combination the **strategy** rejects (for example `rearm_band=0.2` with `entry_probability=0.6`) becomes a row with
  `rejected` and the strategy's own message. It is not an error. A value that does not parse as the parameter's type is
  an error (exit 2), as in `run`.
- The dataset format, `--max-rows`, `@file`, exit statuses and the error document are the same as for `run`
  ([STRATEGY_LAB.md](STRATEGY_LAB.md) section 6).

## 4. Output

`strategy_lab.training` / `strategy_lab.sweep`, schema version 1.0, with the same `provenance` and `input` header as a
replay. A period is:

```json
{"scored": 147, "correct": 136, "up_labels": 68, "accuracy": 0.925, "majority_baseline": 0.537, "signals": 8}
```

`scored` counts labelled predictions (a move inside `label_dead_band` is not labelled). `accuracy` is `correct / scored`.
`majority_baseline` is what always guessing the more common label in that period would score, so it is the number to
beat. Both are `null` when nothing was scored.

`train` writes `result.split`, `result.parameters` (every parameter, defaults filled in), `result.training` and
`result.holdout` (summed over symbols), and `result.symbols[]` with each symbol's periods, `weights` by feature name,
`bias` and `initial_parameters`.

`sweep` writes `ranked_by` (`"training.accuracy"`), `holdout`, `fixed`, `axes`, `rows[]` (best first, rejected rows last,
ties in grid order) and `best`, the top accepted row in full (its assignment plus the same fields as a `train` result), or `null` when every
combination was rejected.

Every number is written as the shortest text that reads back as the same double, so `initial_parameters` is exact.

## 5. Example

On the built-in `ml_trending` dataset (240 bars, 20-bar regimes):

```
strategy_lab_replay train --dataset tests/fixtures/strategy_lab/datasets/ml_trending.csv \
    --param horizon=1 --param min_training_samples=20 --pretty
```

The training accuracy is 0.93 against a baseline of 0.54, and the holdout accuracy is 0.96 against 0.56.

```
strategy_lab_replay sweep --dataset tests/fixtures/strategy_lab/datasets/ml_trending.csv \
    --param min_training_samples=20 --grid horizon=1,3,5 --grid learning_rate=0.01,0.05,0.2
```

## 6. Using the result

1. Sweep on a long stretch of real bars and pick a configuration whose **holdout** accuracy clearly beats its
   baseline. A few points above it is a normal, honest result on real prices. A large gap between training and holdout
   means the grid found noise.
2. Run `train` with that configuration on the bars just before you deploy.
3. Pass `initial_parameters` from that symbol into `LogisticRegressionConfig` so the strategy starts from those weights
   instead of zero. The lab itself does not offer `initial_parameters` as a `--param`, so a replay always starts cold.

Weights are per symbol, and they are only meaningful with the same features and the same standardization history, so
warm-start a symbol only with its own weights.

## 7. Limits and choices

- Accuracy is directional, not profit. A model can be right more often than not and still lose money on the moves it
  gets wrong.
- The split is by row, not by time, so with several symbols the boundary falls at the same row for all of them.
- The warm start sets the weights only. The running feature statistics still start empty, so the first bars after a
  warm start are standardized from very few samples.
- Training on Alpaca historical bars needs a CSV exported in the lab format. A scheduled routine could run `train`
  on fresh bars once that export exists.
