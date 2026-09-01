# Architecture

> Status: **scaffold**. This document describes the intended shape of the
> system and the boundaries the code already encodes. No component has behaviour
> yet.

## 1. Goal and non-goals

**Goal.** An event-driven, modular, well-tested *simulated* trading engine that
ingests market data, runs pluggable strategies, enforces risk limits, simulates
execution, tracks a portfolio, persists everything, and reports performance --
with explicit attention to concurrency, latency, and throughput.

**Non-goals.** It does not predict markets and it never places real-money
orders. "Live" means a live *data* feed driving a *simulated* portfolio.

## 2. Pipeline

```
        ┌────────────────────┐     ┌────────────────────────┐
        │  Alpaca live feed  │     │  Alpaca historical /   │
        │  (WebSocket)       │     │  market-data repo      │
        └─────────┬──────────┘     └───────────┬────────────┘
                  │  raw ticks                 │  raw bars/trades
                  ▼                            ▼
             ┌──────────────────────────────────────┐
             │        Market Data Service           │  validate, normalise,
             │  (IMarketEventSink)                  │  stamp, sequence
             └───────────────┬──────────────────────┘
                             │ domain::MarketEvent
                             ▼
             ┌──────────────────────────────────────┐
             │        Event Bus / Data Queue        │  bounded, thread-safe,
             │  (IEventBus)                         │  backpressure (TBD)
             └───────────────┬──────────────────────┘
                MarketData    │   Signal      Order       Fill
              ┌───────────────┼───────────────┬───────────┬─────────────┐
              ▼               ▼               ▼           ▼             │
      ┌───────────────┐ ┌───────────┐ ┌──────────────┐ ┌────────────┐  │
      │ StrategyEngine│ │RiskManager│ │ExecutionSim  │ │ Portfolio  │  │
      │  + IStrategy  │ │ +IRiskPol.│ │ + IFillModel │ │  Manager   │  │
      └──────┬────────┘ └─────┬─────┘ └──────┬───────┘ └─────┬──────┘  │
             │ TradeSignal    │ approved     │ Order/Fill     │ snapshot│
             └────────────────┘ signal       └───────────────►│         │
                               ▲                               │        │
                               │   PortfolioSnapshot (read)    │        │
                               └───────────────────────────────┘        │
                                                                        ▼
                          ┌───────────────────────┐        ┌────────────────────┐
                          │  Persistence           │◄───────┤  everything above  │
                          │  IMarketDataRepository │        └────────────────────┘
                          │  ITradeRepository      │
                          └───────────┬────────────┘
                                      ▼
                          ┌───────────────────────┐        ┌────────────────────┐
                          │ PerformanceAnalyzer   │───────►│ export file        │
                          │ SystemMetrics         │        │ → Python/Matplotlib│
                          └───────────────────────┘        └────────────────────┘
```

The **portfolio → risk** edge (RiskManager reads the current
`PortfolioSnapshot`) is a required feedback path and is already expressed in
code: `RiskManager` holds a `const portfolio::IPortfolioView&`.

## 3. Components and boundaries

