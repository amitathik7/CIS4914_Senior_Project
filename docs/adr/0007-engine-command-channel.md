# 7. Engine command channel: console commands and run settings over Kafka

- Status: **Proposed** (not Accepted - draft prepared for review)
- Date: 2026-10-30
- Deciders: all four members. Jordan wrote the engine service and the gateway
  side; the Risk Manager owner signs for the kill switch, the console owner
  for the write endpoints' behaviour.
- Related: ADR 0003 (Kafka behind `IEventBus`; this channel uses the same
  broker and envelope), ADR 0006 §2 (the write endpoints this serves) and §4
  ("Kill switch state ... Risk Manager"), plan M6 (manual kill switch),
  [`../ENGINE_GATEWAY.md`](../ENGINE_GATEWAY.md),
  [`../EVENT_BUS.md`](../EVENT_BUS.md).

> **Implementation status (2026-10-30).** Built: `src/control/` (messages,
> codec, `RunSettings`), `app::ConfiguredRun`, `app::EngineService` and
> `trading_engine serve`, the Risk Manager kill switch, `<prefix>.run` from
> `trading_engine run`, and the gateway's three write endpoints. Tested with
> gtest over the in-memory broker, Python tests with a fake engine, and by
> hand against a single-node Kafka 4.1 broker.

## Context

ADR 0006 gives the console three write endpoints: start a backtest, stop a
run, and the kill switch. The engine gateway answered all three with 501,
because nothing in the engine could receive a request: `trading_engine run` is
a one-shot program, and the bus topics (ADR 0003) only carry what happens
*inside* a run.

The topics also did not carry a run's **configuration**. The gateway showed
the engine's default risk limits and starting cash for every run, and derived
the portfolio from a starting cash given on its own command line, so a run
started with `--cash 300` showed the wrong cash and equity.

Constraints:

1. The gateway must stay off the trading path (ADR 0006 §1): a command must not
   make it a participant in a run.
2. One topic prefix is one run (ADR 0003). A backtest that has not started has
   no prefix yet.
3. The core library takes no new dependencies; Kafka stays an adapter.
4. Tests stay broker-free and deterministic.

## Decision

### 1. Three message types on three topics

| Topic | Message | Written by | Read by |
|---|---|---|---|
| `<control>.command` | `Command` | the gateway | the engine (`trading_engine serve`) |
| `<prefix>.run` | `RunStatusReport` | the engine, for each run | the gateway |
| `<control>.engine` | `EngineAnnouncement` | the engine, every 2 s | the gateway |

`<control>` defaults to `trading_engine.control`; `<prefix>` is the run's topic
prefix, which is also its run id. All three use the event bus envelope
(schema 1: `origin`, `sequence`, `type`, `enqueued_at`, `payload`), so the
gateway reads every topic with one decoder. They are **not** bus `EventType`s:
they are about runs rather than inside one, and a run's bus never subscribes
to them. Code: `include/trading_engine/control/`, `src/control/`.

### 2. Commands

```json
{"command_id":"gateway-3f2a-9c1d","action":"start_backtest","run_id":"bt-20261030-101500-3f2a",
 "issued_at":"2026-10-30T10:15:00.000000Z","settings":{ RunConfig }}
{"command_id":"...","action":"stop","run_id":"bt-...","issued_at":"..."}
{"command_id":"...","action":"set_kill_switch","run_id":"bt-...","issued_at":"...","engaged":true,"reason":"..."}
```

- The **sender chooses the run id** (`bt-<UTC time>-<4 hex>`), so it knows
  which `<prefix>.run` to watch before the run exists.
- `settings` is the console's `RunConfig` (`web/src/api/contract.ts`) exactly:
  the gateway forwards the `BacktestRequest` with `mode: "backtest"`. The
  engine adds `risk.policies` (the starter policies on/off) when it reports.
- The engine **answers every command** with a `RunStatusReport` carrying the
  command's `command_id` (and `error` if it refused). That is how the gateway
  turns an asynchronous channel into a synchronous HTTP answer: it writes the
  command, then waits (10 s) for the report with that id.
- A command for a run the engine does not own is ignored, so several engines
  can share a cluster. A duplicate `start_backtest` (same run id) is ignored,
  never answered, so it cannot overwrite another run's status.

