# Implementation Plan

The scaffold is complete: every runtime path is a stub that throws
`common::NotImplemented`. This plan turns it into a working **simulated**
trading engine over the 15-week term. It never trades real money and is not
graded on predicting markets.

## How to use this document

- It is **coordination guidance, not a contract.** Refine a milestone's detail
  as implementation reveals more; keep the goal and week range fixed.
- **A correct end-to-end slice beats many half-built components.** Stretch work
  starts only after the required path for that area works and is tested.
- **Decisions** live in [`OPEN_QUESTIONS.md`](OPEN_QUESTIONS.md) (the register)
  and, once made, in [`adr/`](adr/). This plan cites `OQ#n` / ADR ids; it does
  not re-argue the options.
- **Ownership** lives in [`COMPONENT_OWNERSHIP.md`](COMPONENT_OWNERSHIP.md).
  Each milestone names only its primary and supporting owner *areas* (A/B/C/D).
- **Required** = a proposal deliverable; it must ship and be tested.
  **Stretch** = improvement work that may be cut without failing the course.

## Ground rules (every milestone)

**Definition of Done**

- CI is green on the agreed platforms, `ctest` passes, no new warnings
  (`-DTRADING_ENGINE_WARNINGS_AS_ERRORS=ON`).
- New behaviour has GoogleTest coverage via
  `trading_engine_add_component_test(<component> …)`; integration tests go under
  `tests/integration/` (opt-in, `-DTRADING_ENGINE_BUILD_INTEGRATION_TESTS=ON`).
- When a stub becomes real, its assertion in
  `tests/unit/scaffold_contract_test.cpp` is removed or replaced with a
  behavioural test.
- Alpaca / PostgreSQL / JSON / networking code stays inside
  `trading_engine_alpaca_adapter` or `trading_engine_postgres_adapter`;
  `trading_engine_core` links only the C++ standard library.
- Docs touched by the change are updated; a decision produces an ADR.
- Reviewed by the component owner area + one other member.

**Pull requests** — "Each pull request should implement one coherent,
independently reviewable and independently revertible change. Line count is a
warning signal, not a fixed limit." A milestone may name 2–4 logical delivery
branches when that genuinely helps; it does not prescribe a PR per task.

**Critical path:** M1 → M2 → M3 → M4 → M5 → M6 → M7 → M8 → M9 → M10. M3 and M4
may overlap (M4 needs M2, not M3). Persistence runs as its own staged track
(see *Persistence stages*).

---

## M1 — Weeks 1–2 — Architecture & Technical Requirements

**Goal.** Make the decisions and stand up the minimal tooling that genuinely
block M2. No production component logic.

**Must-have work packages** (~8)

1. **Core architecture ADR(s)** — event shape + v1 `MarketEventType`
   (`OQ#1`); queue / threading model (`OQ#2`); backpressure, shutdown and
   draining semantics (`OQ#3`). These three are coupled; decide them together.
2. **Numeric-representation ADR** — money/price type: `double` vs fixed-point /
   integer minor units (`OQ#11`, money half). Cross-cutting; all four sign off.
   Blocks the domain value types and the PostgreSQL schema.
3. **Platform & toolchain ADR** — supported OS/compilers and minimum versions
   (`OQ#13`); this sets the CI matrix.
4. **Config & logging ADR** — configuration format + precedence (`OQ#7`);
   logging library + hot-path policy (`OQ#8`).
5. **Adapter-library ADR** — Alpaca HTTP + JSON + WebSocket libraries (`OQ#4`);
   PostgreSQL client library, migration tool and schema shape (`OQ#5`, `OQ#6`).
   Everything stays private to its adapter target.
6. **`docs/TECH_REQUIREMENTS.md`** — measurable latency / throughput / memory
   targets and a test-coverage floor. Baselines get filled in from M2 on.
