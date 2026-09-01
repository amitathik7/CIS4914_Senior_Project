# Database

PostgreSQL is the intended store for normalised market data and simulated
trading activity. **Nothing is implemented and no schema is final.**

## Layout

```
database/
  migrations/
    001_initial_schema.sql   # OUTLINE ONLY -- commented, performs no changes
```

## Open decisions (see [`docs/OPEN_QUESTIONS.md`](../docs/OPEN_QUESTIONS.md))

- Migration tool: plain `psql` scripts, Flyway, Liquibase, sqitch, dbmate, or a
  C++-side runner?
- PostgreSQL client library for the engine: libpq, libpqxx, or another.
- Time-series strategy for `market_events`: native partitioning vs TimescaleDB.
- Numeric types: `NUMERIC` everywhere vs integer minor units.
- One `positions` child table vs `JSONB` inside `portfolio_snapshots`.

## When implementing

The concrete `PostgresRepository` (in `trading_engine_postgres_adapter`) is the
only code allowed to know PostgreSQL exists. Core code uses
`IMarketDataRepository` / `ITradeRepository` only.
