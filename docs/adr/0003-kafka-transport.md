# 3. Kafka transport for the event-driven pipeline

- Status: **Proposed** (draft prepared for team review)
- Date: 2026-10-11
- Deciders: senior project team

## Context

The trading engine was originally scaffolded as an in-process, single-node
application using `IEventBus` as the boundary between components. The team has
since decided to use Apache Kafka for communication between backend components.

This supersedes ADR 0001 item 12, which described the engine as in-process with
no IPC.

The current codebase already has an `IEventBus` interface used by components,
but the production `InProcessEventBus` remains unimplemented. Kafka itself is
also not implemented yet.

The core library is intentionally dependency-free, so Kafka client libraries and
serialization libraries should not be introduced directly into
`trading_engine_core`.

## Decision

Kafka will be the transport used to move events between major backend
components.

The existing `IEventBus` interface will remain the component-facing boundary.
Components such as the Market Data Service and Strategy Engine should publish
and consume domain events through this interface without depending directly on
Kafka.

A Kafka implementation of the event bus will live in a separate adapter layer.
Kafka-specific code, configuration, producers, consumers, and serialization
logic will remain outside `trading_engine_core`.

The intended flow for market data is:

`Market Data Service -> MarketEvent -> serialization -> Kafka -> deserialization -> MarketEvent -> Strategy Engine`

Kafka messages should use an event key that preserves ordering where ordering is
required. For market data, the stock symbol is the proposed key so that events
for the same symbol can remain ordered within a Kafka partition.

The exact topic layout, partition count, replication settings, retention
settings, backpressure behavior, and final wire encoding still require team
review before they are treated as finalized.

JSON is currently being evaluated as the first wire format because it is easy to
inspect and matches the event examples already developed, but this ADR does not
yet mark JSON as a permanent transport format.

## Consequences

### Positive

- Backend components remain decoupled from one another.
- Components do not need Kafka-specific code.
- Kafka can provide scalable producer/consumer communication and retained event
  streams.
- Using a symbol as the market-data key can preserve per-symbol ordering.
- Keeping Kafka in an adapter preserves the existing dependency boundaries.

### Negative / risks

- Cross-process communication is more complex than the original in-process
  architecture.
- Events must be serialized and deserialized.
- Kafka configuration and lifecycle management must be added.
- Ordering is guaranteed only within a partition, not globally across all
  partitions.
- Deterministic replay may require additional design work.

## Open implementation decisions

The following still need to be finalized during implementation:

- Kafka topic names and whether event types share or separate topics.
- Number of partitions.
- Consumer group layout.
- Retention settings.
- Producer acknowledgement settings.
- Error handling and retry behavior.
- Shutdown and draining behavior.
- Backpressure behavior.
- Final serialization format and schema/versioning rules.

## Implementation direction

The first implementation work should focus on the Market Data Service to
Strategy Engine path.

The existing `domain::MarketEvent` should remain the canonical C++ market-data
type instead of introducing separate competing `BarEvent` and `QuoteEvent`
types.

Fields from earlier bar and quote prototypes that are still required should be
added to `MarketEvent` as needed.

Serialization should convert between the canonical C++ `MarketEvent` and the
wire representation at the Kafka adapter boundary.

## Sign-off

This ADR records the current Kafka direction but remains Proposed until reviewed
by the team.

- [ ] Data Queue owner
- [ ] Market Data owner
- [ ] Strategy owner
- [ ] Remaining team review
- [ ] Status changed to Accepted after team approval