7. **Repo & CI setup** — minimal `.github/workflows/ci.yml` (configure + build +
   `ctest` on the agreed platforms, warnings-as-errors, `py_compile`);
   `.clang-format`; create the issue-template **labels** (they are a
   prerequisite — see [`../.github/ISSUE_TEMPLATE/README.md`](../.github/ISSUE_TEMPLATE/README.md));
   **advisor decision on repository visibility + licensing** (`OQ#14` — the repo
   is currently public with no `LICENSE`).
8. **Test-support library** — `tests/support/`: a synchronous
   `RecordingEventBus`, in-memory `IMarketDataRepository` / `ITradeRepository`,
   and a `FakePortfolioView`.

**Acceptance criteria**

- Every Weeks 1–2 decision (list at the end of this file) is an *Accepted* ADR
  or an explicit, written deferral.
- CI runs on a PR and blocks merge on failure; shown once with a forced failure.
- `tests/support/` doubles build and are exercised by at least one test.
- Architecture review delivered to the advisor; feedback captured as issues.

**Tests / demo.** Unit smoke tests for each `tests/support/` double. Demo (end
of Week 2): the pipeline diagram, the ratified threading/backpressure model, the
NFR targets, and a live green CI run.

**Dependencies & blocking decisions.** None inbound. Blocks M2 until `OQ#1–3`,
`OQ#7`, `OQ#8` and the numeric-representation ADR are Accepted.

**Owners.** Primary A. Supporting: D (numeric-representation ADR, in-memory
repos), B (event-shape input, adapter-library ADR), C (config/py CI).

---

## M2 — Weeks 3–4 — Event Framework & Data Ingestion

**Goal.** One correct vertical slice: raw input → normalised `MarketEvent` →
bounded event bus → a consumer, deterministically, with basic instrumentation.
Live Alpaca work starts here in parallel.

**Must-have deliverables** (primary vertical slice)

1. **Bounded event delivery** — `InProcessEventBus`: `publish` / `subscribe` /
   `unsubscribe`, a fixed capacity, and the one backpressure policy chosen in
   M1. Plus `depth()`.
2. **Explicit close/shutdown** — `start` / `request_shutdown` /
   `wait_until_drained`: no new events accepted after shutdown; in-flight events
   are delivered before it returns.
3. **`MarketEvent` normalisation** — `MarketDataService` validates raw input
   (reject non-positive price / missing symbol), stamps `ingest_time` from
   `IClock`, assigns a gap-detectable per-symbol `sequence`, and publishes
   `EventType::MarketData`.
4. **Deterministic source** — `FixtureMarketDataSource` reads a committed
   CSV/JSON fixture in `exchange_time` order.
5. **One end-to-end flow** — CLI `--mode ingest --source fixture`: source →
   `MarketDataService` → a printing bus subscriber. Byte-identical output on a
   rerun.
6. **Instrumentation hooks** — `SystemMetrics` counters + at least one latency
   timing point wired into the bus and service. Hooks only, **no optimisation**.
7. **Config** — `config::load_config` + `validate` for the M1 format, enough to
   run the CLI.

**Parallel adapter work** (required deliverables; not all finish in M2)

- `AlpacaHistoricalSource` (REST) — paged fetch, JSON→`MarketEvent`, ascending
  time, 429 retry. May finish in M3.
- `AlpacaLiveSource` **minimum** — connect, authenticate, subscribe, parse one
  live session, clean `stop()`. **Required proposal deliverable**; begins here,
  **must be demonstrable by the end of M3**. (M9 is an emergency backstop only,
  not the expected delivery point.)
- Persistence **P1–P2** (see *Persistence stages*).

**Acceptance criteria**

- CLI ingest on the fixture prints normalised `MarketEvent`s and is
  byte-identical on rerun; malformed rows are rejected and counted, not
  published.
- A unit test fills the bus to capacity and observes the chosen backpressure
  behaviour and its counter; multi-producer publish delivers every event
  exactly once; `wait_until_drained()` returns only after all queued events
  were delivered.
- `MarketDataService` stamps `ingest_time` and a per-symbol `sequence`;
  publishes accepted events only.
