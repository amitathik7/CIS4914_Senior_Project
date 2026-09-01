# 1. Provisional architecture for the simulated trading engine

- Status: **Proposed / Provisional** (scaffold only)
- Date: 2026-08-31
- Deciders: senior project team (to ratify)

## Context

We are scaffolding an event-driven, simulated trading engine for a university
senior project. The proposal fixes the technology stack (C++, CMake, Git/GitHub,
PostgreSQL, Alpaca Market Data API, Python + Matplotlib, GoogleTest) and the
high-level pipeline, but leaves most engineering specifics open. We need a
compilable starting point that encodes the boundaries without committing to
implementations.

## Decision

The choices below are **provisional** and made to unblock scaffolding. Each may
be revisited; several are tracked in [`../OPEN_QUESTIONS.md`](../OPEN_QUESTIONS.md).

1. **Language standard: C++20.** The proposal says "C++" without a version; the
   repo had none. C++20 gives `<concepts>`, `<span>`, `<chrono>` calendar/time
   zones, designated initializers, and `operator<=>` — all useful here — while
   being well supported by current MSVC, GCC, and Clang. Set once in the root
   `CMakeLists.txt` (`CMAKE_CXX_STANDARD 20`, extensions off).

2. **Build: CMake ≥ 3.21**, one reusable **STATIC core library**
   (`trading_engine_core`) plus a **thin executable** (`trading_engine`).
   Source files listed explicitly — **no recursive globbing**.

3. **Vendor isolation via adapter targets.** `trading_engine_alpaca_adapter`
   and `trading_engine_postgres_adapter` are separate static libraries that
   link the core. Future third-party dependencies (WebSocket/HTTP/JSON,
   libpq/libpqxx) are added only to those targets. The core builds against the
   C++ standard library alone.

4. **Testing: GoogleTest**, pinned. `find_package(GTest)` if installed,
   otherwise `FetchContent` at tag `v1.15.2`. Gated by the standard
   `BUILD_TESTING` option. One test executable per component via a helper
   function, registered with `gtest_discover_tests`.

5. **Compiler warnings** enabled per family: `/W4 /permissive-` (MSVC),
   `-Wall -Wextra -Wpedantic -Wshadow -Wconversion ...` (GCC/Clang). Not
   errors by default; a `TRADING_ENGINE_WARNINGS_AS_ERRORS` option for CI.

6. **Dependency injection, no globals.** Components receive collaborators as
   constructor parameters (interface references / `shared_ptr`). `TradingEngine`
   is the sole composition root.

7. **Interfaces where behaviour is absent:** `IEventBus`, `IMarketDataSource`,
   `IStrategy`, `IRiskPolicy`, `IFillModel`, `IMarketDataRepository`,
   `ITradeRepository`, `IClock`, `IPortfolioView`.

8. **Domain model is plain value types**, free of Alpaca / PostgreSQL / JSON
   types. `MarketEvent`, `TradeSignal`, `Order`, `Fill`, `Position`,
   `PortfolioSnapshot`, `PerformanceReport`.

9. **Time via `IClock`.** `SystemClock` for live, `ManualClock` for
   deterministic backtests. Timestamps are UTC `std::chrono` `system_clock`
   time points at nanosecond resolution.

10. **Money/price/quantity are `double` aliases for now**, with a TODO to move
    to a fixed-point / integer-minor-unit type (tracked in OPEN_QUESTIONS #11).

11. **Honest placeholders.** Called-but-unimplemented code throws
    `common::NotImplemented`; nothing returns fabricated market, portfolio, or
    performance data. `main()` only confirms the build.

12. **In-process, single-node** engine. No distribution, no IPC.

13. **`#pragma once`** for header guards (supported by all three target
    compilers).

## Consequences

**Positive**

- Repo configures, builds, and tests green from day one on Windows/MSVC.
- Vendor and DB code cannot leak into the core — enforced by the target graph.
- Backtests can be made deterministic without touching component code.
- Adding a component = add sources to one `CMakeLists.txt` + one test entry.

**Negative / risks**

- `FetchContent` needs network access on first configure (mitigation: allow a
  system GTest; a future vendored/offline option).
- `double` money will need a migration later, touching domain, DB, and
  analytics.
- C++20 sets a compiler floor that CI and contributors must meet.
- `std::variant` event payload is a closed set; plugin-defined events would
  need a redesign.

## Alternatives considered

- **C++17** — safer floor, but loses `<span>`, `<concepts>`, `<=>`, and better
  `<chrono>`. Rejected; toolchains are new enough.
- **Header-only / single target** — simpler, but no seam for tests or adapters
  and slower incremental builds. Rejected.
- **Vendored GoogleTest submodule** — offline-friendly, but adds a git
  submodule and repo mutation we were asked to avoid now. Deferred.
- **One global service locator** — less ctor boilerplate, but hides
  dependencies and hurts testability. Rejected.