### 3. Run status reports carry the settings

```json
{"run_id":"bt-...","engine_id":"engine-99d5e3f7","status":"running","updated_at":"...",
 "config":{ RunConfig },"kill_switch":{"engaged":true,"changed_at":"2026-01-05T14:46:00.000000000Z","reason":"..."},
 "progress":0.42,"clock":"2026-01-05T14:46:00.000000000Z","command_id":"...","error":"..."}
```

Each report is the run's **whole** state, so a reader needs only the latest
(by `updated_at`). Published when a run is queued, starts, changes its kill
switch, every 2% of progress (or 0.5 s), and when it ends (`completed`,
`stopped`, `failed`). `config` holds the settings the run actually used, with
the period filled in from the bars when it was not given.

`trading_engine run` publishes the same report (`running`, then `completed`)
on a topic bus, **without** `engine_id`: nothing can send it commands, and
the gateway answers 409 if the console tries.

### 4. The engine: `trading_engine serve`

`app::EngineService` is the engine as a long-running process. It reads
`<control>.command`, heartbeats on `<control>.engine` (its recorded data's
symbols and sessions, and its queued runs), and runs backtests **one at a
time, in order**: a second request is `queued` until the first ends. Each run
is an `app::ConfiguredRun` (the same composition `trading_engine run` uses:
`Pipeline` + `OrderFlow` + the settings' strategies) on its own
`TopicEventBus`, so it appears in the console exactly like a command-line run.

`start_backtest` is refused (a `failed` report with `error`) when the settings
are invalid (`control::validate`; the error is `"<field path>: <message>"`,
e.g. `strategies.0.long_window: ...`), no recorded bar matches, or the queue
is full (8). Short selling and live runs are refused: the portfolio cannot
short and there is no live feed yet.

### 5. The kill switch lives in the Risk Manager

`RiskManager::set_kill_switch(engaged, reason)`: while engaged, every signal
is rejected **before any policy runs**, with reason `kill_switch` and the
operator's text as the check detail, so the console's risk log shows why.
`changed_at` is the run's market time. Orders already working are not
cancelled (the simulator has no cancels yet). A kill switch set while a run is
queued is applied when it starts.

### 6. The gateway

- `POST /v1/runs`: 503 if no engine has announced itself within
  max(6 s, 3 heartbeats); otherwise sends `start_backtest` and answers with
  the run's summary (`queued` or `running`), or 400 with `field` from the
  engine's refusal, or 504 if no answer arrives.
- `POST /runs/{id}/stop`, `PUT /runs/{id}/kill-switch`: 409 when the run has
  no live engine (a command-line run, or its engine stopped heartbeating).
- `GET /v1/engine`'s `market_data` comes from the engines' announcements, so
  the New backtest form offers exactly the data the engine can replay.
- A queued/running run whose engine stops heartbeating is shown `failed`.
- Run detail shows the reported `config` and kill switch; the derived
  portfolio and report start from the reported starting cash.
- `--replay` and `--read-only` gateways send nothing and keep answering 501.

## Consequences

- The console's New backtest form, Stop and kill switch work against the real
  engine. Run detail shows real limits and strategy parameters.
- The gateway is still off the trading path: it writes one message per click
  and reads the answer like any other topic. A slow browser cannot slow a run.
- One more process to start for console backtests (`trading_engine serve`).
  `trading_engine run` keeps working and its runs still appear, read-only.
- The core stays standard-library only; `serve` needs the Kafka build (the
  in-memory broker drives `EngineService` in the tests).
- Commands are at-least-once and unauthenticated, like the gateway itself
  (loopback only, ADR 0006 open point 4). A redelivered command is harmless:
  starts are de-duplicated by run id, stop and kill switch are idempotent.

## Not done / open points

1. Latency (`latency_ms`) and `participation_cap` are accepted and reported
   but the fill model ignores them (`NaiveFillModel` fills the whole order at
   the next bar's open).
2. Runs replay the CSV bars given to `serve --bars`; replaying from
   persistence or Alpaca belongs to the market-data work.
3. One run at a time per engine. Running several needs per-run threads and a
   decision on CPU budget; the message design does not change.
4. No command authentication or audit trail beyond the topic itself.
5. The engine does not cancel working orders when the kill switch engages.