- `SystemMetrics` exposes event counts and one latency measurement (plausible
  values; no target enforced yet).

**Tests / demo.** Unit: bus capacity / ordering / close / multi-producer;
`MarketDataService` each validation rule + stamping; `SystemMetrics` counters
monotonic under concurrency. Integration (opt-in, auto-skip without
credentials/DB): `it_alpaca_historical`, `it_alpaca_live_smoke`,
`it_postgres_market_data`. Demo (end of Week 4): deterministic CLI ingest +
rejected row + `SystemMetrics` counters; show live-adapter progress (connect +
first frames).

**Dependencies & blocking decisions.** M1 ADRs (`OQ#1–3`, `OQ#7`, `OQ#8`,
numeric representation); adapter-library ADR (`OQ#4`, `OQ#5`) for the parallel
work.

**Owners.** Primary A (bus, config, metrics) + B (market data, sources,
adapter). Supporting: D (persistence P1–P2).

**Stretch.** Advanced live resiliency (reconnect, backoff, heartbeat/staleness,
sequence-gap recovery) — deferred to M8. Live-path performance tuning — M8.

---

## M3 — Weeks 5–6 — Historical Replay

**Goal.** Deterministic, reproducible backtests from recorded data. Also the
hard deadline for the live Alpaca minimum.

**Must-have deliverables**

1. **`HistoricalReplay`** — pulls from an `IMarketDataSource`, advances a
   `ManualClock`, feeds `MarketDataService` in `exchange_time` order.
   `--speed 0` (as fast as possible) and a paced mode.
2. **Multi-symbol merge** — interleaves several symbol streams by
   `exchange_time`, ties broken by `sequence`.
3. **`AlpacaHistoricalSource` complete** — if it slipped from M2.
4. **`AlpacaLiveSource` minimum complete** — connect / auth / subscribe /
   parse / clean stop, **demonstrated live** against a paper account. This is
   the deadline.
5. **`RepositoryMarketDataSource`** — streams from `IMarketDataRepository::load`
   (bridges persistence → replay).
6. **Determinism harness** — run-twice, diff the published event streams
   (reused in M9).

**Acceptance criteria**

- The same fixture replayed twice produces an identical published `MarketEvent`
  stream and identical `SystemMetrics` totals.
- A 3-symbol fixture yields a globally non-decreasing `exchange_time` stream.
- `--speed 0` finishes a ~10k-event fixture in well under a second; paced mode
  holds a documented tolerance; `stop()` mid-run drains cleanly (no hang).
- Live Alpaca minimum demonstrated; a short capture/log is attached to the demo
  notes.

**Tests / demo.** Unit: merge + tie-break; deterministic clock advance;
`max_events`; prompt `stop()`. Integration: `it_replay_from_postgres` (P2);
`it_alpaca_historical`, `it_alpaca_live_smoke` (credential-gated). Demo
(mid-semester, end of Week 6): deterministic replay + empty run-twice diff +
paced mode; then the live Alpaca session.

**Dependencies & blocking decisions.** M2 (bus, service, fixture source).
Persistence P2 read path helpful.

**Owners.** Primary B. Supporting: A (clock/determinism helpers), D
(`RepositoryMarketDataSource`, persistence P2).

**Stretch.** Advanced live resiliency stays out (→ M8).

---

## M4 — Weeks 7–8 — Strategy Framework

**Goal.** Pluggable strategies: `MarketEvent`s in, `TradeSignal`s out. Nothing
downstream consumes them yet.

**Must-have deliverables**

1. **`StrategyEngine`** — routes events to registered `IStrategy` instances via
   a symbol-interest index, provides the `ISignalSink`, stamps each signal
   (`SignalId`, `created_at`, `strategy_id`), publishes `EventType::Signal`.
2. **Exception isolation** — a strategy that throws is logged and counted in
   `SystemMetrics`; the pipeline keeps running.
