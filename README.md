# Simulated Trading Engine

An event-driven, **simulated** trading engine built for a university senior
project. It is a software-engineering exercise in event-driven architecture,
concurrency, modularity, testing, latency, and throughput.

> **It does not predict markets and it never places real-money orders.**
> "Live" mode means a live market-data feed driving a simulated portfolio.

---

## Quick start: `python start.py`

One command starts the whole system and opens the web console in your browser:

```bash
python start.py
```

It runs, in order, and shows each process's output with a name prefix:

1. **kafka**: a single-node Kafka broker in Docker (`docker compose -f docker-compose.kafka.yml up -d`;
   on Windows it starts Docker Desktop first if it isn't running).
2. **gateway**: the engine gateway (`python/engine_gateway/serve.py`), which serves the console's API from
   the Kafka topics, on port 8080, or on 18080 / 8081 / 18081 when 8080 is taken (for example by Apache).
3. **engine**: `trading_engine serve --bars tests/fixtures/pipeline/demo_bars.csv --pace-ms 50`, which waits
   for the console to start backtests.
4. **lab**: the Strategy Lab gateway (`python/strategy_lab/lab_gateway.py`, port 8765) for the Strategies pages.
5. **console**: `pnpm dev` in `web/` with `VITE_ENGINE_API=/api`, pointed at the engine gateway.

When it prints `Everything is up`, open **Backtests > New backtest** at http://127.0.0.1:5173 (it opens by
itself). `start.py` waits for the engine's first heartbeat before saying so, so the console can start a backtest
straight away. **Ctrl+C** stops every process and stops (does not
delete) the Kafka container, so earlier runs are still listed next time. Each process also logs to
`out/logs/<name>.log`.

### Before the first run

You need Python 3.10+, Node.js 22 with pnpm 10 or newer, Docker, and a **Kafka build** of `trading_engine`.
`start.py` installs the rest on first start (`confluent-kafka` for the gateway, `pnpm install` in `web/`).
Check everything without starting anything:

```bash
python start.py --check
```

