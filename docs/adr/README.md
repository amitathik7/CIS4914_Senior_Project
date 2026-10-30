# Architecture Decision Records

Short documents capturing a significant architectural decision, its context,
and its consequences. Format: lightweight [MADR](https://adr.github.io/madr/).

| # | Title | Status |
|---|-------|--------|
| [0001](0001-provisional-architecture.md) | Provisional architecture for the simulated trading engine | Proposed / Provisional |
| [0002](0002-strategy-risk-signal-contract.md) | Strategy Engine → Risk Manager signal contract (draft JSON shape) | Proposed |
| [0003](0003-kafka-transport.md) | Transport: Kafka behind `IEventBus`, superseding ADR 0001 item 12 | Proposed |
| [0004](0004-execution-portfolio-fill-contract.md) | Execution Simulator → Portfolio Manager fill contract | Proposed |
| [0005](0005-portfolio-risk-query-contract.md) | Portfolio Manager → Risk Manager query contract | Proposed |
| [0006](0006-web-frontend-api-contract.md) | Engine → web console API contract | Proposed |

> ADR 0003 records the team's decision to use Kafka and how it is applied
> (Kafka as an adapter behind `IEventBus`). ADR 0004 depends on it.

## Adding one

1. Copy an existing file to `NNNN-short-title.md` (next number).
2. Fill in Status / Date / Deciders / Context / Decision / Consequences.
3. When a decision in [`../OPEN_QUESTIONS.md`](../OPEN_QUESTIONS.md) is settled,
   write the ADR and link it from that question.