3. **Lifecycle** — `start`/`stop` subscribe/unsubscribe and call
   `on_start`/`on_stop` once each.
4. **One reference strategy** (e.g. SMA/EMA crossover) with documented
   parameters and a warm-up mechanism; a second reference strategy is stretch.

**Acceptance criteria**

- Replaying a fixture with K strategies produces a deterministic `TradeSignal`
  list matching a committed golden file.
- A crafted crossover fixture yields the exact expected buy/sell signals at the
  expected timestamps.
- A strategy emits no signals until its configured warm-up history is met.
- A deliberately throwing strategy does not stop the others; the error is
  counted.

**Tests / demo.** Unit: fan-out + routing filter; signal stamping;
`on_start`/`on_stop` ordering; exception isolation; per reference strategy a
golden-signal test + warm-up suppression + parameter edges. Demo (end of Week
8): replay → SMA strategy → signals at the crossover points; add a throwing
strategy and show it is isolated.

**Dependencies & blocking decisions.** M2 (bus, market events); M3 recommended
(deterministic driving). `OQ#9` (reference strategies, signal semantics,
warm-up) ratified at the start of this milestone.

**Owners.** Primary C. Supporting: B (market-event feed), A (bus wiring).

**Stretch.** Second reference strategy; strategy-parameter config surface.

---

## M5 — Weeks 9–10 — Execution Simulator & Portfolio Manager

**Goal.** Close the loop: approved signal → simulated fills → portfolio state
and P&L. End state — a full deterministic backtest ending in a correct
`PortfolioSnapshot`.

**Must-have deliverables**

1. **`NaiveFillModel`** — full fill of remaining qty at reference price ±
   slippage (bps), fees from `FeeModelConfig`, `filled_at` from the caller's
   clock; slippage and fees recorded on the `Fill`.
2. **`ExecutionSimulator`** — `submit(TradeSignal)` → sized `Order` with an
   `OrderStatus` machine; runs the `IFillModel` against a per-symbol
   `MarketContext`; publishes `Order` then `Fill`(s); optional `order_to_fill`
   latency delay that stays deterministic under `ManualClock`; partial-fill
   continuation.
3. **`PortfolioManager`** — `apply(Fill)` (cash, position qty, average cost,
   realised P&L), `mark(MarketEvent)` (unrealised P&L, equity, exposure),
   thread-safe `snapshot()` / `position()` / `cash()`.
4. **End-to-end fixture** — with a hand-computed expected `PortfolioSnapshot`
   committed under `tests/fixtures/expected/`.
5. Persistence **P3** (see *Persistence stages*).

**Acceptance criteria**

- Long round trip (buy 100 @ 10, sell 100 @ 12, fee f): realised P&L =
  `200 − 2f`, position flat, cash reconciles — asserted to the cent. Short and
  partial-fill round trips reconcile similarly.
- Two buys at different prices give the correct `average_cost`; `mark()` moves
  unrealised P&L and equity without touching realised.
- Full pipeline (replay → strategy → risk pass-through → execution → portfolio)
  ends at the committed expected `PortfolioSnapshot`, identical on rerun.
- `snapshot()` under a concurrent `apply()` loop never returns a torn state
  (TSan-clean).

**Tests / demo.** Unit: fill model (slippage sign, fee math, guards); simulator
(sizing, status transitions, market-context update, latency ordering,
`open_order_count`); portfolio (buy/sell/add/reduce/flip/short, `mark()`,
exposure, `equity == cash + Σ position_value`); a `snapshot()` vs `apply()`
race test. Integration: `it_backtest_end_to_end` (+ persist and reload the
snapshot). Demo (end of Week 10): the deterministic backtest ending at the
expected snapshot, with one hand-worked round-trip P&L beside it.

**Dependencies & blocking decisions.** M4 (signals), M3 (replay), M2 (bus).
Money/numeric representation is already fixed (M1). `OQ#11` behaviour half (fill
/ slippage / latency / shorting / partials) ratified at the start of this
milestone.