Build `trading_engine` with Kafka once, into `build-kafka/`. `start.py` uses the most recently built
`trading_engine` in any `build*/` folder **that was configured with `-DTRADING_ENGINE_WITH_KAFKA=ON`** (it reads
each folder's `CMakeCache.txt`), because `trading_engine serve` exits at once with "serve needs Kafka" in any other
build. An ordinary `build/` next to it is fine; if it finds only builds without Kafka, it says so and prints the
command below.

```bash
# Windows (Visual Studio 2022 + vcpkg: vcpkg install librdkafka:x64-windows)
cmake -S . -B build-kafka -G "Visual Studio 17 2022" -A x64 -DTRADING_ENGINE_WITH_KAFKA=ON -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-kafka --config Release --target trading_engine

# Linux / macOS (librdkafka from apt: librdkafka-dev, or Homebrew: librdkafka)
cmake -S . -B build-kafka -DCMAKE_BUILD_TYPE=Release -DTRADING_ENGINE_WITH_KAFKA=ON
cmake --build build-kafka --target trading_engine
```

After that, `python start.py --build` rebuilds `trading_engine` before starting.

The **Strategies** pages (Explore, Compare, Backtest, ...) also need the Strategy Lab tools, built once into
`out/strategy_lab/` (the Backtests pages work without them):

```bash
cmake -S . -B out/strategy_lab -DBUILD_TESTING=OFF -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON   # Windows: add -G "Visual Studio 17 2022" -A x64
cmake --build out/strategy_lab --config Release --target strategy_lab_replay strategy_lab_backtest
```

### Options

| Flag | What it does |
|---|---|
| `--pace-ms N` | pause N ms after each bar time so a run can be watched, stopped or kill-switched (default 50; e.g. `--pace-ms 200` for a slower replay, `0` for full speed) |
| `--build` | run `cmake --build build-kafka --target trading_engine` first (Release unless `--config` says otherwise) |
| `--reset-kafka` | delete the broker's topics, i.e. all earlier runs, before starting (`docker compose down`, then `up`) |
| `--check` | check prerequisites (engine build, pnpm, free ports, Docker) and exit without starting anything |
| `--bars FILE` | a CSV of bars the engine can replay; repeatable (default `tests/fixtures/pipeline/demo_bars.csv`) |
| `--config Debug\|Release\|RelWithDebInfo` | which `trading_engine` build to use (default: the most recently built) |
| `--engine PATH` | use this `trading_engine` instead of searching `build-kafka/` and `build/` |
| `--gateway-port N` | engine gateway port (default 8080, or the first free one of 18080, 8081, 18081) |
| `--no-kafka` | don't touch Docker; use a broker already on `localhost:9092` |
| `--keep-kafka` | leave the Kafka container running on exit |
| `--no-lab` | don't start the Strategy Lab gateway |
| `--no-browser` | don't open the console in a browser |

`python start.py --help` lists the same options. To run the pieces by hand instead, see
[Web console](#web-console) below.

**New to the console?** [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md) is a walkthrough for first-time users and
user testing: a tour of the pages, tasks to try with what you should see, known limitations, and how to
report problems.

## Status (2026-11-11, week 11 of 15)

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

Market data comes from CSV files, which can be recorded fixtures or real Alpaca bars
downloaded with `trading_engine fetch-alpaca` ([`docs/ALPACA.md`](docs/ALPACA.md)).
`trading_engine run --alpaca-live SYMBOLS` trades Alpaca's live minute bars
through the same pipeline (simulated fills only), and `run --config FILE` reads
risk and simulation settings from a JSON file ([`config/README.md`](config/README.md)).
The console can start live runs too (`serve` trades the live feed for them).
`run --report FILE` writes the run's results as JSON, which
`python/visualization/plot_performance.py` charts; `run` and `serve` print
per-stage `SystemMetrics`. PostgreSQL persistence is still a stub. See [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md)
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
| Engine gateway (console API from the Kafka topics) and command channel (New backtest, New live run, Stop, kill switch) | ✅ real + tested ([docs](docs/ENGINE_GATEWAY.md)) |
| Web console on the real engine; Strategy Lab (replay, compare, train/sweep, backtest; 17 built-in datasets incl. five large synthetic ones) | ✅ real + tested ([web](web/README.md), [lab](docs/STRATEGY_LAB.md)) |
| `start.py`: Kafka, engine, both gateways and the console in one command | ✅ works on Windows and Linux |
| CI (`.github/workflows/ci.yml`): C++ Linux + Windows, web, Python gateways | ✅ green on GitHub Actions |
| `SystemMetrics` (per-stage counts, fixed-size latency histogram with p50/p99, queue depth) | ✅ real + tested; every stage timed in `run` and `serve` |
| `trading_engine run --report` results JSON + `python/visualization/plot_performance.py` charts | ✅ real + tested ([docs](python/visualization/README.md)) |
| `config::load_config` + `trading_engine run --config FILE` | ✅ real + tested ([docs](config/README.md)) |
| `AlpacaHistoricalSource` (REST bars, paging, retries) + `trading_engine fetch-alpaca` | ✅ real + tested on recorded responses ([docs](docs/ALPACA.md)); needs libcurl |
| `AlpacaLiveSource` (WebSocket stream, reconnect) + `trading_engine run --alpaca-live` | ✅ real + tested on a scripted socket; checked against Alpaca's test stream ([docs](docs/ALPACA.md)) |
| `PostgresRepository` + migrations | ⛔ stubs, not started |
| `trading_engine_bench` + baseline | ✅ measured ([docs](docs/PERFORMANCE.md)) |
| Performance targets and optimisations, `docs/TECH_REQUIREMENTS.md`, coverage gate, sanitizer CI jobs | ⛔ not started |

Network code lives in two optional adapters: Kafka (librdkafka) and Alpaca
(libcurl). There is no PostgreSQL connectivity yet.

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
start.py                  one-command launcher: Kafka, engine, gateways and web console
cmake/                    CompilerWarnings, Dependencies (GoogleTest), version.hpp.in
config/                   config.example.json (no secrets) + notes
database/migrations/      001_initial_schema.sql  (commented OUTLINE, no-op)
docker-compose.kafka.yml  a single-node Kafka broker for local runs
docs/                     USER_GUIDE, ARCHITECTURE, DATA_FLOW, IMPLEMENTATION_PLAN, EVENT_BUS, BACKTESTING, ENGINE_GATEWAY,
                          STRATEGIES, STRATEGY_LAB*, OPEN_QUESTIONS, strategies/, adr/
include/trading_engine/   public headers, by component
src/                      implementation by component (a few remaining stubs listed above)
apps/trading_engine_main.cpp   the trading_engine executable (`run`, `serve`)
apps/strategy_lab/        optional strategy replay + diagnostics tool (off by default)
tests/                    unit/ (default), integration/ (opt-in), fixtures/, support/ (test doubles)
python/engine_gateway/    serves the console's API from the Kafka topics
python/strategy_lab/      Strategy Lab gateway and Streamlit UI
python/visualization/     plot_performance.py: charts a run's --report file
web/                      engine console (React + TypeScript); real engine via the gateway, or an in-browser demo engine
build-kafka/, out/        local build and run output (gitignored): Kafka build, Strategy Lab tools, out/logs/
```

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.21 | |
| C++ compiler | C++20 | MSVC 19.3x (VS 2022), GCC ≥ 11, or Clang ≥ 14 |
| Git | any recent | GoogleTest is fetched at a pinned tag on first configure (needs network), unless a system GoogleTest is installed |
| Python | ≥ 3.10 | the engine gateway and Strategy Lab (`python/`) |
| Node.js + pnpm | Node 22, pnpm 10 | the web console (`web/`) |
| librdkafka | 2.x | for the Kafka build `start.py` runs (`-DTRADING_ENGINE_WITH_KAFKA=ON`); vcpkg `librdkafka:x64-windows` on Windows |
| Docker (Desktop on Windows/macOS) | any recent | runs the Kafka broker (`docker-compose.kafka.yml`, Kafka 3.7); or bring your own broker on `localhost:9092` with `start.py --no-kafka` |
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
and `trading_engine serve`, which the web console drives (build it into `build-kafka/` for
[`start.py`](#before-the-first-run)).

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

The usual way to run the console on the real engine is [`python start.py`](#quick-start-python-startpy).
Without Kafka or an engine build, the console runs on an in-browser demo engine:

```bash
cd web && pnpm install && pnpm dev     # http://127.0.0.1:5173, in-browser demo engine
```

What `start.py` does, step by step by hand (one terminal each; full detail in
[`docs/ENGINE_GATEWAY.md`](docs/ENGINE_GATEWAY.md)):

```bash
docker compose -f docker-compose.kafka.yml up -d
python python/engine_gateway/serve.py                      # --port N if 8080 is taken
./build-kafka/bin/trading_engine serve --bars tests/fixtures/pipeline/demo_bars.csv --pace-ms 50
python python/strategy_lab/lab_gateway.py                  # or: cd web && pnpm lab
cd web && VITE_ENGINE_API=/api pnpm dev                    # ENGINE_GATEWAY=http://127.0.0.1:N for another port
```

On Windows the engine is `.\build-kafka\bin\Release\trading_engine.exe`, and in PowerShell the console line is
`$env:VITE_ENGINE_API="/api"; pnpm dev`. See [`web/README.md`](web/README.md).

## Charts

```bash
pip install -r python/visualization/requirements.txt
./build/bin/trading_engine run --bars tests/fixtures/pipeline/demo_bars.csv --report run.json
python python/visualization/plot_performance.py --input run.json --output charts/
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
