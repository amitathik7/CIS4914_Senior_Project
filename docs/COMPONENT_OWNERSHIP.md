# Component Ownership

A four-way split of **coordination responsibility**. Placeholders **Member A /
B / C / D** — the team writes in real names in Week 1 via PR.

**Ownership means coordinator + primary reviewer, not exclusive access.**
Anyone may edit any file. The owner is the required reviewer for their area, the
tie-breaker for design questions scoped to it, the keeper of its docs/tests, and
the person to ask first. Every area has a backup owner (the other member most
familiar with it) who covers when the owner is away.

The [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) names the primary and
supporting owner areas for each milestone; this file is the reference for what
those areas contain.

## Primary areas

| Member | Area | Covers |
|--------|------|--------|
| **A** | Core framework, event bus, build & observability | `common/` (types, clock, errors, logging facade), `events/`, `configuration/`, `app/`, `analytics/system_metrics.*`, `cmake/` + root `CMakeLists.txt`, `.github/` (CI, issue templates), `benchmarks/`, `tests/support/`. Backup: B. |
| **B** | Market data, ingestion & backtesting | `market_data/` (sources, `MarketDataService`, `AlpacaHistoricalSource`, `AlpacaLiveSource`), `backtesting/` (`HistoricalReplay`), `trading_engine_alpaca_adapter` + its private deps, `tests/fixtures/`, the determinism harness. Backup: A. |
| **C** | Strategy, risk & analytics | `strategy/` (`StrategyEngine`, reference strategies), `risk/` (`RiskManager`, policies), `analytics/performance_analyzer.*`, `python/visualization/`, `docs/EXPORT_SCHEMA.md`. Backup: D. |
| **D** | Execution, portfolio & persistence | `execution/` (`NaiveFillModel`, `ExecutionSimulator`), `portfolio/` (`PortfolioManager`), `persistence/` (repository interfaces, `PostgresRepository`), `trading_engine_postgres_adapter` + its private deps, `database/` (schema + migrations). Backup: C. |

`domain/` value types are **shared**: any change needs two approvals, including
the owner of the component that most depends on that type.

Most `docs/` are maintained by A; C keeps `python/visualization/` docs and the
user guide; D keeps `RESULTS.md` and the persistence runbook.

## Cross-cutting responsibilities

- **Persistence track (P1–P4)** — D leads, B assists on read paths, A reviews
  adapter isolation. Stages and acceptance criteria are in the plan.
- **Live Alpaca ingestion** — B. The minimum path is a required deliverable
  (starts M2, due end of M3); advanced resiliency is stretch (M8).
- **CI / build / benchmarks / logging facade / determinism harness** — A owns
  the plumbing; each component owner writes their own tests and benchmarks.

## Reviewer relationships

| Change | Approvers |
|--------|-----------|
| Within one area | its owner + one other member |
| `domain/` value types | two members, incl. the top dependent component's owner |
| Adapter third-party dependency | the adapter's owner (B or D) + A (confirms it stays out of `trading_engine_core`) |
| CMake target / adapter structure, `cmake/` | A + one other |
| A cross-cutting ADR (threading, numeric representation, platforms, logging, config, export schema) | all four members |
| Repository visibility / licensing (`OQ#14`) | all four + the course advisor |
| Docs only | the doc's maintainer |

Everyone reviews outside their area regularly. Aim: by Week 10, no component is
understood by only one person.

## Load balancing

- **A** carries the spine (framework, build, CI) with a steady review load all
  term, and leads M1–M2 and M8–M10.
- **B** front-loads on market data and replay (M2–M3), then shifts to support
  and hardening.
- **C** ramps up via M1 drafting (strategy shortlist, risk v1, metric
  definitions) and peaks at M4 and M6–M7.
- **D** spreads the persistence track (P1–P4) and the numeric-representation ADR
  across the whole term, peaking at M5.

If a week looks overloaded, reassign work at the weekly sync — only review
responsibility is fixed. To change ownership, PR this file and get all four
approvals.
