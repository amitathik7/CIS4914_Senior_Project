# Implementation Plan

> Status: **M0 complete** (this scaffold). Everything after is proposed and
> open to change by the team. Milestones are sized in "person-weeks of focused
> work", not calendar dates.

## Principles

- Every milestone ends with the project **building green and tests passing**.
- Add a `trading_engine_add_component_test(<component> ...)` entry with each
  component you make real. No feature without a test.
- Keep vendor code (Alpaca, PostgreSQL) inside its adapter target.
- Replace a `NotImplemented` stub only together with its tests.

## M0 — Scaffold  ✅ (done)

- CMake: core library, two adapter libraries, thin executable, GoogleTest via
  pinned FetchContent, `BUILD_TESTING`, per-component test helper.
- All component headers with responsibilities, interfaces, ownership/threading
  notes, and TODO lists.
- Domain value types defined.
- Stubs throw `NotImplemented`; real bits (`IClock`/`ManualClock`, strong ids +
  generator, enum `to_string`, `default_config`) are tested.
- Docs: this plan, `ARCHITECTURE.md`, `DATA_FLOW.md`, `OPEN_QUESTIONS.md`, ADR 0001.

## M1 — Domain + Event Bus (≈1–2 wk)

- Finalise `MarketEventType` and the `Order` state machine (close two open
  questions).
- Implement `InProcessEventBus`: bounded MPMC queue, `subscribe`/`publish`,
  `start`/`request_shutdown`/`wait_until_drained`, chosen backpressure policy.
- Unit tests: ordering, capacity, multi-producer, shutdown drains, drop/reject
  counters.
- Wire `SystemMetrics` counters (real, lock-free) into the bus.

## M2 — Market data ingest + historical source (≈2 wk)

- `MarketDataService`: validation policy, normalisation, sequencing, ingest
  stamping; publish to the bus.
- `AlpacaHistoricalSource`: REST paging, JSON parse, time-ordered emit
  (dependencies land in `trading_engine_alpaca_adapter`).
- `HistoricalReplay`: deterministic `ManualClock` advance, `replay_speed`
  pacing, multi-symbol merge.
- Fixtures: recorded Alpaca responses under `tests/fixtures/alpaca/`.
- Integration test (opt-in): fetch a tiny real range from a paper account.

## M3 — Strategies + risk (≈2 wk)

- `StrategyEngine`: routing index, `ISignalSink`, per-strategy exception
  isolation, signal stamping + publish.
- One or two reference `IStrategy` implementations (e.g. SMA crossover,
  threshold) — pick in the open questions.
- `RiskManager` + a starter set of `IRiskPolicy` (max order qty, max position
  notional, max gross exposure, daily loss stop, max open orders).
- Tests: policy composition, resize vs reject, portfolio-aware limits using a
  fake `IPortfolioView`.

## M4 — Execution + portfolio (≈2 wk)

- `NaiveFillModel`: full fill at reference ± slippage, fees, zero latency.
- `ExecutionSimulator`: signal→order sizing, order lifecycle, market-context
  cache, latency queue, partial-fill continuation.
- `PortfolioManager`: cash/position/cost-basis math, realised & unrealised
  P&L, exposure; thread-safe `snapshot()`.
- Tests: long/short round trips, fees, averaging, P&L identities; a
  deterministic end-to-end backtest on fixture data.

## M5 — Persistence (≈1–2 wk)

- Decide migration tool + PostgreSQL client library (open questions).
- Finalise schema; write real `001_initial_schema.sql` (+ down migration).
- `PostgresRepository`: connection pool, prepared statements, batched market
  -data insert, transactional trade writes (in `trading_engine_postgres_adapter`).
- In-memory repository fake for fast tests; integration tests against a
  disposable PostgreSQL (container).

## M6 — Analytics + visualization (≈1–2 wk)

- `PerformanceAnalyzer`: return series, volatility, max drawdown, profit
  factor, trade count; then Sharpe/Sortino/win-rate.
- Define the export schema; write it from the analyzer.
- Implement `python/visualization/plot_performance.py` against that schema;
  golden-image or golden-JSON tests.

## M7 — Live feed (≈1–2 wk)

- `AlpacaLiveSource`: WebSocket client, auth, reconnect/backoff, heartbeat,
  frame parser, its own I/O thread.
- End-to-end live run against a paper account, portfolio simulated.
- Soak test: run for hours, watch `SystemMetrics` for leaks / drift.

## M8 — Concurrency, latency & throughput hardening (ongoing)

- Benchmarks: end-to-end event latency percentiles, sustained events/sec,
  queue depth under load.
- Profile hot paths; consider lock-free structures, batching, cache-friendly
  layouts, symbol interning, fixed-point money.
- Document measured numbers against the project's latency/throughput goals.

## Cross-cutting backlog

- Structured logging library (open question) + correlation ids per event.
- Config format + parser; `load_config` / `validate`.
- CI: configure + build + `ctest` on Linux and Windows; warnings-as-errors in CI.
- `clang-format` / `clang-tidy` config and a pre-commit check.