**Owners.** Primary D. Supporting: C (signal→order semantics), A (threading
review), B (fixture format).

**Stretch.** Richer fill models (spread-crossing, volume participation);
latency drawn from a distribution rather than a fixed value.

---

## M6 — Week 11 — Risk Management

**Goal.** A working risk gate between strategy signals and execution, reading
current portfolio state.

**Must-have deliverables**

1. **`RiskManager`** — takes a fresh `IPortfolioView::snapshot()`, builds a
   `RiskContext`, runs every `IRiskPolicy`, and returns approve / resize /
   reject with a clear reason; publishes approved (possibly resized) signals and
   records rejections.
2. **Starter policies** — max order quantity, max position notional, max gross
   exposure, daily-loss stop, max open orders. Each a pure function with its own
   tests.
3. **Kill-switch latch** — manual reset at minimum.

**Acceptance criteria**

- A signal that would breach `max_position_notional` given the *current*
  portfolio is rejected or resized, with a reason; a `FakePortfolioView` test
  proves the manager re-reads state between calls.
- Resize path clamps an over-size signal and sets `adjusted_quantity`.
- After simulated losses exceed `max_daily_loss`, further buy signals are
  rejected until the session boundary.
- The end-to-end backtest with risk enabled stays deterministic and produces a
  committed expected snapshot + rejection count.

**Tests / demo.** Unit: each policy (pass / fail / exactly-at-limit);
composition per the ADR; resize vs reject; daily-loss set/reset; kill-switch.
Integration: `it_backtest_with_risk` (rejections persisted and read back). Demo
(end of Week 11): same fixture with risk off then on — different snapshots, a
readable rejection log, the daily-loss stop tripping.

**Dependencies & blocking decisions.** M5 (`IPortfolioView`), M4 (signals).
`OQ#10` (v1 limit set, breach behaviour, session/kill-switch policy) ratified at
the start of the week.

**Owners.** Primary C. Supporting: D (`IPortfolioView`), A (CLI wiring). The
five policies parallelise across all four members if the week is tight.

**Stretch.** Per-symbol / per-strategy limit scoping; automatic kill-switch on
consecutive rejects or drawdown.

---

## M7 — Week 12 — Analytics & Reporting

**Goal.** Turn a completed run into metrics and Matplotlib charts.

**Must-have deliverables**

1. **`PerformanceAnalyzer::analyze`** — return series, volatility (annualised),
   max drawdown, profit factor, trade count. Sharpe/Sortino/win-rate are
   stretch within the week.
2. **Export** — a documented schema (`docs/EXPORT_SCHEMA.md` + an example file
   under `tests/fixtures/expected/`) written by the engine at end of a
   backtest; the JSON writer lives outside `trading_engine_core`.
3. **`python/visualization/plot_performance.py`** — implement the stubbed
   functions: load the export, plot equity curve / drawdown / return
   distribution / summary table, `--output` writes PNG(s).
4. Persistence **P4** begins (see *Persistence stages*).

**Acceptance criteria**

- On a fixture run with a hand-computed expected report, every metric matches to
  a documented tolerance.
- Profit-factor with zero losing trades returns a documented sentinel, not
  NaN/inf; drawdown matches the ADR definition on a known peak-to-trough series.
- `python -m py_compile python/visualization/plot_performance.py` passes; a
  golden-JSON test drives the loader; a smoke test builds the report figure
  (Agg backend, asserts the expected axes) without displaying.

**Tests / demo.** Unit: return-series construction; each metric on crafted
inputs with known answers; empty/single-sample guards; writer round-trip.
Python tests in the CI `py` job: loader on the golden JSON, figure smoke,
drawdown math. Integration: `it_report_from_run` (write export → parse →
assert; read the same numbers back from PostgreSQL). Demo (end of Week 12):
backtest → export → `plot_performance.py` → equity / drawdown / summary PNG,
engine metrics beside the hand-computed ones.

**Dependencies & blocking decisions.** M5/M6 (fills + snapshots). `OQ#12`
(export format + metric definitions) ratified at the start of the week.

