# Backtesting

A backtest runs one or more strategies over recorded bars through every stage of the engine
and reports P&L. Everything is simulated: no network, no real orders.

```
bars -> ExecutionSimulator (fills orders from earlier bars)
     -> MarketDataService -> bus -> StrategyEngine -> TradeSignal
     -> RiskManager (policies, then a buying-power / shares-held hold)
     -> ExecutionSimulator.submit -> Order / Fill on the bus
     -> PortfolioManager (cash, positions, marks) -> snapshot per timestamp
     -> PerformanceAnalyzer -> PerformanceReport
```

Three ways in:

| Where | What |
|---|---|
| `app::Backtest` (`include/trading_engine/app/backtest.hpp`) | The composition root. `add_strategy()`, then `run(bars)` returns signals, risk decisions, orders, fills, the equity curve, the final snapshot and the report. |
| `strategy_lab_backtest` (`apps/strategy_lab`) | The same over a Strategy Lab CSV, as one JSON document (section 5). Takes the replay tool's `--strategy/--param` arguments, so every catalog strategy works, including `ml_logistic`. |
| Web console, Strategies > **Backtest** | The draft from Configure/Explore plus account, cost and risk settings, run through the gateway's `POST /lab/v1/backtest`. Shows equity, drawdown, fills, positions and every risk decision. |

Build and run the tool:

```bash
cmake -S . -B out/strategy_lab -DBUILD_TESTING=OFF -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON
cmake --build out/strategy_lab --config Release --target strategy_lab_replay strategy_lab_backtest
out/strategy_lab/bin/Release/strategy_lab_backtest --dataset tests/fixtures/strategy_lab/datasets/ml_trending.csv \
    --strategy ml_logistic --param symbols=AAPL --param requested_quantity=50 --fee-per-trade 1 --pretty
```

The gateway looks for `strategy_lab_backtest` next to `strategy_lab_replay`; `/status` reports
it under `backtest`.

## 1. Determinism and timing

A `ManualClock` is set to each bar's `exchange_time` before the bar is delivered, and the bus is
drained after every step, so event order never depends on thread timing. The same bars and
settings give the same document byte for byte.

Per bar: (1) orders submitted on **earlier** bars for this symbol fill at this bar's open (its
close when the data has no open column), moved against the order by `slippage_bps`; (2) the bar
goes through `MarketDataService`, the portfolio marks the close, strategies decide, risk checks
each signal against the portfolio as it is now, and approved signals become working orders;
(3) after the last bar at a timestamp a `PortfolioSnapshot` is kept. Strategies decide on a
close, so they never trade at the price that produced the decision. An order still working when
the data ends is reported, not filled.

## 2. Risk

`RiskManager` runs its policies in order. The first rejection wins; a resize feeds the smaller
quantity to the policies after it. Then the portfolio must grant a hold
(`IReservationLedger::hold_for_signal`, ADR 0005): buys need buying power (last mark plus a 5%
buffer, plus fees), sells need shares held and not already committed. The account is **long
only**: a Sell with nothing to sell is rejected, never a short.

| Policy | Limit (`config::RiskLimits`) | Effect |
|---|---|---|
| `max_order_quantity` | 1000 shares | resize down |
| `max_position_notional` | 25,000 | reject a buy that would make the position worth more |
| `max_gross_exposure` | 100,000 | reject a buy that would take gross exposure above it |
| `max_open_orders` | 50 | reject while that many orders are working |
| `daily_loss` | 5,000 | reject buys once the session's loss (UTC day, realised + unrealised) reaches it |

`--no-risk-policies` (or unticking "Apply the risk policies") keeps only the hold.

## 3. Execution and costs

`NaiveFillModel` fills the whole remaining quantity at the reference price moved by
`slippage_bps` (buys pay more, sells receive less). A limit order fills only when the reference
price reaches its limit. Fees per fill: `per_trade + per_share * shares + fee_bps of notional`
(`execution::fee_for`). Latency and partial fills are not modelled yet.

## 4. Metrics (`PerformanceAnalyzer`)

- **Returns** per equity-curve sample, starting cash first: `r_i = E_i / E_(i-1) - 1`.
- **Total return** `E_last / E_start - 1`; **max drawdown** the most negative `E / running peak - 1`.
- **Volatility** sample std-dev of returns times `sqrt(periods_per_year)`; **Sharpe** mean/std times
  the same, omitted under 3 returns or zero volatility. The tool infers `periods_per_year` from the
  median bar gap (252 sessions of 6.5 hours; 252 for daily bars) unless `--periods-per-year` is given.
- A **trade** is one closing execution. Its P&L is `(fill price - average cost) * shares closed`
  minus its fees and the opening fees of the shares it closes, so trade P&L sums to the
  portfolio's realised P&L. **Profit factor** = gross profit / gross loss, reported only when
  there is a losing trade (`profit_factor_status` says `no_losing_trades` or `no_trades`).

## 5. `strategy_lab.backtest` document (schema 1.0)

One JSON object on stdout; exit status and error documents are the replay tool's
(`docs/STRATEGY_LAB.md` section 6). Money and prices are exact decimals (at most 6 places) written
as JSON numbers, quantities are integers, times are RFC 3339 UTC.

| Member | Contents |
|---|---|
| `input` | dataset name, `sha256`, `rows`, `symbols`, first/last time |
| `config` | `starting_cash`, `slippage_bps`, `fees{per_share,per_trade,bps}`, `risk_policies`, `risk{...}`, `periods_per_year`, `periods_per_year_source`, `fill_rule` |
| `strategies` | `kind`, `strategy_id`, every resolved `parameters` value |
| `report` | the metrics above plus counts, `starting_equity`, `ending_equity`, `gross_profit`, `gross_loss`, `fees_paid` |
| `counts` | bars accepted/rejected, signals, approved/resized/rejected, orders, fills, `open_orders_at_end`, errors |
| `final` | `cash`, `equity`, `gross_exposure`, `reserved_cash`, `positions[]` |
| `equity` | one point per bar time: `time`, `equity`, `cash`, `gross_exposure`, `drawdown` |
| `fills` | `fill_id`, `order_id`, `signal_id`, `strategy_id`, `symbol`, `side`, `quantity`, `price`, `fees`, `slippage`, `time` |
| `orders` | latest state of each order, with `reject_reason` when rejected |
| `decisions` | one per signal: request, `approved`, `adjusted_quantity`, `reason`, deciding `policy` |

## 6. Not yet

Source-driven `HistoricalReplay` (Alpaca / PostgreSQL), latency and partial fills, shorting, a
kill switch, persistence of runs, and wiring risk and execution into `trading_engine run` (live
bus path) are open. The demo Backtests page in the web console still runs the in-browser
simulator; the Strategies > Backtest view is the real engine.
