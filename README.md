# Simulated Trading Engine

An event-driven, **simulated** trading engine built for a university senior
project. It is a software-engineering exercise in event-driven architecture,
concurrency, modularity, testing, latency, and throughput.

> **It does not predict markets and it never places real-money orders.**
> "Live" mode means a live market-data feed driving a simulated portfolio.

---

## Status (2026-10-30, week 9 of 15)

The engine runs a full **simulated backtest end to end**: CSV bars ->
`MarketDataService` -> event bus (in-process or Kafka) -> `StrategyEngine` (three
strategies) -> `RiskManager` -> `ExecutionSimulator` -> `PortfolioManager` ->
`PerformanceAnalyzer`. `trading_engine run` does this once from the command line
([`docs/EVENT_BUS.md`](docs/EVENT_BUS.md)); `trading_engine serve` runs backtests that
the web console starts, stops and kill-switches over Kafka
([ADR 0007](docs/adr/0007-engine-command-channel.md)); `app::Backtest` and the
`strategy_lab_backtest` tool run the same stages in one process
([`docs/BACKTESTING.md`](docs/BACKTESTING.md)). CI builds and tests everything on
Linux and Windows on every push and pull request.

Market data comes only from recorded CSV files. Alpaca (historical and live),
PostgreSQL persistence, config-file loading, `SystemMetrics`, the source-driven
`HistoricalReplay` and the Python charts are still stubs that throw
`NotImplemented`. See [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md)
for the milestone-by-milestone status.

| Area | State |
|---|---|
| CMake build (core lib + adapters + executables), presets | ✅ builds on Linux/GCC and Windows/MSVC |
| Domain value types, `common::Decimal` fixed-point money, `IClock`/`ManualClock`, ids | ✅ real + tested |
| Event bus: `InProcessEventBus` (bounded queue, dispatcher thread) and `TopicEventBus` over Kafka | ✅ real + tested, incl. against librdkafka's mock cluster ([docs](docs/EVENT_BUS.md), [ADR 0003](docs/adr/0003-kafka-transport.md)) |
| `MarketDataService` (validation, `ingest_time`, per-symbol sequence) fed from CSV bars | ✅ real + tested; no Alpaca or database source yet |
| `StrategyEngine` + three strategies: moving-average crossover, mean reversion, online logistic regression (ML) | ✅ real + tested ([docs](docs/STRATEGIES.md)) |
| `RiskManager` + 5 starter policies, buying power, no short selling, kill switch | ✅ real + tested |
| `NaiveFillModel` + `ExecutionSimulator` (fills at the next bar's open, slippage, fees) | ✅ real + tested; latency, participation cap and partial fills are not modelled yet |
| `PortfolioManager` (+ `PortfolioBusBridge`, reservation ledger) | ✅ real + tested |
| `PerformanceAnalyzer` (return, volatility, drawdown, Sharpe, profit factor, trades) | ✅ real + tested |
| `trading_engine run` / `serve`, `app::Backtest`, `app::OrderFlow` | ✅ real + tested |
| Engine gateway (console API from the Kafka topics) and command channel (New backtest, Stop, kill switch) | ✅ real + tested ([docs](docs/ENGINE_GATEWAY.md)) |
| Web console on the real engine; Strategy Lab (replay, compare, train/sweep, backtest) | ✅ real + tested ([web](web/README.md), [lab](docs/STRATEGY_LAB.md)) |
| CI (`.github/workflows/ci.yml`): C++ Linux + Windows, web, Python gateways | ✅ green on GitHub Actions |
| `HistoricalReplay` (source-driven), `TradingEngine::run_backtest/run_live`, `config::load_config`, `SystemMetrics` | ⛔ stubs (`NotImplemented`) |
| Alpaca historical + live sources, `PostgresRepository` + migrations, `python/visualization/plot_performance.py` | ⛔ stubs, not started |
| Benchmarks, `docs/PERFORMANCE.md`, `docs/TECH_REQUIREMENTS.md`, coverage gate, sanitizer CI jobs | ⛔ not started |

The only network code is the optional Kafka adapter (librdkafka). There is no
Alpaca or PostgreSQL connectivity yet.

## Architecture at a glance

```
Alpaca live feed  ─┐
                   ├─► Market Data Service ─► Event Bus / Queue ─► Strategy Engine
Historical replay ─┘                                                   │
                                                                       ▼
   Performance Analytics ◄─ Portfolio Manager + Persistence ◄─ Execution Simulator ◄─ Risk Manager
                                     ▲                                                   │
                                     └───────────  current portfolio state  ────────────┘
```

Details: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) ·
[`docs/DATA_FLOW.md`](docs/DATA_FLOW.md).

Key structural rules: dependency injection over globals; interfaces where
behaviour is absent; Alpaca and PostgreSQL confined to adapter libraries the
core never links; stubs throw rather than fake data.

## Repository layout

