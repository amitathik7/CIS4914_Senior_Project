# Strategy Lab

> Status (2026-10-02): the **C++ replay tool, its per-bar diagnostics, fixtures and tests are
> implemented**, and so is the browser UI (Streamlit/Plotly): **Explore** (section 14), and **Compare**, **Validate** and a **dark theme**
> ([STRATEGY_LAB_COMPARE_VALIDATE.md](STRATEGY_LAB_COMPARE_VALIDATE.md); a 5-minute walkthrough is in [STRATEGY_LAB_DEMO.md](STRATEGY_LAB_DEMO.md)).
> The Engine console's native **Strategies** section (React, `web/`) reuses the replay tool through a small gateway: [STRATEGIES_CONSOLE.md](STRATEGIES_CONSOLE.md).
> The tool is optional and OFF by default. It replays recorded
> bars through the real `StrategyEngine` and the reference strategies over a **lab-local synchronous
> bus**, and reports signals and diagnostics. It is not the production queue, runs no risk, execution
> or portfolio stage, and shows no fills, P&L, confidence or ML. Strategy defaults are the strategies'
> local placeholders ([STRATEGIES.md](STRATEGIES.md)), **not tuned or recommended trading parameters**.

## 1. What it is, and is not

| It is | It is not |
|---|---|
| `strategy_lab_replay`: CSV bars + strategy parameters in, one versioned JSON document out | A trading pipeline: nothing downstream of `TradeSignal` exists, so signals are "Buy request" / "Sell request" |
| The real `StrategyEngine`, `MovingAverageCrossoverStrategy`, `MeanReversionStrategy`, `ManualClock` | A replica: no strategy logic is written outside `src/strategy/` (the only Python "model" is a test oracle, section 12) |
| Deterministic: same inputs, same bytes (provenance aside) | The production `InProcessEventBus`, `HistoricalReplay` or `MarketDataService`; they are stubs that throw |
| The C++ tool is free of GoogleTest, Python, a JSON library and any UI library; the UI is a separate, optional Python layer over its JSON | The team's performance dashboard (`python/visualization`, an outline; nothing is shared) |

## 2. Observed state it was built against

Branch `jordan`, one unpushed local commit ahead (`origin/main` and `origin/jordan` lack the engine and strategies).
`StrategyEngine` and both strategies are real; the event bus, `HistoricalReplay`, `MarketDataService`, `RiskManager`,
`ExecutionSimulator`, `PortfolioManager` and `TradingEngine` throw `NotImplemented`. No dashboard exists on any local
ref. The production bus being a stub is why the lab has its own bus (section 5). The C++ tool needs neither Streamlit nor
Plotly; the Explore UI installs them into its own virtual environment (section 14).

## 3. Files

| Path | Role |
|---|---|
| `apps/strategy_lab/` | The tool (`lab/*.{hpp,cpp}`, `main.cpp`, `CMakeLists.txt`). Target `trading_engine_strategy_lab` (static) and `strategy_lab_replay` (exe); links only `trading_engine::core` |
| `include/trading_engine/strategy/strategy_diagnostics.hpp` | NEW public header: the diagnostics observer (section 11) |
| `src/strategy/*_strategy.cpp`, the two strategy headers, `strategy.cpp` | Additive observer hook; no decision code changed |
| `tests/fixtures/strategy_lab/datasets/*.csv` | 13 small fixtures (section 12) |
| `tests/unit/strategy_lab_*_test.cpp`, `strategy_diagnostics_*` | GoogleTest suites |
| `tests/strategy_lab/` | Python `unittest` process tests, the exact reference model, `json_probe.cpp` |
| `apps/CMakeLists.txt`, `tests/unit/CMakeLists.txt` | The option and the test registration (section 15) |
| `python/strategy_lab/` | The UI: `app.py` (a router over Explore, Compare, Validate), the `strategy_lab_ui/` package, `tests/`, `setup.ps1`, `run_lab.ps1`, `requirements.txt`, `.streamlit/config.toml` (dark theme), a scoped `.gitignore`. No C++ and no CMake file was changed for it |
| `tests/fixtures/strategy_lab/scenarios/`, `invalid/` | The 18 hand-derived validation scenarios and 3 small invalid datasets used by the Validate view ([STRATEGY_LAB_COMPARE_VALIDATE.md](STRATEGY_LAB_COMPARE_VALIDATE.md) section 4) |

Source layout under `apps/strategy_lab/lab/`: `cli` (arguments to documents), `dataset` (CSV), `catalog` (strategy
kinds, parameters, defaults), `session` and `runner` (the replay), `demo_bus`, `collector` (the observer),
`result_json` and `json_writer` (output), plus `timestamp`, `sha256`, `utf8`, `numbers`, `paths`, `io`, `errors`.

## 4. Build and run (verified on Windows 11, MSVC 19.39, Visual Studio 17 2022)

The lab needs no network, GoogleTest or Python. `out/` is git-ignored.

```powershell
cmake -S . -B out/strategy_lab -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON
cmake --build out/strategy_lab --config Release --target strategy_lab_replay
out\strategy_lab\bin\Release\strategy_lab_replay.exe describe --pretty
```

Reproduce ONE fixture run (the documented moving-average crossover: Buy at bar 5, Sell at bar 8):

```powershell
out\strategy_lab\bin\Release\strategy_lab_replay.exe run --dataset tests\fixtures\strategy_lab\datasets\sma_crossover.csv `
    --strategy sma_crossover --param short_window=2 --param long_window=3 --param symbols=AAPL --pretty --no-wall-clock
```

`--no-wall-clock` omits `provenance.generated_at`, so the output is byte-reproducible. The signals show `buy` at
`event_index` 4 (`2026-01-05T14:34:00Z`) and `sell` at 7, with `short_sma` 2.5 / `long_sma` 2 and 2.5 / 3.

**Configuration selection (Visual Studio is multi-config).** Pick the configuration when building
(`--config Debug|Release`) and when testing (`ctest -C Debug|Release`); the executable is
`<build dir>\bin\<Config>\strategy_lab_replay.exe`. With the tests (needs a GoogleTest source tree; the flags below make
the configure offline, `<googletest-src>` being e.g. `build\_deps\googletest-src`):

```powershell
cmake -S . -B out/strategy_lab_tests -G "Visual Studio 17 2022" -A x64 -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON `
    -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<googletest-src> -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build out/strategy_lab_tests --config Debug
ctest --test-dir out/strategy_lab_tests -C Debug -L strategy_lab --output-on-failure     # 131 GoogleTest cases + the Python suite
ctest --test-dir out/strategy_lab_tests -C Debug -L reference_strategies                 # includes the 21 diagnostics tests
```

