# Architecture Decision Records

Short documents capturing a significant architectural decision, its context,
and its consequences. Format: lightweight [MADR](https://adr.github.io/madr/).

| # | Title | Status |
|---|-------|--------|
| [0001](0001-provisional-architecture.md) | Provisional architecture for the simulated trading engine | Proposed / Provisional |
| 0003 | *(reserved)* Transport: Kafka, superseding ADR 0001 item 12 | Not yet written |
| [0004](0004-execution-portfolio-fill-contract.md) | Execution Simulator → Portfolio Manager fill contract | Proposed |

> **0002 is missing from this table on purpose.** "Strategy Engine → Risk
> Manager signal contract" (issue #3) lives on an unmerged branch; linking it
> from `main` would be a broken link. It takes the 0002 slot on merge.
>
> **0003 is reserved, not written.** The team's decision to move the pipeline
> onto Kafka supersedes ADR 0001 item 12 and is not yet recorded anywhere.
> ADR 0004 depends on it.
>
> *Merge note:* the ADR 0002 branch edits this same table, so expect a
> conflict here. Resolution is to keep all rows.

## Adding one

1. Copy an existing file to `NNNN-short-title.md` (next number).
2. Fill in Status / Date / Deciders / Context / Decision / Consequences.
3. When a decision in [`../OPEN_QUESTIONS.md`](../OPEN_QUESTIONS.md) is settled,
   write the ADR and link it from that question.