```
CMakeLists.txt            root build
CMakePresets.json         convenience configure/build/test presets
cmake/                    CompilerWarnings, Dependencies (GoogleTest), version.hpp.in
config/                   config.example.json (no secrets) + notes
database/migrations/      001_initial_schema.sql  (commented OUTLINE, no-op)
docker-compose.kafka.yml  a single-node Kafka broker for local runs
docs/                     ARCHITECTURE, DATA_FLOW, IMPLEMENTATION_PLAN, EVENT_BUS, BACKTESTING, ENGINE_GATEWAY,
                          STRATEGIES, STRATEGY_LAB*, OPEN_QUESTIONS, strategies/, adr/
include/trading_engine/   public headers, by component
src/                      implementation by component (a few remaining stubs listed above)
apps/trading_engine_main.cpp   the trading_engine executable (`run`, `serve`)
apps/strategy_lab/        optional strategy replay + diagnostics tool (off by default)
tests/                    unit/ (default), integration/ (opt-in), fixtures/, support/ (test doubles)
python/engine_gateway/    serves the console's API from the Kafka topics
python/strategy_lab/      Strategy Lab gateway and Streamlit UI
python/visualization/     plot_performance.py outline + requirements.txt
web/                      engine console (React + TypeScript); real engine via the gateway, or an in-browser demo engine
```

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.21 | |
| C++ compiler | C++20 | MSVC 19.3x (VS 2022), GCC ≥ 11, or Clang ≥ 14 |
| Git | any recent | GoogleTest is fetched at a pinned tag on first configure (needs network), unless a system GoogleTest is installed |
| Python | ≥ 3.10 | the engine gateway and Strategy Lab (`python/`) |
| Node.js + pnpm | Node 22, pnpm 10 | the web console (`web/`) |
| librdkafka + a Kafka broker | librdkafka 2.x; Kafka 3.7+ / 4.x | optional: `-DTRADING_ENGINE_WITH_KAFKA=ON` (vcpkg `librdkafka:x64-windows` on Windows); broker via `docker-compose.kafka.yml` |
| PostgreSQL | 14+ | **not needed yet**: nothing connects to it (persistence is a later milestone) |
| Alpaca account | — | **not needed yet**: market data comes from recorded CSV files; a paper account later |

## Build

```bash
cmake -S . -B build
cmake --build build
```

With a preset:

```bash
cmake --preset default
cmake --build --preset default
```

On Windows with Visual Studio (multi-config):

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

Useful options: `-DBUILD_TESTING=OFF`, `-DTRADING_ENGINE_BUILD_APPS=OFF`,
`-DTRADING_ENGINE_WARNINGS_AS_ERRORS=ON`,
`-DTRADING_ENGINE_BUILD_INTEGRATION_TESTS=ON`,
`-DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON` (the optional strategy replay tool, off by default; needs no
GoogleTest or Python itself -- see [`docs/STRATEGY_LAB.md`](docs/STRATEGY_LAB.md)).

Run a backtest from the command line (prints build info and usage with no arguments):

```bash
./build/bin/trading_engine run --bars tests/fixtures/pipeline/demo_bars.csv
```

`-DTRADING_ENGINE_WITH_KAFKA=ON` adds the Kafka bus (`--bus kafka`, `--bus kafka-mock`)
and `trading_engine serve`, which the web console drives.

## Test

```bash
ctest --test-dir build --output-on-failure
# multi-config (VS): ctest --test-dir build -C Debug --output-on-failure
```

Filter by component label (`common`, `domain`, `strategy`, `reference_strategies`, ...):

```bash
ctest --test-dir build -L common
ctest --test-dir build -L reference_strategies   # both strategies, alone and in the engine
```

### Continuous integration

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push to
`main`/`jordan` and every pull request: the C++ build and full `ctest` on
Linux/GCC (with the Kafka adapter, against librdkafka's mock cluster, and the
Strategy Lab tools) and on Windows/MSVC, the web console's typecheck, tests and
build, and the Python gateway suites. The web and Python jobs run their
real-engine tests against the Linux job's `strategy_lab_replay`.

## Web console

```bash
cd web && pnpm install && pnpm dev     # http://127.0.0.1:5173, in-browser demo engine
```

On the real engine (Kafka build; full steps in [`docs/ENGINE_GATEWAY.md`](docs/ENGINE_GATEWAY.md)):

```bash
docker compose -f docker-compose.kafka.yml up -d
python python/engine_gateway/serve.py
./build/bin/trading_engine serve --bars tests/fixtures/pipeline/demo_bars.csv --pace-ms 50
cd web && VITE_ENGINE_API=/api pnpm dev
```

The Strategies section also needs the Strategy Lab gateway (`cd web && pnpm lab`).
See [`web/README.md`](web/README.md).

## Python visualization (syntax check only for now)

```bash
python -m py_compile python/visualization/plot_performance.py
```

## Contributing (team)

- Pick up a milestone from [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md).
- Resolve the relevant item in [`docs/OPEN_QUESTIONS.md`](docs/OPEN_QUESTIONS.md)
  first and record it as an ADR in [`docs/adr/`](docs/adr/).
- Replace a stub together with its tests; add a
  `trading_engine_add_component_test(...)` entry.
- Keep vendor code inside its adapter target.
- Don't commit secrets or local config; both are gitignored.

## License

No license has been chosen yet — see
[`docs/OPEN_QUESTIONS.md`](docs/OPEN_QUESTIONS.md) (#14).
