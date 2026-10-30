# 3. Transport: Kafka behind `IEventBus`

- Status: **Accepted** (2026-10-30)
- Date: 2026-10-30
- Deciders: all four members. The team has already chosen Kafka as the
  inter-component communication layer; this record writes that decision down
  and proposes *how* it is applied. Each component owner signs for the
  consequences in their area (see "Consequences").
- Supersedes: [ADR 0001](0001-provisional-architecture.md) item 12
  ("In-process, single-node engine. No distribution, no IPC.").
- Related: [`../OPEN_QUESTIONS.md`](../OPEN_QUESTIONS.md) OQ#2 (queue and
  threading) and OQ#3 (shutdown and backpressure), which this answers for v1;
  ADR 0004 §0 and §10-§11 (which were written to survive either transport);
  ADR 0006 §1 and §3 (the gateway reads the bus; the JSON encoding rules);
  [`../EVENT_BUS.md`](../EVENT_BUS.md) (the user guide for what is built).

> **Implementation status (2026-10-30).** Built as decided and merged to
> `main`: `InProcessEventBus`, `TopicEventBus` with `InMemoryTransport` and
> `RdKafkaTransport` (`trading_engine_kafka_adapter`, tested against
> librdkafka's mock cluster in CI), `PortfolioBusBridge`, `app::Pipeline` and
> `app::OrderFlow` (risk and execution on the bus), the `trading_engine run`
> CLI, and the Python engine gateway reading the topics
> ([`../EVENT_BUS.md`](../EVENT_BUS.md), [`../ENGINE_GATEWAY.md`](../ENGINE_GATEWAY.md)).
> One addition to §2: a fifth topic, `<prefix>.risk_decision`
> (`EventType::RiskDecision`, `domain::RiskDecisionReport`), so the console
> can show why a signal was approved or rejected. The "not built yet" note
> for risk and execution under Consequences is historical.

## Context

Components (market data, strategies, risk, execution, portfolio, analytics,
the web console's gateway) need to exchange typed events. ADR 0001 assumed one
process and an in-memory queue. The team has since decided to use **Kafka**,
which makes every component boundary potentially cross-process: components
can run, restart and be demonstrated separately, the event history is durable
and replayable, and the web gateway can read the same stream the engine uses.

Two concerns pull the other way:

1. **Tests and backtests must stay fast and deterministic.** 340+ unit tests
   run in about a second with no services. A broker in every test run would
   make them slow and flaky, and M5 lists deterministic replay as an
   acceptance criterion.
2. **The core library takes no third-party dependencies** (ADR 0001, the
   adapter pattern used for Alpaca and PostgreSQL). Linking a Kafka client into
   every component would break that.

## Decision

### 1. Components depend on `IEventBus` only; Kafka is an adapter

`events::IEventBus` stays the only thing a component sees. Its contract is
written down in [`event_bus.hpp`](../../include/trading_engine/events/event_bus.hpp)
(serialized handlers, `unsubscribe()` barrier, publish-from-handler, drain
semantics, refusal = `false`) and both implementations meet it:

| Implementation | Where | Use |
|---|---|---|
| `InProcessEventBus` | core | Unit tests, deterministic backtests, single-process runs. Bounded queue, one dispatcher thread. |
| `TopicEventBus` + `RdKafkaTransport` | core + `trading_engine_kafka_adapter` | **The production transport.** Each event becomes a Kafka message; local subscribers receive it back from Kafka, exactly as a subscriber in another process would. |
| `TopicEventBus` + `InMemoryTransport` | core | The Kafka code path without a broker, for tests. |

`TopicEventBus` holds everything engine-specific (topics, keys, encoding,
dispatch, lifecycle). The transport seam (`IMessageTransport`) only moves
bytes, so librdkafka is confined to one small adapter, linked `PRIVATE`, and
optional at build time (`-DTRADING_ENGINE_WITH_KAFKA=ON`).

### 2. Topics, keys, ordering

- One topic per event type: `<prefix>.market_data`, `.signal`, `.order`,
  `.fill`. The prefix is configurable per run (e.g. `te.run42`) so runs do not
  read each other's history.
- Message key = the payload's symbol. Kafka orders messages per partition, so
  **all events for one symbol stay in order**; events for different symbols,
  or of different types, may interleave differently than they were published.
  Every consumer that exists today (strategies, portfolio marks) needs only
  per-symbol order. A consumer that needs cross-type order for one symbol
  (e.g. an order's `Working` before its first `Fill`) gets it only if both are
  published by one producer in that order, which is how ADR 0004's execution
  simulator is specified. If that proves insufficient, the fix is one combined
  topic per symbol group, not a change to components.
- Every bus instance reads **every partition** of its topics (manual
  assignment, no consumer group): an event bus delivers each event to every
  subscriber, in every process. A process that wants to share work with copies
  of itself (consumer-group semantics) is out of scope for v1.
- A new subscriber starts at the end of each topic as it is at `start()`.
  `from_beginning` replays the retained history instead (useful for the
  gateway and for post-run analysis).

### 3. Wire format: versioned JSON following ADR 0006's encoding rules

One UTF-8 JSON object per message: `{"schema":1,"origin","sequence","type",
"enqueued_at","payload":{...}}`. Prices and money are exact decimal strings,
quantities and ids integers, times RFC 3339 UTC with nanoseconds, absent
optionals omitted. Chosen over a binary format (Avro/Protobuf) because it adds
no dependency and no schema registry, it is the format ADR 0006 already
requires of the gateway (so the gateway can consume topics directly, in any
language), and it is debuggable with `kafka-console-consumer`. Throughput is
not a v1 concern (OQ performance work is M7+); the codec is behind one
function pair and can be swapped. Decoding is strict, unknown members are
ignored, and `schema` is bumped on any incompatible change.

### 4. Backpressure and shutdown (answers OQ#3 for v1)

- `InProcessEventBus`: bounded, `Block` by default (a backtest must not lose
  bars), `Reject` available for a live feed that cannot be paused.
- `TopicEventBus`: no capacity of its own; librdkafka's producer queue is the
  bound, and a full queue makes `publish()` return `false`.
- Both: `request_shutdown()` stops accepting events from outside the bus, but
  handlers may still publish while draining, so the signal for a bar queued
  before shutdown is not lost. `wait_until_drained()` returns when everything
  accepted has been dispatched (the topic bus: when every message it published
  has come back from Kafka and been handled, bounded by `drain_timeout`).

### 5. Determinism

Determinism is a property of the in-process path. A backtest that must be
byte-identical on rerun (M5) runs on `InProcessEventBus`. Through Kafka the
per-symbol results are identical (the pipeline test checks the same signals
field for field on every bus), but cross-symbol interleaving, and therefore
the numbering of `SignalId`s across symbols, can differ with partitioning.

## Consequences

- **Market data / strategies / portfolio**: no code change to run over Kafka;
  `app::Pipeline` wires them through whichever bus it is given.
- **Risk and execution (not built yet)**: subscribe and publish on `IEventBus`
  like the others. They must publish an order's lifecycle events and its fills
  from one producer, in order (§2).
- **Web console gateway (ADR 0006)**: can consume the four topics with any
  Kafka client and serve the read model. The wire format already follows the
  API's encoding rules, so mapping is field renaming, not re-encoding.
- **Build**: Kafka support is opt-in. Windows/MSVC: `vcpkg install librdkafka`
  then `-DTRADING_ENGINE_WITH_KAFKA=ON` with the vcpkg toolchain. Linux:
  `librdkafka-dev`. macOS: `brew install librdkafka`.
- **Testing**: the Kafka adapter is tested against librdkafka's in-process
  mock cluster, so CI needs no broker; a real broker is optional
  (`TE_KAFKA_BOOTSTRAP`).
- **ADR 0001 item 12** is superseded; components may now run in separate
  processes. Ids (`SignalId`, `OrderId`) are still per process; anything that
  correlates them across processes needs run/origin scoping (already noted in
  ADR 0002).

## Alternatives considered

- **Kafka client calls inside each component.** Rejected: couples every
  component and every test to a broker, and breaks the no-dependency core.
- **Only the in-process bus, Kafka later.** Rejected: the team decided on
  Kafka; building the seam now means nothing changes for components later.
- **Consumer groups per component.** Deferred: needed only for horizontal
  scaling of one component, which v1 does not do; manual assignment avoids
  rebalance delays and is simpler to reason about.
- **Avro/Protobuf + schema registry.** Deferred: more infrastructure than the
  project needs now; revisit with the M7 performance work.
