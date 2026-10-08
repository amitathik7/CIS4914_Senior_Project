# Simulated Trading Engine

An event-driven, **simulated** trading engine built for a university senior
project. It is a software-engineering exercise in event-driven architecture,
concurrency, modularity, testing, latency, and throughput.

> **It does not predict markets and it never places real-money orders.**
> "Live" mode means a live market-data feed driving a simulated portfolio.

---

## Status: scaffold (M0), strategy layer started

This repository is mostly **structure, not behaviour**. It configures, builds, and
tests cleanly. Apart from the strategy layer below, every real operation is a stub that
throws `NotImplemented`. The strategy layer is implemented and tested **only against
test doubles**: no production event bus, market-data service, risk manager, execution
simulator or run loop exists yet, so nothing has run end to end. See
[`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md), and
[`docs/STRATEGIES.md`](docs/STRATEGIES.md) for the strategy documentation.

| Area | State |
|---|---|
| CMake build (core lib + adapters + thin exe) | ✅ builds |
| Component headers: responsibilities, interfaces, ownership/threading notes, TODOs | ✅ written |
| Domain value types (`MarketEvent`, `TradeSignal`, `Order`, `Fill`, `Position`, `PortfolioSnapshot`, `PerformanceReport`) | ✅ defined |
| GoogleTest wiring + smoke/contract/unit tests | ✅ passing |
| `IClock`/`ManualClock`, strong ids + generator, enum `to_string`, `default_config()` | ✅ real + tested |
| `StrategyEngine` (registration, lifecycle, routing, signal stamping, failure isolation) | ✅ real + tested against a test bus and `ManualClock` ([docs](docs/STRATEGIES.md)) |
| Reference strategies: moving-average crossover, rolling z-score mean reversion | ✅ real + tested ([crossover](docs/strategies/moving_average_crossover.md), [mean reversion](docs/strategies/mean_reversion.md)) |
| Market data, event bus, risk, execution, portfolio, persistence, analytics | ⛔ stubs (`NotImplemented`) |
| Alpaca connectivity, PostgreSQL, threading/queue, Python charts | ⛔ not started |

No external network or database connectivity is implemented anywhere in this
scaffold.

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
docs/                     ARCHITECTURE, DATA_FLOW, IMPLEMENTATION_PLAN, OPEN_QUESTIONS, STRATEGIES, strategies/, adr/
include/trading_engine/   public headers, by component
src/                      implementation (strategy layer) and stubs (the rest), by component
apps/trading_engine_main.cpp   thin executable (build confirmation only)
apps/strategy_lab/        optional strategy replay + diagnostics tool (off by default)
tests/                    unit/ (default), integration/ (opt-in), fixtures/, support/ (test doubles)
python/visualization/     plot_performance.py outline + requirements.txt
web/                      engine console (React + TypeScript); runs on a demo engine until the API exists
```

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.21 | |
| C++ compiler | C++20 | MSVC 19.3x (VS 2022), GCC ≥ 11, or Clang ≥ 14 |
| Git | any recent | GoogleTest is fetched at a pinned tag on first configure (needs network), unless a system GoogleTest is installed |
| Python | ≥ 3.10 | only for `python/visualization/` (later milestones) |
| PostgreSQL | 14+ | **not needed for the scaffold**; later milestones |
| Alpaca account | — | **not needed for the scaffold**; paper account later |

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

Run the placeholder executable (prints a build-confirmation banner and exits):

```bash
./build/bin/trading_engine
```

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

## Web console

```bash
cd web && pnpm install && pnpm dev     # http://127.0.0.1:5173
```

Runs against an in-browser demo engine (clearly labelled) until the engine
serves the API in [ADR 0006](docs/adr/0006-web-frontend-api-contract.md). See
[`web/README.md`](web/README.md).

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
