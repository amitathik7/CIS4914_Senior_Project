# Data Flow

> Status: **planned**. Sequence of responsibilities the scaffold is built
> around. No stage is implemented yet.

Two entry points feed one identical downstream pipeline: a **live** WebSocket
feed and a **historical replay**. Everything after "Market Data Service" is the
same in both modes.

## Common downstream pipeline

```
raw source data
  → MarketDataService.on_market_event(raw)
        · validate (positive prices, ordering, spike filter)   [MarketDataPolicy]
        · normalise → domain::MarketEvent (canonical fields)
        · stamp ingest_time (from IClock), assign per-symbol sequence
        · (optional) IMarketDataRepository.store(event)
        · bus.publish(Event{MarketData, event})
  → EventBus  ── MarketData ──▶ StrategyEngine.on_market_event(event)
        · route to interested IStrategy instances
        · each strategy may emit 0..n domain::TradeSignal via ISignalSink
        · StrategyEngine stamps SignalId + created_at, bus.publish(Event{Signal})
  → EventBus  ── Signal ──▶ RiskManager
        · snapshot = IPortfolioView.snapshot()          ◀── current portfolio state
        · build RiskContext (limits, open orders, realised PnL today)
        · for each IRiskPolicy: evaluate(signal, snapshot, context)
        · combine → RiskDecision { approve | resize | reject(reason) }
        · approved → bus.publish(Event{Signal, approved})   ; rejects recorded
  → EventBus  ── approved Signal ──▶ ExecutionSimulator
        · size signal → domain::Order (OrderId), status transitions
        · IFillModel.simulate(order, MarketContext, SimulationConfig)
              · latency, slippage (bps), fees, partial fills
        · bus.publish(Event{Order}) and Event{Fill} per fill (FillId)
  → EventBus  ── Order / Fill ──▶ PortfolioManager
        · apply(fill): adjust cash, position qty & avg cost, realised P&L
        · mark(market event): update unrealised P&L, total equity, exposure
        · publishes / exposes updated PortfolioSnapshot
  → Persistence (ITradeRepository)
        · record_order, record_fill, record_snapshot
  → PerformanceAnalyzer (end of run, or periodically)
        · analyze(equity_curve, fills) → domain::PerformanceReport
        · ITradeRepository.record_performance(report)
        · export file  →  python/visualization/plot_performance.py

SystemMetrics is updated at every hop: per-stage latency, queue depth,
throughput, error counts.
```

## Live mode

| Step | Detail |
|---|---|
| Clock | `SystemClock` (real UTC time). |
| Source | `AlpacaLiveSource` — WebSocket to `stream_url`, authenticate with `ALPACA_API_KEY` / `ALPACA_API_SECRET`, subscribe to configured symbols, push each parsed frame to `MarketDataService`. Runs its own I/O thread. |
| Pacing | Real time; throughput bounded by the feed and the bus. |
| Backpressure | If the bus is full: block the source, drop, or reject — **undecided**. On a slow consumer the live feed cannot be paused, so a policy is mandatory. |
| Shutdown | External signal → `TradingEngine.request_shutdown()` → stop source, drain bus, flush persistence, emit report. |
| Persistence | Raw `MarketEvent`s stored as they arrive (optional), plus all orders/fills/snapshots. |

## Historical replay (backtest)

| Step | Detail |
|---|---|
| Clock | `ManualClock` — advanced by `HistoricalReplay` to each event's `exchange_time`. No wall-clock waits. |
| Source | `AlpacaHistoricalSource` (REST) **or** `IMarketDataRepository.load(symbol, range)` for previously stored data. |
| Ordering | `HistoricalReplay` guarantees ascending `exchange_time`; multi-symbol streams are merged into one time-ordered sequence. |
| Pacing | `replay_speed = 0` → as fast as possible (CI, parameter sweeps). `replay_speed > 0` → fraction of real time for demos. |
| Determinism | Same input + same config ⇒ identical `PerformanceReport`, because time, ordering, and (seeded) fill models are all controlled. |
| Termination | Source exhausted (or `max_events` hit) → run `PerformanceAnalyzer`, persist the report, export for charts. |

## Where data is persisted

| Data | Written by | Interface | Table (draft) |
|---|---|---|---|
| Normalised market events | MarketDataService | `IMarketDataRepository` | `market_events` |
| Orders | ExecutionSimulator | `ITradeRepository` | `orders` |
| Fills | ExecutionSimulator | `ITradeRepository` | `fills` |
| Portfolio snapshots | PortfolioManager | `ITradeRepository` | `portfolio_snapshots` |
| Performance report | PerformanceAnalyzer | `ITradeRepository` | `performance_runs` |
| Run metadata | TradingEngine | `ITradeRepository` | `runs` |

Schema is a draft — see [`database/migrations/001_initial_schema.sql`](../database/migrations/001_initial_schema.sql).
