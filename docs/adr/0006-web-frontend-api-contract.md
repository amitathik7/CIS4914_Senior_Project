# 6. Engine → web console API contract

- Status: **Proposed** (not Accepted - draft prepared for review)
- Date: 2026-10-08
- Deciders: all four members. The console is owned by Adam; the endpoints are
  served from every component's state, so each component owner signs for the
  fields their component produces (see "Field sources").
- Related: [`web/`](../../web/) (the console), ADR 0002 (signals), ADR 0004
  (fills and reservation), ADR 0005 (portfolio query), OQ#12 (export format),
  the pending transport ADR 0003 (Kafka).

## Context

The project needs a front end for the demo and for day-to-day use: watching a
run, tracing an order through the pipeline, reading risk decisions, and
comparing backtests. The engine exposes no network interface yet, and most of
its stages are still stubs, so the console cannot be built against real data
today.

Waiting for the engine would put all UI work into weeks 13-15. Instead, the
console is built now against a written contract, with an in-browser demo engine
behind the same interface. When the engine serves the contract, one setting
switches the console over (`VITE_ENGINE_API`).

The canonical definition is
[`web/src/api/contract.ts`](../../web/src/api/contract.ts). This document
explains it and records what it asks of the engine.

## Decision

### 1. Transport: REST for state, Server-Sent Events for change

- `GET` endpoints return the current read model of a run as JSON.
- `GET /v1/runs/{id}/stream` is an SSE stream of `RunEvent` messages. The
  console treats every message as "this run changed" and re-reads what it
  shows, at most a few times a second. Deltas are therefore an optimisation:
  a server that only ever sends `{"type":"run",...}` heartbeats is correct.
- The API is a **read-model gateway**. It subscribes to the engine's events (the
  in-process bus today, Kafka if ADR 0003 lands) and answers queries from what
  it has seen. It never sits on the trading path, so a slow browser cannot
  apply backpressure to the pipeline.

### 2. Endpoints (prefix `/v1`)

| Method | Path | Returns |
|---|---|---|
| GET | `/engine` | `EngineInfo`: version, data source, recorded-data coverage |
| GET | `/runs` | `RunSummary[]`, live runs first |
| POST | `/runs` | `BacktestRequest` → `RunSummary` (status `queued`) |
| GET | `/runs/{id}` | `RunDetail`: summary + `RunConfig` + kill-switch state |
| POST | `/runs/{id}/stop` | `RunSummary` |
| PUT | `/runs/{id}/kill-switch` | `{engaged, reason?}` → `RunDetail` |
| GET | `/runs/{id}/portfolio` | `PortfolioSnapshot` |
| GET | `/runs/{id}/equity` | `EquityPoint[]`, one per bar |
| GET | `/runs/{id}/bars?symbol=` | `Bar[]` |
| GET | `/runs/{id}/signals` | `TradeSignal[]` |
| GET | `/runs/{id}/risk-decisions` | `RiskDecision[]` |
| GET | `/runs/{id}/orders` | `Order[]` with status history |
| GET | `/runs/{id}/fills` | `Fill[]` |
| GET | `/runs/{id}/trades` | `Trade[]` (closed round trips) |
| GET | `/runs/{id}/metrics` | `SystemMetrics` |
| GET | `/runs/{id}/stream` | SSE of `RunEvent` |

Errors are `{"error": {"code", "message", "field?"}}` with a 4xx/5xx status.
`field` is a dotted path into the request (`strategies.0.long_window`), and
`message` is written for the person filling in the form; the console shows it
verbatim next to that field.

### 3. Encoding

Follows the Strategy Lab's JSON rules so the two do not diverge:

- **Money and prices are exact decimal strings** (`"150.02"`), at most six
  places, never an exponent: the text of `common::Decimal`. Never a JSON
  number, because JavaScript doubles cannot hold every int64 millionth.
- **Quantities are JSON integers** (whole shares). Share counts stay far below
  2^53.
- **Times are RFC 3339 UTC** with a `Z`. Sub-millisecond digits are welcome; the
  order trace shows microseconds.
- Absent optional members are **omitted, never `null`**.
- Enum spellings are the lower-snake form of the C++ `to_string()` values
  (`partially_filled`, `pending_risk`).