| Component | Responsibility | Boundary / what it must NOT know |
|---|---|---|
| **common** (`types`, `identifiers`, `clock`, `errors`) | Shared vocabulary: UTC time, money/price/quantity aliases, strong ids, `IClock`, exception types. | No component logic. Standard library only. |
| **domain** (`MarketEvent`, `TradeSignal`, `Order`, `Fill`, `PortfolioSnapshot`, `PerformanceReport`) | Plain value types passed between stages. | No Alpaca / PostgreSQL / JSON types. No behaviour that computes results. |
| **MarketDataSource** (`IMarketDataSource`) | Common interface for live and historical feeds; pushes raw `MarketEvent`s to a sink. | Doesn't know the bus, strategies, or storage. |
| **AlpacaLiveSource** | *(future)* WebSocket adapter to Alpaca streaming. | Lives in `trading_engine_alpaca_adapter`; the only place a WebSocket/JSON lib may appear. |
| **AlpacaHistoricalSource** | *(future)* REST adapter for historical bars/trades, emitted in time order. | Same adapter target; no HTTP client in core. |
| **MarketDataService** | Validate + normalise + stamp + sequence raw data into canonical `MarketEvent`s; tap for raw-data persistence; publish to the bus. | Doesn't parse vendor payloads (sources do) and doesn't run strategies. |
| **EventBus** (`IEventBus`) | Bounded, thread-safe delivery of typed events between stages; lifecycle (start / shutdown / drain); backpressure. | Doesn't interpret payloads. |
| **HistoricalReplay** | Drive the pipeline from a historical source in timestamp order, advancing a `ManualClock`; deterministic at `replay_speed = 0`. | Backtest-only; not used in live mode. |
| **Strategy** (`IStrategy`) | Pure decision logic: market events in, `TradeSignal`s out. | No sizing against the portfolio, no risk checks, no I/O, no blocking. |
| **StrategyEngine** | Own registered strategies, route events, stamp and publish emitted signals. | Doesn't judge signals -- risk does. |
| **RiskPolicy** (`IRiskPolicy`) | One composable rule: pure function of (signal, portfolio snapshot, context). | Stateless, side-effect free, thread-safe. |
| **RiskManager** | Read current portfolio state, run all policies, approve / resize / reject. | Doesn't place orders; forwards approved signals. |
| **FillModel** (`IFillModel`) | Encapsulate execution assumptions: latency, slippage, fees, partial fills. | Pure function; no global state. |
| **ExecutionSimulator** | Turn approved signals/orders into simulated `Fill`s via the `IFillModel`; maintain per-symbol market context; publish orders and fills. | Never contacts a real venue. |
| **PortfolioManager** (`IPortfolioView`) | Single source of truth for cash, positions, realised/unrealised P&L; expose immutable snapshots. | Writers and readers are different threads -- reads must be safe. |
| **Repositories** (`IMarketDataRepository`, `ITradeRepository`) | Storage boundary for market data and trading artefacts. | No SQL / driver / connection types in the interfaces. |
| **PostgresRepository** | *(future)* the interfaces implemented on PostgreSQL. | Lives in `trading_engine_postgres_adapter`; the only code that knows PostgreSQL exists. |
| **PerformanceAnalyzer** | Compute run metrics (return, volatility, drawdown, profit factor, trade count, ...) from recorded state. | Pure computation; no I/O. |
| **SystemMetrics** | Engineering telemetry: per-stage latency, queue depth, throughput, error counts. | Measures the engine, not the strategy. |
| **TradingEngine** | Composition root: own every component, wire via DI, coordinate ordered start-up / shutdown, run live or backtest mode. | The only place components are constructed and connected. |

## 4. Cross-cutting rules the scaffold enforces

- **Dependency injection, not globals.** Every collaborator arrives through a
  constructor parameter, as an interface reference or `shared_ptr`. There are no
  singletons.
- **Interfaces over implementations.** Where behaviour is intentionally absent
  there is an abstract interface (`IEventBus`, `IStrategy`, `IRiskPolicy`,
  `IFillModel`, `IMarketDataSource`, repositories, `IClock`, `IPortfolioView`).
- **Vendor isolation.** Alpaca and PostgreSQL code is confined to dedicated
  adapter libraries that the core never links. The core builds with the C++
  standard library alone.
- **Honest placeholders.** A stub that is called throws
  `common::NotImplemented`. Nothing returns fabricated market, portfolio, or
  performance data.
- **Determinism hook.** `IClock` + `ManualClock` let backtests run without any
  dependence on the wall clock.

## 5. Concurrency model (planned, not built)

- One `EventBus` with bounded queue(s); producers (sources) and consumers
  (stages) on separate threads.
- `PortfolioManager` written from the execution/bus thread, read from the risk
  thread → needs a lock-free snapshot or a mutex-guarded copy.
- `SystemMetrics` written from every stage → lock-free counters / per-thread
  accumulation.
- Strategy callbacks are single-threaded per strategy so `IStrategy`
  implementations need no internal locking.

The exact queue type, thread count, backpressure policy, and shutdown/drain
semantics are **open** -- see [`OPEN_QUESTIONS.md`](OPEN_QUESTIONS.md).

## 6. Build topology

```
trading_engine_core            (STATIC)  domain + interfaces + services; stdlib only
 ├─ trading_engine_alpaca_adapter   (STATIC, links core)  future WS/HTTP/JSON deps
 └─ trading_engine_postgres_adapter (STATIC, links core)  future libpq/libpqxx dep

trading_engine                 (EXE)     thin; links core + both adapters
te_test_*                      (EXE)     one GoogleTest binary per component
```