Always check the build's exit status before running tests (`cmake --build ...; if ($LASTEXITCODE) { stop }`): a stale
binary would otherwise be reported as a pass. The `strategy_lab_python` test is registered only if CMake finds a Python 3
interpreter (stdlib only, no pip installs); otherwise configure prints a status line saying so. It fails, rather than
skips, if any of its tests cannot run. `-DTRADING_ENGINE_WARNINGS_AS_ERRORS=ON` builds clean. The lab option lives in
`apps/CMakeLists.txt`, so it needs `TRADING_ENGINE_BUILD_APPS=ON` (the default). POSIX commands are the same without
`--config` and `.exe`; they were **not run** here.

## 5. The bus and the clock

`SynchronousDemoBus` (`lab/demo_bus.hpp`) satisfies the engine's documented bus assumptions
([STRATEGIES.md](STRATEGIES.md) section 4) and says in its header what it is not.

| Requirement | How |
|---|---|
| Lifecycle owned by the composition root | The runner calls `start()`, then after the replay `request_shutdown()` and `wait_until_drained()`, then stops the engine. `publish()` before start or after shutdown returns `false` |
| `unsubscribe()` is a barrier | Single-threaded and synchronous; a handler unsubscribed during a delivery is skipped for the rest of it |
| Serialized delivery | Remembers the first thread that uses it and throws `std::logic_error` if another does |
| A `subscribe()` that throws leaves nothing | An empty handler is refused before anything is stored |
| A handler may publish | The engine's `Signal` publication runs nested inside its `MarketData` handler, depth first (capped at 32 levels) |
| Stamps | `Event::sequence` 1, 2, 3... across all event types; `enqueued_at` from the injected clock |
| Optional faults | `--bus-fault reject-signals` (`publish` returns false) or `throw-on-signal`: only `Signal` events, never market data |

**Clock.** A `ManualClock` is set to each event's `exchange_time` **before** the event is published, so a signal's
`created_at` is the triggering bar's time (the engine stamps the clock, not the bar). No wall clock is read in `lab/`
except the one provenance read in `main.cpp`. A signal the bus refuses is lost, never re-sent (state is committed first);
the id it consumed is skipped and listed under `publication_failures`.

## 6. Command line and exit status

```
strategy_lab_replay describe [--pretty]
strategy_lab_replay run --dataset <csv> --strategy <kind> [--param name=value]... [--strategy <kind> [--param ...]]...
                        [--bus-fault none|reject-signals|throw-on-signal] [--max-rows N] [--pretty] [--no-wall-clock]
strategy_lab_replay --help        (usage on stderr; stdout stays empty)
@<file>                           anywhere: that file's lines, one argument each (UTF-8; blank lines and '#' lines skipped; no nesting)
```

Kinds: `sma_crossover`, `mean_reversion`; `ml` is listed and **unavailable**. Parameter names are the config field names.
`--param` applies to the most recent `--strategy`; strategies keep command-line order, which is registration and delivery
order. At most 16 strategies. `--max-rows` defaults to 50,000 (hard cap 200,000); a larger file is an error, never truncated.
Every argument is UTF-8; on Windows the C runtime delivers arguments in the ANSI code page, so put non-ASCII values in an `@file`.

**stdout is exactly one JSON document and its newline: a result or a `strategy_lab.error`. stderr is for humans** (the error,
warnings, `--help`). Output is written in binary mode (always LF); if stdout cannot be written the exit status is 4.

| Exit | Meaning | Document |
|---|---|---|
| 0 | Finished. Strategy errors, refused signals and warnings are **results** | `strategy_lab.replay` / `.catalog` |
| 2 | Usage; unknown, repeated or unparsable parameter; unavailable strategy; a configuration the **strategy itself rejects** (its message verbatim) | `strategy_lab.error` |
| 3 | Dataset unreadable, malformed, out of order, or over a limit | `strategy_lab.error` (with `problems[]`) |
| 4 | Internal error, or stdout could not be written | `strategy_lab.error` |

## 7. Dataset CSV (`strategy_lab_bars_csv/1`)

A lab-local schema, not the project's planned market-data fixture schema. The loader **validates and never repairs**:
every refusal names a line and a column, up to 50 are listed (the total is counted), and nothing is skipped, re-sorted,
clamped or defaulted. Full rules: `apps/strategy_lab/lab/dataset.hpp`.

| Topic | Rule |
|---|---|
| File | UTF-8, LF or CRLF, optional BOM, header required, no blank lines, no quote characters, at most `max_rows` data rows and 4096 bytes per line; a file with no data rows is an error |
| Columns | Required: `symbol`, `exchange_time`, `type`, `price`. Optional: `open`, `high`, `low`, `volume`. Any order, none repeated, no others |
| Time zone | `exchange_time` is RFC 3339 **UTC**: `YYYY-MM-DDTHH:MM:SS[.f{1,9}]Z`, years 1970-2261, real calendar dates, no leap seconds. **No offsets, no conversion**: `+00:00` or a missing zone is an error. `ingest_time` is set equal |
| Interval | **Not carried and not checked.** The strategies expect finalized bars of one consistent interval per symbol (`MarketEvent` has no such field, `OQ#1`); that is the data author's responsibility. Windows count accepted bars whatever the spacing |
| Symbols | 1-32 printable ASCII characters, no comma or quote; case preserved. The strategies match **exactly and case-sensitively**: `aapl` is not `AAPL`, and a mismatch is reported as a warning, not fixed |
| Types | `bar` (the price is the close) and `trade` (so a dataset can show the strategies ignoring it). Anything else is an error |
| Prices | `price` is required and must be a **plain decimal number greater than 0 with at most 6 decimal places** (`101.25`, `3`, `.5`, `0.000001`; `1.2500000` is fine, trailing zeros past the sixth change nothing). It is read **exactly** into a `common::Decimal` (an int64 count of millionths): no floating point, no rounding. `nan`, `inf`, `1e3`/`1e-7` (any exponent), `0`, negatives, `+1`, empty, spaces, hex, **more than 6 real decimals** (`0.0000001`) and a value that does not fit int64 (`9223372036854.775808`) are errors, never turned into a valid value. `open/high/low` follow the same rules and must be consistent with the close; `volume` is a **whole number of shares** >= 0 (`1500` or `1500.0`, not `1500.5`); bars only |
| Per-symbol order | A symbol's **bars strictly increase** in time (a duplicate or older bar is an error, not dropped), and no row precedes that symbol's previous row |
| File order | Across the file `exchange_time` never decreases (the replay clock only moves forward). Rows are replayed **in file order; nothing is sorted** |
| Ties | Rows of **different symbols with equal `exchange_time` replay in the order the file lists them**. This is a **lab-only policy** so a run is reproducible. It is **not** the production replay's merge or tie-break (plan M3 says ties break by `sequence`; undecided and unimplemented) and it decides nothing about it. A trade may share a timestamp with a bar of its symbol |
| Fingerprint | SHA-256 of the file bytes is reported; the file's name (never a path) identifies it |

