# Open Questions

Decisions the project proposal does **not** settle. Each needs a team call
before or during the milestone that depends on it
([`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md)). Record the outcome as an
ADR under [`adr/`](adr/) and update the affected headers.

Legend: **Owner** = who drives the decision · **By** = milestone it blocks.

---

## 1. Exact market event types
`domain::MarketEventType` is currently `{ Unknown, Trade, Quote, Bar, Status }`.
- Do strategies need L2 order-book deltas? imbalance? corporate actions?
- Are bars produced by the feed, or aggregated by us from trades?
- One `MarketEvent` with optional fields (current) vs a `std::variant` of
  distinct trade/quote/bar structs?
- **Owner:** strategy + market-data leads · **By:** M1

## 2. Queue and threading model
> **Superseded in part, pending an ADR.** The team has decided to move the
> pipeline onto **Kafka**, making every component boundary cross-process. That
> reverses ADR 0001 item 12 ("in-process, single-node engine. No distribution,
> no IPC.") and is **not yet recorded in any ADR** - expected to be 0003. The
> in-process options below only remain live under the alternative of keeping
> `IEventBus` as the component-facing seam with Kafka behind it as an adapter
> (the pattern already used for Alpaca and PostgreSQL). Note that the
> cross-process option costs deterministic replay, which M5 lists as an
> acceptance criterion - see `adr/0004-execution-portfolio-fill-contract.md`
> §10–§11.

- One bus with a single MPMC queue, or per-`EventType` queues (SPSC/MPSC)?
- `std::variant` payload (current) vs type-erased `Event`?
- Thread count: one consumer thread per stage, a pool, or pinned threads?
- Ring buffer (e.g. capacity = power of two) vs `moodycamel`-style queue vs
  `std::deque` + mutex/condvar for v1?
- **Owner:** systems/perf lead · **By:** M1

## 3. Shutdown and backpressure semantics
- Bus full → **block producer**, **drop oldest**, or **reject (publish returns
  false)**? Possibly per-`EventType`.
- Live feed cannot be paused — what happens to dropped market data (count?
  gap-mark? widen queue?)?
- Shutdown: stop accepting → drain in-flight → stop consumers, with a timeout?
  What is the hard-kill fallback?
- Does `wait_until_drained()` have a deadline?
- **Owner:** systems/perf lead · **By:** M1

## 4. Alpaca client libraries
- WebSocket: Boost.Beast, `websocketpp`, `libwebsockets`, or IXWebSocket?
- HTTP (historical REST): `libcurl`, `cpr`, Boost.Beast, or `httplib`?
- JSON: `nlohmann/json`, `RapidJSON`, `simdjson` (parse-only), or Boost.JSON?
- TLS: OpenSSL vs schannel/native.
- All of the above must stay **private** to `trading_engine_alpaca_adapter`.
- **Owner:** market-data lead · **By:** M1 — pick the libraries for the
  historical REST source **and** the required live WebSocket source together.
  Advanced live resiliency (reconnect/backoff/heartbeat/gap-recovery) is
  stretch (M8), but the WebSocket library is chosen now.

## 5. PostgreSQL client library
- `libpqxx` (C++ wrapper) vs raw `libpq` vs an ORM-ish layer?
- Connection pooling: in-process pool we write, or PgBouncer?
- Bulk market-data insert: multi-row `INSERT`, `COPY`, or batched prepared
  statements?
- Must stay **private** to `trading_engine_postgres_adapter`.
- **Owner:** persistence lead · **By:** M1 (decided with `OQ#6`); implemented
  across persistence stages P1–P4.

## 6. Database schema and migration tool
- Migration runner: plain `psql` scripts, Flyway, Liquibase, sqitch, dbmate,
  Alembic-style, or a small C++ runner?
- Time-series handling for `market_events`: native declarative partitioning vs
  TimescaleDB hypertables.
- Numeric representation: `NUMERIC(18,6)` vs integer minor units (see Q11).
- `positions` as a child table vs `JSONB` on `portfolio_snapshots`.
- Retention / archival policy for high-volume market data.
- **Owner:** persistence lead · **By:** M1 (schema shape + migration tool);
  the retention policy can be recorded later, by persistence stage P4.

## 7. Configuration format
- JSON (example provided), TOML, YAML, or an env-var-only scheme?
- Layering: file → environment overrides → CLI flags; precedence order?
- Schema validation approach (hand-rolled vs JSON Schema vs a library).
- Where does the config file live and how is it discovered?
- **Owner:** whoever implements `load_config` · **By:** M2

## 8. Logging library
- `spdlog`, `glog`, Boost.Log, or a thin wrapper over `std::format` + sinks?
- Structured (JSON) logs vs text; per-module log levels.
- Correlation id per `Event` threaded through the pipeline?
- Performance: is logging on the hot path allowed, or async-only?
- **Owner:** systems lead · **By:** M1–M2

## 9. Initial strategies
- Which reference strategies ship first? Candidates: SMA/EMA crossover,
  price-threshold, momentum/breakout, mean-reversion (z-score).
- Signal semantics: target exposure (fraction of equity) vs explicit quantity
  vs delta? (`TradeSignal` currently allows either.)
- Warm-up handling: how does a strategy request history before it can act?
- **Owner:** strategy lead · **By:** M3

## 10. Risk limits
- Which limits are in the v1 set, and what are the default numbers?
  (`config::RiskLimits` has placeholders.)
- On breach: hard reject vs auto-resize to the max allowed?
- Are limits per-symbol / per-strategy / per-portfolio, or global only?
- Daily-loss stop: session boundary definition and reset time (exchange TZ?).
- Kill-switch: manual only, or automatic on N consecutive rejects / drawdown?
- **Owner:** risk lead · **By:** M3

## 11. Execution assumptions

**Split.** The money / numeric-representation question is decided in **Weeks
1–2** (the M1 numeric-representation ADR), because it touches `domain/` value
types, the PostgreSQL schema, portfolio accounting, execution, risk and
analytics. The rest is drafted in Weeks 1–2 and ratified at **M5**.

- **Money / price type — DECIDE WEEK 1–2:** keep `double` or move to
  fixed-point / integer minor units? Cross-cutting; all four members sign off.
- Fill model v1: full instant fill? spread crossing? volume participation cap?
- Slippage model: fixed bps (current field) vs function of size/volatility.
- Latency: fixed `signal_to_order` / `order_to_fill` (current) vs distribution.
- Short selling: allowed? borrow availability modelled?
- Partial fills: enabled in v1, and how are remainders handled?
- The **Execution → Portfolio** fill contract (message shape, gross-vs-net
  pricing, partial-fill completion/ordering/idempotency) is drafted in
  [`adr/0004-execution-portfolio-fill-contract.md`](adr/0004-execution-portfolio-fill-contract.md)
  (**Proposed**, not Accepted; tracks GitHub issue #4). That ADR **does not**
  decide the money type - it is written to survive either outcome, but flags
  that the M5 acceptance criterion ("round-trip P&L asserted to the cent") is
  at risk under `double`, and that changing the type is a **breaking** wire
  change once the transport is cross-process.
- **Owner:** execution lead (money type: + all four sign off) ·
  **By:** money type **Weeks 1–2**; the behaviour half **M5**

## 12. Analytics output format
- Export as one JSON file, several files, CSV, or Parquet?
- Does it include the full equity-curve / per-trade series, or only summary
  metrics (charts then re-query the DB)?
- Metric definitions: drawdown (return- vs equity-based), annualisation factor,
  profit factor edge cases (no losing trades).
- Is there a live metrics endpoint, or file-only?
- **Owner:** analytics lead · **By:** M6

## 13. Supported platforms
- Windows is a dev machine today (MSVC). Is Linux (GCC/Clang) a first-class
  target for CI and deployment? macOS?
- Minimum compiler versions.
- Container image for running backtests / integration tests?
- **Owner:** whole team · **By:** M1 (affects CI), revisit at M8

## 14. Repository visibility & license
- The repository is **already public** at
  `github.com/amitathik7/CIS4914_Senior_Project`, with **no `LICENSE`** file
  (GitHub default: all rights reserved). The earlier "wait to add a license"
  note is superseded — the repo is already exposed.
- **Visibility:** confirm with the advisor whether public is intended, or the
  repo should be **private until submission** (course policies vary). Set it to
  match.
- **License:** choose an approach — MIT / BSD-3 / Apache-2.0 / proprietary /
  university-owned / an explicit "no licence — coursework, all rights reserved"
  note. Check the university's senior-project IP policy first.
- Confirm dependency licences are compatible (GoogleTest = BSD-3; others TBD).
- **Owner:** whole team + advisor · **By:** **Week 1** (the repo is already
  visible), revisit at M10.

---

## Smaller items

- `#pragma once` (current) vs include guards.
- Error handling: exceptions (current) vs `std::expected` / status codes on hot
  paths.
- ID type: 64-bit counter (current) vs UUID vs DB sequence; uniqueness across
  process restarts when persisted.
- `std::span` in interfaces vs `const std::vector&` (toolchain floor).
- clang-format / clang-tidy rule set.
- Namespace style: nested `trading_engine::<component>` (current) — keep?
- **Signed vs unsigned quantity is inconsistent across the domain model.**
  `Order::quantity` is `> 0` with direction carried by `side`;
  `Fill::filled_quantity` and `Position::quantity` are signed. Noted while
  drafting ADR 0004 (§4), which deliberately did **not** fix it - harmonising
  touches every component and is too broad for a two-party interface ticket.
- **Who owns open-order state?** `ExecutionSimulator::open_order_count()` and
  `RiskContext::open_order_count` place it in execution and risk;
  `PortfolioSnapshot` has no such field; GitHub issue #5 proposes moving it to
  the Portfolio Manager. ADR 0004 §8 argues for leaving it where the code
  already puts it. Issues #4 and #5 must agree - see that section.
- **`DATA_FLOW.md` line 35 contradicts `portfolio_manager.hpp`.** The diagram
  routes `Order / Fill` to the PortfolioManager, which has no entry point for
  an Order. ADR 0004 §7 resolves this in favour of the header (fills only);
  the diagram still needs correcting.