- Ratios that cannot be computed are omitted with a status member, as in
  `profit_factor_status: "no_losing_trades"`.

### 4. What the console needs beyond today's domain types

Most fields map one-to-one onto `domain::` and `portfolio::` types. The
additions below are the ones the screens need; each is a read-model concern
and none changes a component boundary.

| Addition | Why | Produced by |
|---|---|---|
| `Order.history[]` (status, time, note) | The order trace shows the lifecycle, not just the current status | Execution Simulator (each `OrderStatus` change) |
| `Fill.side` | The blotter and the price chart need direction without a join | Execution Simulator |
| `Position.mark_price`, `market_value` | Shown in the positions table | Portfolio Manager (`mark()`) |
| `PortfolioSnapshot.session_pnl`, `realized_pnl`, `unrealized_pnl` | Headline figures and the daily-loss limit meter | Portfolio Manager |
| `RiskDecision` (outcome, approved quantity, per-policy checks) | The risk log and the trace; the engine records rejections already (plan M6) | Risk Manager |
| `EquityPoint[]` | Equity and drawdown charts | Portfolio Manager snapshots, or Performance Analytics |
| `Trade[]` | Report trade list and P&L distribution | Performance Analytics |
| `PerformanceReport.win_rate`, `sharpe`, `ending_equity`, `fees_paid` | Report figures (M7 stretch metrics) | Performance Analytics |
| `SystemMetrics.stages[]`, `history[]`, `latency_histogram[]` | The System page; extends `SystemMetrics::Snapshot` | SystemMetrics |
| Kill switch state | Plan M6 requires a manual kill switch | Risk Manager |
| `EngineInfo.market_data` | The backtest form limits dates and symbols to recorded data | Persistence / market-data repository |

### 5. Definitions the console relies on

- **Session P&L**: equity now minus equity at the first bar of the current
  session. This is the quantity the daily-loss stop compares against.
- **Drawdown**: `equity / running_peak - 1`, so it is `<= 0`.
- **Volatility**: annualised standard deviation of per-bar returns
  (`sqrt(252 × 390)` for one-minute bars).
- **Sharpe**: annualised from session-close returns (`sqrt(252)`), omitted with
  fewer than three sessions. A bar-level Sharpe annualises into meaningless
  magnitudes.
- **Trade**: a round trip from flat to flat for one symbol. Its P&L includes the
  fees of every fill in it.
- **Identity the demo engine is tested on**: `total_equity − starting_cash ==
  realized_pnl + unrealized_pnl` to the micro, with fees charged to realized
  P&L. `buying_power == cash − reserved_cash` (ADR 0005).

### 6. The demo engine

`web/src/mock/` implements the contract in the browser so the console works
with no backend. It runs the documented strategy rules (SMA crossover with
exact cross-multiplied comparison and a silent baseline; population z-score
mean reversion with an inclusive latch), the starter risk policies from plan
M6, a participation-capped fill model, and cash reservation per ADR 0004. Bars
are synthetic and seeded, so every run is reproducible.

It is **not** a second implementation of the engine and must not be cited as
evidence that the engine works. The console shows a "Demo data" badge whenever
it is active, and the System page says the timings are synthetic.

## Consequences

- Front-end work proceeds in parallel with M5-M7, and the demo has a working UI
  even if a stage slips.
- The contract becomes a target for the engine's API work. A field the engine
  cannot produce should be removed here first, not faked.
- The gateway adds one process (or one thread) to run. It needs an HTTP/SSE
  library, which belongs in an adapter target so `trading_engine_core` stays
  standard-library only.
- Python/Matplotlib reporting (plan M7) remains the graded chart deliverable;
  the console's JSON export uses the same report fields and can feed it.

## Open points

1. HTTP library for the gateway (cpp-httplib, Boost.Beast, or Drogon), chosen
   with OQ#4 because the Alpaca adapter needs one too.
2. Pagination for orders and fills on long runs (cursor by id). The console
   currently caps tables at 500 rows.
3. Whether the gateway reads live state from the components (`IPortfolioView`)
   or only from events. Events-only is simpler to make thread-safe.
4. Authentication: none while the gateway binds to `127.0.0.1`, as the Strategy
   Lab does.