**Owners.** Primary C. Supporting: D (persistence read paths, JSON writer), A
(engine `--report` wiring).

**Stretch.** Sharpe / Sortino / win-rate / turnover; a live metrics endpoint.

---

## M8 — Week 13 — Performance Optimization

**Goal.** Measure against the M1 targets, then optimise the proven bottleneck
without weakening correctness or determinism. Graded on rigour, not on hitting a
number.

**Must-have deliverables**

1. **Benchmark harness** — `benchmarks/` with a pinned Google Benchmark
   dependency (behind a build option) and the end-to-end benches:
   event latency (p50/p99), sustained throughput (events/sec), peak memory.
2. **`docs/PERFORMANCE.md`** — methodology (machine, flags, dataset, warm-up,
   samples), a baseline table, and the final numbers vs the M1 targets.
3. **1–2 proven optimisations** — each with a before/after benchmark bound to
   commit SHAs, an ADR if it changes a design, and no behaviour change.
   Experiments that don't pay off are reverted with the negative result
   recorded.
4. **Sanitiser CI jobs** — ASan/UBSan and TSan, if not already running.

**Acceptance criteria**

- All prior tests still pass; still deterministic (run-twice diff empty);
  TSan-clean.
- Headline latency / throughput / memory are measured and reported against the
  M1 targets, with variance shown.
- Every merged optimisation PR carries its benchmark delta and shows no
  correctness regression.

**Tests / demo.** Micro-tests pinning any fast-path invariant introduced;
`it_throughput_smoke` (large fixture within a generous wall-clock ceiling) as a
CI regression guard; the full benchmark suite. Demo (end of Week 13):
`docs/PERFORMANCE.md` baseline vs final, one accepted optimisation walked
through, one rejected experiment and what the data said.

**Dependencies & blocking decisions.** M1 (targets), M6/M7 (a working pipeline
to measure). No optimisation PR without a profile pointing at the code.

**Owners.** Primary A. Supporting: B, C, D each profile and tune their own
components.

**Stretch.** Advanced live-path resiliency + tuning (carried from M2/M3);
lock-free event bus (keep the mutex version behind a flag as the reference).

---

## M9 — Week 14 — Testing & Debugging

**Goal.** Harden everything: coverage to the M1 floor, the integration suite
complete, the defect backlog burned down, the full suite reliably green.

**Must-have deliverables**