How a strategy treats a **malformed event that reaches it** (a zero, negative or extreme price, repeated time...) is a separate
matter: the loader refuses such rows, so those cases are tested with in-memory events (section 12). A `Price` is an int64, so it
can never be NaN or infinite.

## 8. Strategies, parameters and defaults (single authoritative mapping)

`lab/catalog.cpp` has one table per kind. It drives parsing, strategy construction and `describe`; defaults are read from the
default-constructed config structs, so they cannot drift. Validation is the strategy constructor's alone: the lab only parses
text into the right type (a negative number never wraps to a huge unsigned value). `describe` publishes all of this, plus each
kind's decision reasons, indicator and state names, metadata keys, the CSV columns, limits and bus faults.

| `sma_crossover` | Type | Default | Rule (enforced by the constructor) |
|---|---|---|---|
| `strategy_id` | string | `sma_crossover` | Non-empty; unique per run |
| `short_window` | uint | `5` | At least 1, below `long_window` |
| `long_window` | uint | `20` | Above `short_window` |
| `requested_quantity` | uint | `1` | Positive whole number of shares (an exact int64 count; above INT64_MAX is refused, never wrapped; a fraction, sign, `nan` or exponent is refused at parse time) |
| `symbols` | list | none (required) | Non-empty, no empty entry, no repeat |

| `mean_reversion` | Type | Default | Rule |
|---|---|---|---|
| `strategy_id` | string | `mean_reversion` | As above |
| `lookback` | uint | `20` | At least 2 |
| `entry_threshold` | double | `2.0` | Finite, above `rearm_threshold` |
| `rearm_threshold` | double | `0.5` | Finite, at least 0, below entry |
| `requested_quantity`, `symbols` | | `1`, none | As above |

Derived facts reported in `configuration.strategies[].derived`: crossover `earliest_signal_accepted_bar` = `long_window + 1`;
reversion `max_abs_z` = `sqrt(lookback - 1)` and `entry_threshold_reachable`. An unreachable entry is accepted by the strategy
and never fires; the lab warns (`entry_threshold_unreachable`) and does not reject. Other warnings (never repairs):
`dataset_symbol_not_allowlisted`, `allowlisted_symbol_not_in_dataset`, `fewer_bars_than_warmup`.

## 9. Output: schema version 1.0

Three documents: `strategy_lab.replay` `{schema, schema_version, provenance, result}`, `strategy_lab.catalog`, `strategy_lab.error`.
`schema_version` is `"MAJOR.MINOR"`: an additive member bumps the minor, a rename, removal or re-type the major; consumers
ignore unknown members and require the same major. **This version belongs to the lab's own JSON. It is not tied to the
approval status of ADR 0002 or any other ADR**, and no ADR is an input to it. Signal members use the C++ field names
(`requested_quantity`, `order_type`, `side`, `created_at`, `strategy_id`). Absent optional values are **omitted, never `null`**.
**Indexing:** `events[].index` and `signals[].event_index` are **0-based**. Prose that says "bar 5" or "event 5" (this document, the UI's
"Event 5 of 9") counts from 1, so the crossover fixture's Buy is `event_index` 4.

| Member | Contents |
|---|---|
| `provenance` | **The only place the environment appears:** `tool`, `project_version`, `compiler`, `build`, `generated_at` (wall clock, omitted by `--no-wall-clock`), and `result_sha256` = SHA-256 of the **compact** `result` bytes (in compact output, exactly what follows `"result":` up to the document's final `}`) |
| `result.run` | `run_id` (first 16 hex digits of SHA-256 over schema version, dataset hash, bus fault and each strategy's kind and resolved parameters: reproducible, not a timestamp), `mode: signal_replay`, a fixed notice, the signal-id scope, `context` (engine, bus, clock, replay order, fault) |
| `result.input` | Dataset `name`, format, `sha256`, bytes, rows, per-symbol bar/trade counts and first/last time |
| `result.configuration` | `max_rows`, `bus_fault`, and per strategy `kind`, `strategy_id`, `window_size`, every parameter with defaults filled, `derived` |
| `result.warnings[]` | `code`, `message`, `strategy_id?` |
| `result.events[]` | One per replayed row, in order: `index`, `source_line`, `symbol`, `exchange_time`, `type`, `price`/`open`/`high`/`low`/`volume` (omitted if absent), `bus_sequence`, and `results[]`, one per strategy (below) |
| `events[].results[]` | `strategy_id`, `diagnostics_available`, then `verdict`, `reason`, `action`, `window {fill?, remaining?, size}`, `indicators{}` (only values that exist), `unavailable{}` (name to reason), `states{}`, `signal_ids[]`. If a strategy reported nothing for an event, `diagnostics_available` is false with a reason |
| `result.signals[]` | In publish order: `signal_id`, `signal_ref` (`<run_id>:<id>`), `strategy_id`, `event_index`, `bus_sequence`, `symbol`, `side`, `order_type`, `requested_quantity`, `created_at`, `metadata{}` (real, as the strategy wrote it); `target_exposure`, `limit_price`, `confidence` only if set (never today). **No hold signal is ever manufactured** |
| `result.publication_failures[]` | Signals the bus refused or threw on: the signal as stamped, `event_index`, `kind` (`rejected` / `publish_threw`), `message` |
| `result.engine` | The engine's `Stats`, each strategy's `StrategyStats` plus `observer_failures`, and the bus summary |
| `result.summary` | Counts only: events, bars, signals; per strategy buy/sell, verdict and reason counts |

**Money, price and quantity are exact integers underneath and are never written through a double.** A price (`price`, `open`,
`high`, `low`, a signal's `limit_price`) is a `common::Decimal` (int64 millionths) written as its exact decimal text: `150.02`,
`0.000001`, `9223372036854.775807`, never an exponent and never `NaN`. A quantity (`requested_quantity`, `volume`) is a whole number
of shares, written as a JSON integer (`1`, `9223372036854775807`). **Read prices with an exact decimal type** (Python:
`json.loads(text, parse_float=decimal.Decimal)`); a float cannot hold the larger ones, and JavaScript loses integer precision above
2^53. The wire text of a valid price or quantity is the same as before this was made exact, so the schema version did not change.

A **non-finite number is never written** (JSON has none). Only the DERIVED real numbers can be non-finite: a field such as a
signal's `confidence` or `target_exposure` (rates, doubles) is omitted and `<field>_status` says `nan`, `inf` or `-inf`; an
indicator that does not exist, or is non-finite, is listed under `unavailable` with its reason (`warming_up`, `event_ignored`,
`constant_window`, `measurement_failed`, `non_finite`). Derived statistics (averages, z-scores, thresholds) are doubles written as the
shortest text that reads back to the same bits, independent of any locale; whole values print without a fraction (`100`, `-0`), so
a consumer must read them as numbers, not as integers versus floats. Times are RFC 3339 UTC with nine fractional digits. Counters
are JSON integers (exact, though a JavaScript consumer loses precision above 2^53).

## 10. Determinism and ordering

1. Everything under `result` is a pure function of the dataset bytes and the request: no wall clock, randomness, hash-order
   iteration or thread. Compare runs by `result_sha256`. Output is LF-only; golden comparisons elsewhere should normalize CRLF.
2. Replay order is file order (section 7). Per row: publish one `MarketData` event; the engine delivers to strategies in
   registration order; each emits in its own order. Signal ids count 1, 2, 3... in publish order, **fresh in every run** (a new engine
   per run), and `signal_ref` scopes them by `run_id`.
3. Members are written in the documented order; `metadata` is key-sorted (it is a `std::map`), `indicators` and `states` follow the
   strategy's fixed order.
4. Strategies are built fresh for every run; reusing the same strategy objects across two runs also reproduces the first run,
   because `on_start()` clears every window, baseline and latch (tested).
5. The bytes were verified on **MSVC only**. Identical bytes on GCC or Clang are unverified.

## 11. Per-bar diagnostics

Both strategies keep their state private and ignored or silent bars left no trace, so the lab asks the strategy itself.
Header: `include/trading_engine/strategy/strategy_diagnostics.hpp` (its comment is the normative contract).

| Piece | Meaning |
|---|---|
| `IStrategyObserver::on_bar(const MarketEvent&, const BarSnapshot&)` | Called synchronously, once per `on_market_event()` that returns normally, after the state update and **before** any `emit()` |
| `BarSnapshot` | A by-value, read-only record: `verdict` (`ignored`, `warming_up`, `evaluated`), `reason`, `action` (`none`/`buy`/`sell`: **not** a hold signal), `window_fill`/`window_size`, up to 4 named `indicators` (a value is **empty when unavailable**, never zero) and 3 `states` (before/now/after). Its `string_view`s are valid only during the call |
| `set_observer()`, `observer_failures()` | On the two **concrete** strategies only. `IStrategy`, `ISignalSink`, `TradeSignal`, `MarketEvent` and the bus are untouched |
| `ObserverSlot` | One shared implementation of the policy below |

**Observer contract (enforced, and tested).**
* **Exceptions.** An observer that throws (any type, including `std::bad_alloc` and non-`std::exception`) is swallowed and counted in
  `observer_failures()`. The decision, state, emitted signals and **every engine counter** are exactly what they would be with no
  observer; it is not counted as a strategy error. (Rethrowing from the `noexcept` slot would terminate the process; MSVC flags it as C4297.)
* **No re-entry.** An observer must not call back into the strategy, the engine or the bus. Calling `on_market_event()`, `on_start()`
  or `set_observer()` on the same strategy from inside `on_bar()` throws `std::logic_error` to the observer, **before touching state**,
  so a violation is deterministic. Re-entering the engine is covered by the engine's own rule (a nested market event is dropped and
  counted in `events_dropped`; nested `start()` / `stop()` stay unsupported). Both are tested.
* **Read-only.** The observer receives const references to values built for the call; nothing points into strategy state.
* **Cost when unset:** one pointer test per event, before any work. `set_observer()` may only be called between events.

**Why trading behaviour does not change, and how that was checked rather than argued.** The report calls sit at existing exits of
`on_market_event`; they read values the decision already computed (the one new expression is `short - long`, and a branch label).
That is rationale. The evidence: with the observer on and off, on seeded streams of 4000 events full of zero, negative, extreme
(INT64_MIN/INT64_MAX) and absent prices, repeated and older timestamps, unlisted and wrongly cased symbols and non-bar events, across 3 crossover
windows and 3 reversion setups, the signals (every field, metadata included) are identical, driven directly **and** through the real
engine, where `stats()` and every `strategy_stats()` field also match; one snapshot arrives per call; throwing and re-entering
observers change nothing; attach, detach and engine restart are covered. All 189 pre-existing tests pass unmodified. A mutation
check (an observer that changes state, one whose reported value disagrees with the decision, one that lets exceptions escape, one
without the re-entry guard) fails the tests each time.

## 12. Warm-up and suppression semantics

"Accepted" means a bar passed every check below; only accepted bars fill a window. An ignored event changes nothing. Checks run in
this order and the **first failure is the reason**. `describe` carries a plain-language text for each code.

| Reason (verdict `ignored`) | When |
|---|---|
| `not_a_bar` / `symbol_not_allowlisted` / `price_absent` | Not a bar / symbol not in `symbols` / no price |
| `price_invalid` | Zero or negative (a `Price` is an int64: never NaN or infinite) |
| `price_above_max_close` | Crossover only: above `INT64_MAX / long_window^2` millionths, so its exact integer sums cannot overflow |
| `time_not_after_last_accepted` | `exchange_time` not later than the symbol's last accepted bar |

| Accepted bar | Verdict | Signal? | Effect |
|---|---|---|---|
| **Crossover** `warming_up` | `warming_up` | no | Until `long_window` bars; no averages exist |
| `baseline_established` | `evaluated` | no | First nonzero short-vs-long relation: recorded, **never signalled** (first possible signal: bar `long_window + 1`) |
| `averages_equal` / `same_side` | `evaluated` | no | Exactly equal (compared as integers, no tolerance; keeps the last relation) / still on one side |
| `crossover_buy` / `crossover_sell` | `evaluated` | **yes** | Relation committed before emitting |
| **Reversion** `warming_up` | `warming_up` | no | Until `lookback` bars; **no silent baseline**: the first full window may signal |
| `measurement_failed` / `constant_window` | `evaluated` | no | Guard, latch unchanged / deviation negligible: z unavailable, latch rearms |
| `entry_buy` / `entry_sell` | `evaluated` | **yes** | `z <= -entry` and latch not lower / `z >= +entry` and latch not upper (inclusive; a direct flip emits at once) |
| `excursion_already_requested` / `inside_rearm_band` / `between_bands` | `evaluated` | no | Beyond entry on the requested side / `abs(z) <= rearm` (latch neutral) / otherwise |

## 13. Fixtures, expectations and tests

Expected sequences are **hand-derived from the strategy documents** and written as literals in the tests (each case's derivation is in
a comment beside it); none is a snapshot of the implementation. All are synthetic, one bar per minute from `2026-01-05T14:30:00Z`.

| Fixture | Series | Expectation (hand-derived) |
|---|---|---|
| `sma_crossover` | AAPL 3 2 1 2 3 4 3 2 1; SMA 2/3 | Buy bar 5 (short 2.5, long 2), Sell bar 8 (2.5, 3); baseline at bar 3 |
| `sma_oscillation` | 1 2 1 2... ; SMA 1/2 | Flip on every bar from bar 3: 8 signals (first at `long_window + 1`) |
| `mr_rearm` | 10 10 10 6 9 2 30; lookback 4, 1.5 / 0.5 | Buy 4 (z -1.732), rearm 5, Buy 6, Sell 7 (flip) |
| `mr_suppression` | 10 10 10 6 2 1 1 1 1 10 | Buy 4; no repeat 5-8; constant window 9 rearms; Sell 10 |
| `constant_price` | 30 x 0.1; defaults | No signal ever: equal averages / constant window |
| `warmup_boundary` | 10 x5 then 100 | Reversion lookback 6: Sell on the **first full window**; lookback 7: nothing; crossover 2/6: only a baseline |
| `two_symbols_interleaved` | AAPL and MSFT per minute; `sma_2_3` + `mr_4` | The 8 signals and ids of `strategies_combined_engine_test.cpp`; each symbol decided on its own |
| `tie_aapl_first` / `tie_msft_first` | Both symbols sell at one timestamp | Id 1 goes to whichever the file lists first |
| `bars_and_trades` | Bars with trade rows | Trades are `not_a_bar`, windows untouched; Sell on bar 3 |
| `ohlcv_example`, `precision_edge`, `escape_symbol` | Optional fields; exact-decimal edges (one millionth, 6-decimal fractions, trailing zeros, INT64_MAX); a `\` in a symbol | Round-trip and escaping tests |

What ran (all on this machine, MSVC 19.39, warnings treated as errors; **a build was completed before every test run**):

| Suite | Count | Covers |
|---|---|---|
| Diagnostics (`reference_strategies`) | 21 new (116 total) | Every bar of both worked examples, every ignore reason, snapshot/signal agreement, the invariance streams, observer exceptions and re-entry, attach/detach/restart |
| `strategy_lab` GoogleTest | 131 | UTF-8, timestamps (vs hand epoch values), SHA-256 (vs FIPS vectors), JSON writer (escaping, shortest doubles, exact decimal prices, hostile locale, refusal of NaN), **CSV validation (exact decimal prices and whole-share volumes, every refusal)**, the bus contract and the engine on it, catalog and verbatim configuration errors, **fixture sequences, warm-up boundaries, per-symbol isolation, combined strategies, ties, repeatability, malformed events (in-memory), publication failures, run identity, the CLI exit and stdout/stderr contract** |
| `strategy_lab_python` | 51 | A **real JSON parser** (duplicate keys and NaN/Infinity refused): documents, hostile text, exact edge doubles for derived numbers, **prices and quantities read digit for digit at the int64 extremes**, counters beyond 2^53, locale independence, error documents with invalid UTF-8, exit statuses, byte-reproducibility, CRLF absence, write failure, a 20,000-row run, and **agreement with an independent exact-rational reference** (`reference_model.py`, test-only, written from the docs with `fractions`) on all comparable fixtures |

Results (final tree, clean strict builds, after money/price/quantity became exact integers): default build (lab OFF) 240/240; lab ON, whole suite 372/372 in Debug and in Release (132 of them labelled `strategy_lab`); the **whole** 372/372 under AddressSanitizer with no report (an earlier run filtered by label and covered 342; the other 25 are the `smoke`, `domain`, `scaffold_contract` and `support` components, which that filter did not select)
(MSVC `/fsanitize=address`; on Windows 11 build 26200 the runtime also needs `ASAN_WIN_CONTINUE_ON_INTERCEPTION_FAILURE=1`). A mutation
check of 14 deliberate defects in the lab (clock not advanced, duplicates accepted, price 0 accepted, rows re-sorted, backslash unescaped,
NaN written as null, no unsubscribe barrier, wrong run id, wrong exit status, wall clock in `result`, paraphrased errors, CRLF, ignored
flush failure) found every one, and six further defects after the int64 change (an overflow bound a factor too loose, the crossover comparing with the wrong windows, the CSV loader truncating extra decimals, the bound computed in signed arithmetic so a huge `long_window` wraps, mean reversion converting closes to double before subtracting) were each caught (the CSV one at compile time, by a `static_assert` on the parse). Not run: GCC, Clang, ThreadSanitizer, UBSan (MSVC has none: signed overflow in the exact sums is prevented by the `max_close` bound and tested at its boundary, not detected), POSIX.

## 14. Explore interface (implemented) and what is still planned

Streamlit + Plotly in `python/strategy_lab/`, bound to `127.0.0.1`. It starts `strategy_lab_replay` as a subprocess, validates the JSON it
writes and shows it; **it never computes a decision** (every price, average, mean, z-score, threshold, marker and reason on screen is a
value or text the C++ tool wrote). Modules: `bridge` (the only place a process starts), `schema`/`catalog`/`jsonio` (strict parsing),
`replay` (cursor, prefix, filters), `state` (request/snapshot/status), `session` (widget state and callbacks), `charts`, `tables`,
`inspector`, `details`, `exports`, `datasets`, `sidebar`, `views`.

### 14.1 Setup and launch (Windows 11, PowerShell; verified on Python 3.13.0, Streamlit 1.64.0, Plotly 7.1.0)

Needs Python 3.10+ and the replay tool built as in section 4. Internet is needed **once**, to download the two pinned packages
(`python/strategy_lab/requirements.txt`) into `python\strategy_lab\.venv`; nothing is installed globally and C++ is never rebuilt by the UI.

```powershell
# once: create the isolated environment and verify the pins
powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\setup.ps1
# any time: start the lab (checks the prerequisites and says what to do if one is missing)
powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\run_lab.ps1
```

Open <http://127.0.0.1:8501> (the launcher opens it after a few seconds; `-NoBrowser` suppresses that; `-Port 8502` and
`-Exe <path>` are the other options). The launcher checks and prints the app-scoped `.streamlit\config.toml` it relies on (dark theme, `127.0.0.1` only). Ctrl+C in that window stops it. After setup, the built-in demos work **offline**; Streamlit's usage statistics are off (`.streamlit/config.toml`). `setup.ps1 -Recreate` rebuilds the environment. The launcher binds the server
to `127.0.0.1` only. The executable is found from `-Exe`, else `$env:STRATEGY_LAB_EXE`, else `out\strategy_lab\bin\<Release, RelWithDebInfo,
MinSizeRel, Debug>\`; no path is ever taken from the browser. Manual start, from `python\strategy_lab`:
`.venv\Scripts\python.exe -m streamlit run app.py --server.address 127.0.0.1`. The scripts are ASCII-only for Windows PowerShell 5.1.

### 14.2 Workflow and layout

Choose dataset and strategy, press **Run replay**, inspect the chart, step through the recorded decisions.

* **Header:** "Strategy Lab / Signal replay", the displayed run's dataset (name, synthetic or unknown provenance, SHA-256 prefix, run number)
  and a status chip: *No run yet*, *Run complete - settings match*, *Settings changed*, *Last run failed*.
* **Left panel:** built-in dataset or CSV upload; strategy (the catalog's available kinds; ML is listed as unavailable); parameters; the
  **symbols allowlist** (it changes what is computed); *Advanced settings* (collapsed: `strategy_id`, `requested_quantity`); **Restore defaults**
  and **Run replay**, kept visible at the bottom of the panel. Parameter names, types, defaults and constraint text come from `describe`; the
  UI keeps no copy, and every other check is the runner's (its messages are shown verbatim). Choosing a built-in fixture or a strategy loads a
  **demo preset** (the parameter pairs the lab's own tests use for that fixture); Restore defaults loads the C++ defaults instead.
* **Chart (full width):** close line; status markers for every revealed row (evaluated dot, warming-up open circle, ignored x, trade-row diamond);
  SMA crossover draws the short and long averages; mean reversion draws the rolling mean and a **separate z-score panel** with the run's configured
  entry and re-arm thresholds; Buy (triangle up, text "Buy") and Sell (triangle down, text "Sell") markers at the signalling event; a ring on the
  selected event. A value that does not exist is a gap, never 0. With several symbols the chart stacks one panel pair per symbol (at most 4).
* **Replay controls:** Reset, Previous, Next bar, Next signal, a timeline slider, and the **display-symbol** selector.
* **Decision inspector:** the selected event's verdict, reason code and the catalog's plain-language reason text, outcome (a silent bar is "No request",
  never a hold signal), window fill, indicator values or *unavailable* with the reason, states, and every signal request on that event with its metadata.
* **Beside it:** *Replay so far* (counts over the visible prefix) and *Full run* (whole-result facts that never change with the position).
* **Tabs:** *Signals* (prefix table, two CSV downloads), *Diagnostics* (reason counts so far, one row per revealed event), *Run details*
  (dataset provenance, the configuration that was run, runner warnings, run and build identity, whole-run summary, JSON download, command and stderr).

### 14.3 State and replay semantics

1. **Four kinds of state, kept apart:** editable settings (widget values); the **immutable snapshot** of the displayed run (`RunRequest`: dataset kind,
   name and SHA-256, strategy, every parameter text sent, executable SHA-256); the parsed completed result; playback position and display filters.
2. A result is labelled *current* only when the request built from the settings on screen equals its snapshot. Any difference (dataset, strategy,
   any parameter text, the executable being rebuilt) marks it **stale**, with the reasons, until Run. A failed run keeps the last successful result,
   labelled as such, and shows the failure; the next success clears it. Nothing from other inputs, settings or executable builds is reused.
3. **Only the Run button starts the engine** (`session.launch`, the sole caller of `bridge.run_replay`). Stepping, the slider, symbol filtering, tabs, downloads
   and editing settings never do; Run details shows the launch counter, and the tests wrap the call with a counter. No result caching is used.
4. **Cursor:** a number of events revealed, 0..N, in the recorded order (file order, equal timestamps and interleaved symbols exactly as replayed).
   Position 0 shows an empty state; position k shows only events 1..k in the chart, markers, tables, counters and inspector, so going back removes the future.
   A new run starts at 0. The slider moves over **all** events regardless of the display filter.
5. **Allowlist versus display symbol:** the allowlist (left) is a parameter of the run. The display-symbol selector (above the slider) only hides rows of
   an existing result. **Next bar / Previous** step over the displayed symbol's rows; **Next signal** jumps to the next event, after the cursor, that produced a
   signal for the displayed symbol(s); several signals on one event are one stop, all listed in the inspector and tables. Filters never move the cursor.
6. **Joins:** events and signals are linked by the integers `event_index`, `signal_id` and `strategy_id`, never by timestamp, float or text. Parsing proves the
   links (indexes 0..N-1, each signal's event lists its id and symbol, no unknown ids, counts agree) before anything is drawn. Integers beyond 2^53 stay
   exact; times stay as the tool's text (the chart axis uses millisecond placement only; nanoseconds appear in hovers, tables, the inspector and exports).

### 14.4 What happens when something goes wrong (the details are in each message's expander)

| Situation | What the UI does |
|---|---|
| Executable missing / not the replay tool / cannot start | Setup message with the two build commands; no Run button (`describe` is how an executable is accepted) |
| Empty, over 8 MiB or binary upload | Inline error under the uploader, Run disabled, nothing launched |
| Dataset rejected (exit 3), configuration rejected (exit 2), internal error (4) | The runner's own message, and its problems with line and column in a table; the last successful run stays, labelled |
| Exit status other than 0/2/3/4, or an error document that disagrees with the status | Reported as unexpected or malformed, with exit status, stderr and the start of stdout |
| No output, invalid UTF-8, invalid JSON, duplicate keys, NaN/Infinity, text after the document | `malformed_output`; nothing is displayed |
| Schema name or major version not understood; inconsistent links or counts | `unsupported_schema` / `invalid_structure`, naming the JSON path |
| A result whose dataset SHA-256, strategy or `max_rows` differs from the request | `result_mismatch`; never displayed |
| Over 60 s, or over 128 MiB of stdout | The process is killed and the run reported (stderr is capped at 1 MiB) |
| A valid run with no signal requests, or with runner warnings | Shown as results: an info line, the warnings by code |

The process runs with `shell=False`, from a private temporary directory that is always removed. Every argument travels in a UTF-8 `@args.txt` response file
referenced by a pure-ASCII **relative** path (Windows hands the tool its command line in the ANSI code page; the dataset path and symbols such as
non-ASCII allowlist entries live inside the file). The dataset is copied to `dataset.csv` there, so the tool reports that name; the UI shows the file's
real name. An argument that file cannot carry (empty, a line break, starting with `#` or `@`) is refused rather than altered. `--max-rows` is 20,000.

### 14.5 Exports (each says what it covers)

* **Run JSON, FULL RUN:** the replay tool's stdout, byte for byte (so `provenance.result_sha256` still verifies); never re-serialized.
* **Signals CSV, VISIBLE PREFIX** and **Signals CSV, FULL RUN:** one row per signal request: run-scoped `signal_ref`, 0-based `event_index` and 1-based
  `event_number`, symbol, side, quantity, `created_at` (nanoseconds), bus sequence and every metadata key. The `export_scope` column repeats the scope on each row, and the
  file name carries it. The prefix export is exactly the Signals table (same cursor and symbol filter). Values are written as recorded; a spreadsheet may treat a
  cell starting with `=`, `+`, `-` or `@` as a formula.
* **Exact prices, quantities and ids in the tables, and the "Sort rows by" control.** A price (`Close`), a whole-share quantity and a signal id are
  shown as **exact text**, right-aligned. `st.dataframe` cannot do better itself, which was checked in a browser (Streamlit 1.64): a text column sorts as
  text (`100` before `20` before `3`); a numeric column is converted to a floating-point number in the browser, so `9223372036854.775807` and
  `9223372036854.775806` both display as `9223372036854.773`, and integers above 2^53 are flagged and mis-ordered; there is no option to turn
  header sorting off. So the exact order is an explicit **Sort rows by** selector above each of the four sortable tables (Explore and Compare, Signals
  and Diagnostics): *Recorded order* (the default), or a column low to high / high to low. The rows are ordered in Python on the exact `int` / `Decimal`
  values (no float is involved; equal values keep their recorded order and rows without a value come last), and the choice is kept when you leave the
  view. **Limitation:** clicking the header of a price, quantity or id column still sorts as text; use the selector for numeric order (its help text says
  so). Indicator columns are derived statistics (floats) and sort natively. The plotted line is the only float made from a price, and it is for drawing.

### 14.6 What was checked (2026-10-02, this machine only)

| Check | Result |
|---|---|
| `python\strategy_lab\.venv\Scripts\python.exe -m unittest discover -s tests` (from `python\strategy_lab`) | **186 tests, all pass** (about 15 s; **433 now**, with Compare, Validate and the theme: [STRATEGY_LAB_COMPARE_VALIDATE.md](STRATEGY_LAB_COMPARE_VALIDATE.md) section 6): strict JSON and schema (malformed, unsupported, inconsistent, ids beyond 2^53, equal timestamps), replay prefix/navigation/filters/two strategies on one event, charts, exports and stale/failed status, datasets, the subprocess bridge (fake runner: timeout, flood, exit statuses, UTF-8, non-ASCII temp directory, cleanup; **and the real executable**) and 32 AppTest scenarios against the real executable |
| Targeted mutation check of the UI core (22 deliberate defects: future row revealed, filters ignored, stale reported as current, links/major version/dataset hash/exit status unchecked, timeout off, export scope, thresholds hard-coded, gap drawn as zero, an engine launch on navigation, ...) | all 22 killed in the end: 20 at once, and two (the per-signal link check; another strategy's signals leaking into a model) only after a test was added for each |
| Existing C++ `reference_strategies` suite (diagnostics, invariance streams), strict build, Debug | 116/116 pass. This covers the question whether moving the per-symbol state reference ahead of the invalid-price checks changed ignored events: it binds a reference only (`find`, no insertion, state is built in the constructor); no C++ was changed |
| Real browser (the Claude desktop app's built-in Chromium pane, 1440x900 emulated) | Built-in SMA run and its Buy at event 5 and Sell at event 8; mean reversion run (3 requests, z panel with the configured thresholds); two-symbol run with equal timestamps and the MSFT filter; forward and backward stepping (the chart's traces shrink to the prefix); stale labelling after a parameter edit; a failing configuration (message verbatim, last run kept and labelled); Restore defaults (5 and 20); a valid run with no signals; CSV upload (valid, invalid with line/column, empty); the prefix CSV's content; Run details showing launch number 1 after a run, nine Next-bar clicks and a tab change |
| Setup and launcher | `setup.ps1 -Recreate` from nothing under Windows PowerShell 5.1: exit 0, both pins verified (streamlit 1.64.0, plotly 7.1.0; 38 packages in all, pandas 3.0.6, numpy 2.5.3), then the 186 tests re-run in that environment: all pass. `run_lab.ps1` refusals checked: no environment, missing executable, port in use (each prints what to do, exit 1). The page loaded 110 resources, **all from 127.0.0.1:8501** (the network was not disconnected, so "offline" means no external host is contacted) |
| Not done | Other browsers, a phone/tablet width, a screen reader, a very large (20,000-row) run in the browser, autoplay (not implemented), the default launcher's real browser-open (only its command syntax was checked; `-NoBrowser` was used) |

Automated AppTest checks are **not** browser checks: they run the script and widgets without a DOM. The file input was driven in the browser with a `DataTransfer`
object (no native file dialog), and downloads were verified by fetching the URL a click produced, not by saving a file.

### 14.7 Troubleshooting

| Symptom | Fix |
|---|---|
| "Python 3.10 or newer was not found" | Install Python 3.12/3.13 from python.org (tick *Add to PATH*) or `winget install Python.Python.3.12`; open a new terminal; the Microsoft Store shortcut does not count |
| `setup.ps1` cannot download packages | Connect to the internet once; or use `-Python` with another interpreter; `-Recreate` after a half-finished install |
| "the Python environment does not exist yet" | Run `setup.ps1` |
| "strategy_lab_replay.exe has not been built" | The two commands in section 4 (or `-Exe`) |
| "port 8501 ... already in use" | A lab is already running (close it), or `-Port 8502` |
| `running scripts is disabled` | Use the `powershell -NoProfile -ExecutionPolicy Bypass -File ...` form shown above (it affects only that process) |
| A change to a UI module has no effect | Streamlit keeps imported modules: stop (Ctrl+C) and start the lab again |
| The result looks old | The banner says which settings changed; press Run replay. After rebuilding C++, press Run again: a different executable marks the old result stale |

### 14.8 Compare, Validate and the dark theme (implemented; details in [STRATEGY_LAB_COMPARE_VALIDATE.md](STRATEGY_LAB_COMPARE_VALIDATE.md))

The title row switches between Explore, Compare and Validate; each keeps its own settings, results and position, and switching never starts the engine. **Compare** runs two
configurations in two independent runner invocations over one shared dataset, with a shared cursor and filter, member-scoped signal identities and its own exports. **Validate** runs the
checked-in scenarios against the real executable and compares them with expectations derived by hand (Passed / Failed / Error; it is **not** the CTest suite). The theme is dark by default
(config file, `theme.py`, charts). Wording rule everywhere: "request", never "trade" or "profit".

## 15. Reviews required before merge (NOT done)

`COMPONENT_OWNERSHIP.md` requires the reviews below. **None has happened;** this table records what is owed, not what is done.

| Changed | Required review | Status |
|---|---|---|
| `apps/CMakeLists.txt` (option, subdirectory), `tests/unit/CMakeLists.txt` (component, probe, Python test), `apps/strategy_lab/CMakeLists.txt` | CMake target structure: **A + one other** | **Not reviewed** |
| `strategy_diagnostics.hpp`, the two strategies, `strategy.cpp` | strategy area owner (**C**) + one other | **Not reviewed** |
| `tests/fixtures/strategy_lab/` (new fixture directory) | fixtures owner (**B**) informed; it is a lab-local schema | **Not reviewed** |
| `python/strategy_lab/` (new: Explore, Compare and Validate UIs, the dark theme, their tests, scripts and the pinned Streamlit/Plotly requirements) | next to `python/visualization/`, which area **C** owns: **C + one other** | **Not reviewed** |
| `tests/fixtures/strategy_lab/scenarios/`, `invalid/` (validation scenarios with hand-derived expectations) | fixtures owner (**B**) informed | **Not reviewed** |
| This and the strategy docs | the docs' maintainer | **Not reviewed** |

## 16. Limitations and open questions

* **MSVC only** was available: GCC and Clang builds, Release byte-identity across compilers and POSIX launch are unverified. The code avoids
  `std::chrono::parse` and uses only C++20 library features the project already relies on, but that is not evidence.
* Diagnostics exist on the two concrete strategies only. Promoting the hook to `IStrategy` (for a future ML strategy) is a separate decision
  (ADR 0002 section 9 keeps `IStrategy` untouched).
* `common::RunId` is a numeric session id for persistence that does not exist yet, so the lab's `run_id` is a content hash instead.
* Output is row-oriented JSON capped at 50,000 rows by default (200,000 hard): readable and diffable, not compact. 20,000 rows were exercised.
* The lab replays through a synchronous test-grade bus: results say nothing about production timing, threading, backpressure or risk.
* The interval and "finalized bar" assumptions of the strategies cannot be verified from the data (`OQ#1`).
* Windows delivers arguments in the ANSI code page; non-ASCII values need an `@file`.
* **UI:** Explore was verified with the built-in Chromium pane; Compare, Validate and the theme with Chrome 154 through the Playwright MCP (1440 and 1024 px); all on Windows 11 only; Python 3.13.0 only (3.10+ is the stated floor, not tested); only the two
  direct packages are pinned (their dependencies float within Streamlit's and Plotly's ranges). Explore shows one strategy per run (Compare shows two, from two runs), has no autoplay, and caps
  uploads at 8 MiB and datasets at 20,000 rows. All built-in datasets are tiny synthetic fixtures (3 to 30 bars per symbol), so charts are sparse; the demo
  presets are the parameters the tests use, **not recommended trading parameters**. The browser repaints the whole chart on every step (fine at demo size; not
  measured at 20,000 rows). Streamlit's `st.dataframe` draws its cells on a canvas, so the Signals and Diagnostics tables are not selectable text (the CSV
  downloads are); short key/value lists are real HTML tables. The chart's time axis places points to the millisecond; the exact nanosecond times are in the tables and exports.