1. **Coverage** — a CI coverage job with the M1 threshold as a gate; a
   component-by-component gap sweep (each area's owner covers their own).
2. **Integration suite complete** — Alpaca historical (recorded + opt-in live),
   PostgreSQL round-trips, end-to-end backtests with and without risk, report
   generation, throughput smoke. Each runs in isolation and auto-skips when a
   credential or DB is absent.
3. **`docs/TESTING.md`** — how to run each tier, what needs credentials or a
   database, the determinism harness, the sanitiser jobs.
4. **Defect backlog** — zero open blocker/major; every fixed defect has a
   regression test linked from its issue.
5. `scaffold_contract_test.cpp` removed (or any remaining entry maps to an
   explicitly out-of-scope feature listed in `docs/TESTING.md`).

**Acceptance criteria**

- Unit `ctest` green on all CI platforms; `ctest -L integration` green with a
  database; determinism harness green; ASan/UBSan/TSan green.
- Coverage ≥ the M1 floor; CI fails if it drops.
- The required live Alpaca ingestion has been demonstrated (in M2/M3 as
  expected, or here only if it slipped) with evidence in the demo notes.
- Persistence P4 met (see *Persistence stages*).

**Tests / demo.** Fill unit coverage gaps (error paths, each
`ConfigError`/`ValidationError`, each `OrderStatus` transition, each policy
boundary, analytics edges); randomised tests for the bus and the P&L identity;
one binary-level `tests/e2e/` golden scenario. Demo (end of Week 14): the CI
dashboard all green; coverage vs the floor; one defect walked from bug report →
failing test → fix → green.

**Dependencies & blocking decisions.** M2–M8 merged; a disposable PostgreSQL in
CI. Triage the backlog on day 1 and cut minors if the week is tight.

**Owners.** Primary A (coverage, harness, e2e). Supporting: B, C, D each sweep
their own components and fix their own defects.

---

## M10 — Week 15 — Documentation & Final Presentation

**Goal.** Make the project understandable and reproducible by an outsider.
**No new features** — docs and demo scripting only.

**Must-have deliverables**

1. **`README.md`** — status flipped from "scaffold" to a per-component
   implemented / tested / known-gaps view; build / run / demo instructions.
2. **`docs/RESULTS.md`** — capability matrix, final metrics vs targets, coverage
   number, chart screenshots, known limitations and future work. Reconcile
   `ARCHITECTURE.md` / `DATA_FLOW.md` / `OPEN_QUESTIONS.md` with the final code.
3. **User & developer guides** — run a backtest and read a report; build, test
   tiers, and how to add a strategy / risk policy / fill model.
4. **`scripts/demo.*`** — one command that runs the graded demo end to end on a
   clean checkout.
5. **De-scaffold** — `kIsScaffold = false`, version bump, banner text, and the
   matching smoke-test update (a single small PR).
6. **`LICENSE`** (or an explicit "no licence — coursework, all rights reserved"
   note) and repo visibility set to match the Week-1 advisor decision (`OQ#14`).

**Acceptance criteria**

- A teammate or the advisor reproduces the demo from a clean clone using the
  README + user guide, without asking questions.
- Every `docs/` file is consistent with the code; `OPEN_QUESTIONS.md` has no
  undecided critical-path item.
- CI green on the final commit; `scripts/demo` works in CI or a documented
  environment.
- Repo visibility and licence state match the advisor decision.

**Tests / demo.** No new features; the suite is green on the final commit; a
docs link-check and a dry-run of the guide command blocks. **Final
presentation:** live (or recorded-backup) `scripts/demo` run — configure →
backtest → risk-constrained fills → portfolio → analytics → charts — with slides
on architecture, the event-driven / concurrency story, determinism, metrics vs
targets, testing, and per-member ownership.

**Dependencies & blocking decisions.** M9 green; final M8 numbers; a stable demo
fixture + config. Feature freeze at the end of M9.

**Owners.** Primary A (coordinates). Supporting: C (user guide), C/D (developer
guide), D (results), B (demo script + arch/data-flow reconcile).

---

## Persistence stages (PostgreSQL)

**Owner.** Member D leads (`persistence/**`, `database/**`,
`trading_engine_postgres_adapter`). Member B assists on replay/analytics read
paths. Member A reviews every adapter change for core isolation.

| Stage | When | Deliverable | Done when |
|-------|------|-------------|-----------|
| **P1 — Foundations** | M1–M2 | Client library, migration tool and schema shape decided (`OQ#5`, `OQ#6`; numeric type from the M1 ADR). A disposable test database in CI (service container) and the in-memory `IMarketDataRepository` / `ITradeRepository` doubles. | The decisions are ADRs; CI can spin a throwaway PostgreSQL; the in-memory doubles pass their unit tests. |
| **P2 — Market events** | M2–M3 | `001` migration for `runs` + `market_events` (**up and down**); `PostgresRepository` market-data write/read path. | `it_postgres_market_data` stores a batch and reads it back field-for-field equal; the migration applies and rolls back cleanly in CI; replay can source from the DB. |
| **P3 — Trade artefacts** | M5 | Migration for `orders` + `fills` + `portfolio_snapshots` (up and down); `ITradeRepository` write path. A **shared repository contract test** run against both the in-memory double and `PostgresRepository`. | An end-to-end backtest persists every order/fill/snapshot and reloading reproduces the exact final `PortfolioSnapshot`; the contract suite is green against both implementations. |
| **P4 — Analytics, resilience & docs** | M7–M9 | Migration for `performance_runs`; report read path feeding `plot_performance.py`; connection-loss handling (typed error, graceful degradation); `database/README.md` runbook; retention / partitioning decision recorded. | `it_report_from_run` round-trips the report through the DB; a connection-drop test passes; the runbook exists; the persistence integration suite is required-green in CI. |

**Invariants (hold throughout)**

- PostgreSQL / libpq / libpqxx symbols appear only in
  `trading_engine_postgres_adapter` (CI-checked).
- No credentials committed; connection via `DATABASE_URL`;
  `config.example.json` names env vars only.
- `main` CI passes without a live database — integration tests are opt-in
  (`-DTRADING_ENGINE_BUILD_INTEGRATION_TESTS=ON`) and run against a CI service
  container or auto-skip.
- From P3, the in-memory and PostgreSQL repositories share one contract-test
  suite.
- Every migration ships an `up`, a `down`, and a test that applies both.

---

## Risks

| Risk | Likelihood / impact | Mitigation | Owner |
|------|---------------------|------------|-------|
| Architecture / concurrency complexity — event-bus races, lost wakeups, deadlock | Med / High | Simplest correct design first (mutex+condvar is fine for v1); TSan in CI; multi-producer property tests; lock-free only as an M8 experiment behind a flag | A |
| External API reliability — Alpaca REST + stream | Med / Med | Unit tests use recorded fixtures, never the live vendor; credential-gated live tests auto-skip; the live minimum has an M3 deadline with M9 as a named backstop | B |
| Database / persistence complexity | Med / Med | Staged P1–P4; in-memory double + shared contract tests; `main` CI never needs a live DB; every migration tested both ways | D |
| Monetary correctness | Low / High | Numeric representation decided in Weeks 1–2, before the schema and domain harden; hand-computed golden P&L fixtures; `equity == cash + Σ position_value` property test | D |
| Schedule / scope pressure | High / Med | Required vertical slice before any stretch; one-week milestones (M6, M7) parallelise across four people; milestone detail is refined, not expanded, as work reveals more | all |
| Performance targets not met | Med / Low | Graded on measurement + honest analysis, not a fixed number; measure first, profile before optimising, report variance | A |
| Cross-platform / toolchain differences | Med / Med | Platform + compiler floor decided in M1; CI matrix runs from Week 1; cache `_deps`, allow a system GoogleTest | A |
| Repo visibility / licensing + team coordination | Low / Med | Advisor decision in Week 1 (`OQ#14`); ownership means coordinator, not gatekeeper; weekly sync rebalances load | all |

---

## Decisions to settle in Weeks 1–2

Full context and options are in [`OPEN_QUESTIONS.md`](OPEN_QUESTIONS.md); record
each outcome as an ADR under [`adr/`](adr/).

**Must resolve — these block M2:**

- `OQ#1` event shape & v1 `MarketEventType`
- `OQ#2` queue / threading model
- `OQ#3` backpressure, shutdown & draining semantics
- `OQ#11` (money) — numeric representation (`double` vs fixed-point)
- `OQ#13` supported platforms & compiler floor
- `OQ#7` configuration format
- `OQ#8` logging approach
- `OQ#4` Alpaca adapter libraries (HTTP + JSON + WebSocket)
- `OQ#5` + `OQ#6` PostgreSQL client library, migration tool & schema shape
- `OQ#14` repository visibility & licensing (advisor call — repo is currently
  public with no `LICENSE`)
- Non-functional targets → `docs/TECH_REQUIREMENTS.md`

**Record now, ratify at the owning milestone:**

- `OQ#9` reference strategies + signal semantics + warm-up → M4
- `OQ#10` risk v1 limits + breach behaviour → M6
- `OQ#11` (behaviour) fill / slippage / latency / shorting / partials → M5
- `OQ#12` analytics export format + metric definitions → M7
- `OPEN_QUESTIONS.md` "smaller items" → fold into whichever ADR touches them
