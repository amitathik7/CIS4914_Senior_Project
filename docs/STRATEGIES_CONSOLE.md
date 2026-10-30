# Strategies in the Engine console

> Status (2026-10-10): **Configure**, **Explore**, **Compare** and the **Advanced** area (validation, traceability) are implemented in
> `web/` and checked on this machine only (Windows 11, MSVC build of the replay tool, Chrome through the Playwright MCP and the Claude
> app's built-in browser). An integration review on 2026-10-10 (section 9) fixed the findings listed there. Nothing is
> committed or pushed and **no ownership review has happened** (section 12).
> Everything here is **signal-only**, like the [Strategy Lab](STRATEGY_LAB.md) it reuses: it shows the Buy/Sell requests the real
> strategies emit and why, never orders, fills, positions or performance. **Compare has no financial comparison**, and section 7 says why.

## 1. What is real, what is a fixture, what is demo

| Part | Source | How it is labelled |
|---|---|---|
| Every replay result (signals, per-row verdicts, reasons, indicator values, window fill, warm-up), in Explore **and** in each side of Compare | The **real C++** `strategy_lab_replay` (real `StrategyEngine`, both reference strategies, lab-local synchronous bus) | "Real engine" badge ("Real engine · signal replay" in Compare), tooltip with the executable's name and SHA-256; Run details shows the same |
| Strategy names, parameter types, defaults, constraint text, reason texts, "Custom ML unavailable" | The tool's own `describe` document, fetched live | Nothing is copied into the console; friendly names and units are presentation (`strategies/copy.ts`) |
| "Accepted" / "rejected" verdict on a configuration (each side separately in Compare), derived facts, the unreachable-threshold warning | The **real strategy constructors**, asked through the gateway's `/check` | "Accepted by the engine's strategy code" / the engine's message verbatim |
| Built-in datasets | 11 synthetic fixtures in `tests/fixtures/strategy_lab/datasets` | "Synthetic fixture" badge |
| Uploaded CSV | Whatever the person chose; parsed by the tool's own validator | "Provenance unknown" badge |
| Scenario validation (Advanced) | The Lab's own scenario files and runner (`validate.run_all`) through the real executable | "These are scenario checks, not the repository's test suite"; every result names the executable and files it ran |
| "Recorded in this run" cards, and the recorded backtests in Compare's financial panel | The Console's engine API (`RunDetail`, reports, equity) | "Demo data" badge while no engine API is configured (the runs then come from the in-browser simulator) |
| Example presets | Parameter pairs the lab's own tests use per fixture | Labelled "Example ... not recommendations" |
| Saved presets | This browser's `localStorage` (`te.strategies.presets`), one list for Configure and both Compare sides | The menu says so, and says when the browser could not store them |

**Which strategies**: exactly the kinds the engine registers and describes: `sma_crossover` ("Moving-average crossover") and `mean_reversion`, with `ml` shown as unavailable (not implemented; nothing here fakes it).
The in-browser **demo engine is not involved in any replay.**

## 2. Run it

```powershell
# once: build the replay engine (docs/STRATEGY_LAB.md section 4)
cmake -S . -B out/strategy_lab -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON
cmake --build out/strategy_lab --config Release --target strategy_lab_replay

# terminal 1: the gateway (Python 3.10+, standard library only: no venv, no pip)
python python/strategy_lab/lab_gateway.py            # 127.0.0.1:8765; --port, --exe, --allow-origin

# terminal 2: the console
cd web
pnpm install
pnpm dev                                             # http://127.0.0.1:5173  ->  Strategies  ->  Compare
```

`pnpm lab` is the same as terminal 1. Vite proxies `/lab` to the gateway (`LAB_GATEWAY` overrides the target, `VITE_LAB_API` what the browser calls). **Restart a gateway that was started
before this change**: it does not have `/validation` and `/validate`. Without the gateway the section opens, says so at the Run button and offers Retry; the rest of the console is
unaffected. The executable is found from `--exe`, else `$STRATEGY_LAB_EXE`, else `out\strategy_lab\bin\<config>\`. A production `pnpm build` contains no gateway.

**Using Compare** (`/strategies/compare`, also under `/runs/:id/strategies/compare`): pick the dataset and the symbols both sides share, fill A and B (a **quick start**, **Copy A to B**, a preset, or by
hand), press **Run comparison**. The result shows aligned charts, a difference ribbon, an inspector for the selected row, the summaries, tables and exports. **Advanced** (header button) holds validation
and traceability. A and B start as copies of the Configure draft.

## 3. How it is put together

```
browser ── /lab/v1/* ──> Vite proxy ──> gateway.py ──> bridge.run_replay ──> strategy_lab_replay.exe
(React, lossless JSON)                  (stdlib HTTP)   (existing, tested)      (the real engine)
```

| Path | Role |
|---|---|
| `python/strategy_lab/strategy_lab_ui/gateway.py`, `gateway_body.py`, `gateway_validation.py`, `lab_gateway.py` | The only server code. Every replay goes through `bridge.run_replay`; validation through `validate.run_all` and `validation_report.report_dict` (both unchanged) |
| `web/src/lib/losslessJson.ts`, `exactTime.ts`, `zip.ts`, `reportFigures.ts` | Strict JSON reader that keeps numbers as text; exact nanosecond time display; a deterministic store-only ZIP writer; the Report page's headline figures, defined once and used by Report and by recorded-run comparison |
| `web/src/api/lab.ts` | `LabClient` / `HttpLabClient` (now also `validationInfo`, `validate`), `LabError` |
| `web/src/strategies/` | Framework-free logic. Explore: `catalog`, `draft`, `replay`, `cursor`, `explain`, `exports`, `presets`, `snapshot`, `store`. **Compare**: `compare` (alignment, per-row classification, scoped signal keys, navigation), `compareSummary` (counts, agreement, readiness), `compareEvent` (inspector rows), `compareState` + `compareStore` (two drafts, one dataset, launch lifecycle), `compareStarts`, `compareExports`, `recordedCompare`. **Validation**: `validation`, `validationStore`. Test helpers: `testClient`, `testDoc`, `testCompare`, `testValidation` |
| `web/src/components/strategies/`, `.../compare/`, `web/src/pages/Strategies*.tsx`, `web/src/styles/compare.css` | The UI. `ConfigPanel` was split into `StrategyKinds`, `ParamFields`, `EngineCheck`, `DatasetPicker`, `PresetMenu`, `SymbolChips` (each takes a host object) so Compare reuses them per side |

### Gateway endpoints (prefix `/lab/v1`, bound to 127.0.0.1)

| | |
|---|---|
| `GET /status`, `GET /catalog`, `GET /datasets`, `POST /check`, `POST /replay` | As before (runner identity and build instructions; the tool's `describe` verbatim; fixtures; the real constructors' verdict; one replay as metadata line + the tool's stdout verbatim) |
| `GET /validation` | The checked-in scenario files: directory, identity SHA-256, and one entry per scenario. Runs nothing, needs no executable |
| `POST /validate` | Body `{}` or `{"include_demonstration": bool}`. Runs every scenario through the real executable and answers with the Lab's `strategy_lab.validation_report`. A validation takes a few seconds and holds one of the two run slots |

Guard rails: loopback `Host` only, `Origin` must be the console's, POST needs `application/json` and at most 12 MiB, no path ever comes from the browser, at most two engine processes at once
(`503 busy`).

**Request bodies are bounded in size and in time, and every answer is a complete one.** Answering and closing with unread request bytes makes Windows reset the connection, and the client then never sees the status
(a flaky `405` test exposed it). So `_send`, the one exit of every response, first reads and discards whatever body is still unread (at most 12 MiB, at most `DRAIN_S` = 2 s **in total**, not per read, so a trickle cannot hold
the connection open). A body that will be used must arrive within `BODY_READ_S` = 30 s in total or the request is answered `400 "did not arrive completely and in time"`; a client that hangs up mid-body is not an error of the gateway.
Every response says `Connection: close` and the server speaks HTTP/1.0, so one connection carries one request and a body is never read as the next request. Known limit: a body declared above 12 MiB is answered `413` after its first 12 MiB are read, and a client that keeps sending the rest may see a connection reset instead of the `413` (the console cannot send such a body: uploads stop at 8 MiB). A chunked body (no `Content-Length`) is refused with `400` without being read.

## 4. Numbers (the project's int64 fixed-point contract)

* The tool writes a price as exact decimal text inside a JSON number and a quantity as an int64 integer. `JSON.parse` would round both, so the gateway passes the bytes through and the browser reads them with `parseLossless`, which keeps each number's text.
* Price, open/high/low, volume, quantity, signal ids and timestamps are **text** from the wire to the screen and **every export**. Indicators are derived doubles kept as `{value, text}`. The one place a price becomes a `Number` is `cursor.plotNumber` (chart coordinates). Agreement percentages use integer arithmetic.
* Sorting uses fixed-width text keys (`sortKeys.ts`), so two int64 values that are the same double still order correctly; Compare's signal and event tables use them too. **Timestamps** sort by `timeKey` (the fraction padded to nine digits): as written, `...:00Z` sorts after `...:00.5Z`, because the dataset allows 0 to 9 fractional digits and `Z` is greater than `.`.
* Timestamps keep all nine fractional digits (ET in tables, ET and UTC in the inspector and exports).
* A value that does not exist stays missing: `unavailable` with the tool's reason, a gap in the chart, an empty CSV cell, a dash with its reason in the financial table. **Never zero.** A silent row is never a Hold order.

## 5. Behaviour worth knowing (Configure and Explore)

* **Nothing reruns by itself.** `LabStore.runReplay` is the only Explore launch; a result is a frozen snapshot, marked stale (never edited) when its inputs change; one replay at a time, late answers dropped; validation is layered (client syntax, the real constructors via `/check`, the gateway boundary); exports name their scope on every row.
* **Use in new backtest** carries the strategy configuration only (router state to the New backtest form). Nothing starts.

## 6. Compare

### 6.1 Rules (each is enforced in code and covered by a test)

* **Two sides, one dataset.** A and B each have their own draft, engine check, result and identity. They share the dataset bytes and the **symbol allowlist** (editing it in either place edits both). Any strategy kinds or parameter sets; "Copy A to B" and two **quick starts** (crossover vs mean reversion, two crossover variants: B = A with the short window +1 and the long window +2) fill the editors and run nothing.
* **Only the button runs.** `CompareStore.runComparison` is the only launch and starts exactly two replays (each in its own fresh engine run, so independent strategy state), in parallel. Stepping, the slider, filters, tabs, difference mode, editing and every download never start the engine (`launches` counts; tests assert it, and the browser's network log showed exactly two `/replay` calls for a double-click).
* **Stale, not overwritten.** Editing a side, the dataset, the symbols or rebuilding the engine marks the comparison "Inputs changed: rerun to update", listing the differences per side. The comparison object is frozen and never relabelled. A failed launch keeps the last good one.
* **One launch at a time; late answers dropped.** A second click is refused. Cancel discards both in-flight answers; an answer for an older launch (even one arriving while a newer launch is still running) changes nothing. Tests stage these races with deferred promises.
* **Partial failure is shown as exactly that.** If one side fails: "Only A completed; B failed. There is no comparison from this launch", with the failed side's message, located problems and technical details; the finished side is described (run id, request count) but **not** presented as half a comparison. Both failing, or two results that are not the same recorded events (`comparison_mismatch`), are reported the same way.
* **Identity.** Each side has its **run id**, **result SHA-256** (from the tool), and a **configuration id** (SHA-256 of the exact request sent); the pair has a **comparison id** (SHA-256 over both configuration ids and the engine file). Identical configurations give identical run ids; every signal is therefore keyed `A/<run id>:<id>` or `B/...` (table rows, exports, inspector). A raw id alone never joins anything.
* **Event identity.** Rows are joined by their **position in the recorded order** (`event_index`), after checking that both results have the same dataset SHA-256, replay order, clock, bus policy, event count and, per row, the same source line, symbol, time, type and prices (`compare.alignmentProblems`; every input field of a row is checked: source line, symbol, time, type, price, open, high, low, volume, each covered by a test). **Never timestamp alone**: equal timestamps on different symbols stay different rows. A bus sequence is each run's own numbering and is not compared. The two replays must also have run on the **same executable** (the SHA-256 each answer reports): the gateway looks the executable up per replay, and one comparison id names one engine.

### 6.2 What is shown

* **Aligned charts.** One symbol at a time (the display filter, else the selected event's symbol): A above B, then a ribbon (A's requests, B's requests, "A vs B": agree / requests differ / not comparable), all on the same horizontal axis (that symbol's rows in recorded order), the same width and a shared hover. Each side has its **own price scale**; for mean reversion the **z-score is a separate panel** with its entry and re-arm bands. A is lavender with solid and dashed lines, B is orchid with dotted and dash-dot lines, each chart carries its letter, so colour is never the only cue. Past 600 revealed rows per-row marks are thinned (requests and differences stay).
* **One shared cursor.** Previous, Next event, Next signal (either side), **Next difference** and a slider, all moving one position for both sides and following the display filter. "A difference is": *requests differ* (the sides asked differently) or *requests or verdicts differ* (also where one side evaluated and the other did not).
* **Event inspector.** The selected row side by side: each side's plain-language explanation, then verdict, reason code, request, window (readiness), every indicator and the state, with "same / differs / n/a" in words. A field only one side reports (an indicator of the other strategy kind) is "n/a", not a disagreement. Both sides' scoped requests, and identifiers (bus sequences shown per side, labelled as not used to join).
* **Per-symbol warm-up.** Each side's recorded window per symbol as of the last revealed row of that symbol ("Warming up: 3 of 5 accepted bars (as of event 3)"), read from the window, never inferred from requests.

### 6.3 Signal summaries and "agreement" (the definitions, as shown in the UI)

* A row is **comparable** when **both** sides evaluated it. A row where either side was warming up, ignored or reported no diagnostics is **not comparable** and is never counted as an evaluated "no request".
* A comparable row **agrees** when both made the same request (none, Buy or Buy, Sell or Sell) and **differs** otherwise (including Buy against Sell).
* The figure is "Of **N** comparable rows (both evaluated), **a** agree and **d** differ", with the **denominator** and the number left out and **why** ("A evaluated · B warming up 2; both warming up 2"). With no comparable row it says "not available", never 0% or 100%.
* "Replay so far" (events 1..k under the display symbol) and "Full run" (everything) are separate panels and separate exports. Counts are counts of recorded requests, not a measure of profit: a note says so.

### 6.4 Exports (existing formats; nothing reruns; exact values)

CSV and the ZIP follow the Lab's own Compare formats (same column names where they exist). Every file says whether it is the **visible prefix** (with its event range and symbol) or the **full run**, in the file name and in a column of every row.

| Export | Content |
|---|---|
| Signals CSV (prefix / full) | one row per request, with `member`, `member_label`, `run_id`, the raw `signal_id`, `signal_ref` and the side-scoped `member_signal_key` |
| Events CSV (prefix / full) | one row per recorded event: both sides' verdict, reason, action, window, scoped signal keys, indicators and their unavailable reasons, and the A-vs-B relation |
| Summary CSV, Agreement CSV | per-side counts (including "no diagnostics"); the agreement figures with their definition |
| **Comparison bundle (ZIP)**, full run | `comparison_provenance.json` (both configurations as passed and as run, derived facts, dataset identity and provenance, shared settings, engine file and build, scope, agreement), the four full-run CSVs, and **both runs' raw JSON byte for byte** (so each `provenance.result_sha256` still verifies). STORE-only and no wall-clock time, so exporting twice gives identical bytes; it is not compressed |

## 7. Financial comparison: what the backend can and cannot do

**Determination (checked in the source and in the browser):** the existing backend **cannot** produce comparable execution or portfolio results.
`TradingEngine::run_backtest`, `HistoricalReplay::run` and `RiskManager::start/check` throw `NotImplemented`, `apps/trading_engine_main.cpp` only proves linking, and nothing in the repository serves the
ADR 0006 engine API. The Console's **Backtests** therefore run on its **in-browser demo simulator** over its own synthetic market data, not over the Lab dataset, and say so ("Demo data").

So Compare is a **complete signal comparison and nothing more**. The panel under it says this plainly and does not show returns, P&L, drawdown, fees or trade counts for A and B (they do not exist; computing them from the requests would be invention), and
adds no second portfolio engine. What it provides instead:

* **A backtest handoff per side**: "Use A / B in new backtest" pre-fills the existing New backtest form with that one configuration. One strategy per backtest, so neither competes for shared capital and portfolio, risk and execution state are independent by construction. What the handoff says and does:
  * **Where it leads is said next to the button and again on the form, before anything runs**: the New backtest form runs on the console's in-browser demo simulator over its own synthetic market data (or on the connected engine API's own data) and **does not replay the Lab dataset**, which is named; what it reports cannot be tied to the signals seen in Strategies (`backtestHandoff.destinationText`).
  * **What is carried, and what is not, is listed**: the strategy's kind, id, symbols, shares per signal and its parameters travel as typed; the dataset and its time range, the replay settings and the other configuration do not, and the form keeps its own period, capital, fees, slippage, latency and risk limits (its defaults), which the person is told to review.
  * **Nothing is silently changed or dropped**: the button is disabled, with the reason, for a quantity, window or lookback above 2^53-1, a decimal the form reads as `NaN` (`2.`, `1e-1`: the Lab accepts them, the form's reader is `PLAIN_DECIMAL`, shared by both), a strategy id with surrounding spaces (the form trims), a parameter the form has no field for (the engine's catalog is checked, so a future setting is refused rather than dropped), a symbol the Console has no data for, a rejected draft or an unsupported strategy.
  * **Which settings, draft or result**: a side's own editor button and Configure's carry **the settings in the editor now**, and say so. Under the comparison, once a side has been edited after the run, two buttons appear: **"as edited"** (the editor now) and **"as compared"** (the parameter text the comparison on screen actually ran, `draftFromRequest`), with the difference listed ("Short window: 2 -> 1"). The form's notice names which one it received.
* **Guidance**: same period, starting capital, fee/slippage model, risk limits and execution settings; allow for each side's different warm-up (it shows A's and B's window sizes) and state the scored period and any pre-roll; only information available at each event.
* **Recorded backtests side by side**: pick any two completed Console backtests. A checklist compares period, symbols, starting capital, fee model, slippage, latency, participation cap and risk limits (decimals by value, not spelling), and the runs are labelled **"Not comparable"** with the differences listed, or, when every setting matches, **"Unverified · settings match"** (a neutral badge, never green): the engine API records **no fingerprint of the market data**, so comparability can be ruled out but never established, and the line under the badge says the runs are "not shown to be comparable" (and that demo runs are a demonstration). The figures use the **Report page's own definitions** (`lib/reportFigures.ts`, now also used by Report): net return, max drawdown, completed trades, win rate, fees, volatility, Sharpe and profit factor, **net P&L = ending equity − starting capital** in exact decimal arithmetic, and equity and drawdown curves. Sharpe (needs 3+ sessions), profit factor (needs a losing trade) and win rate (needs a closed round trip) are dashes **with the reason**, never zero. A run that traded several strategies is flagged: its figures are portfolio-wide and **no per-strategy P&L is derived** from symbol positions. No winner is named or chosen.

## 8. Advanced: validation and traceability

* **Scenario validation** reuses the Lab's own scenarios (`tests/fixtures/strategy_lab/scenarios`, expectations derived by hand) and runner. It runs **only on "Run validation"** (opening the page, reading the file list and the demonstration checkbox start nothing). It shows the passed / failed / error counts (the number of scenarios is read from the files, never a constant), the run's start and finish time, the **executable** (name, SHA-256, size, tool, version, compiler, build), the **scenario-file identity**, and per scenario its dataset SHA-256 as given to the tool, run id and result digest, with **expected-against-actual** tables for any mismatch (failures and errors open by default; an error is "could not be carried out", never a pass or a strategy failure). The opt-in demonstration (two deliberately wrong expectations, must fail) is separate and not counted. The report is downloadable exactly as the service sent it.
* It says on the page that these are **scenario checks, not the repository's CTest/GoogleTest suite** and that passing them says nothing about that suite.
* A result is **never shown as validating a newer binary or different files**: if the executable (SHA-256) or the scenario-file identity changes afterwards (the engine is re-checked on window focus; the scenario files when the list is re-read with "Check the files again"), the result is badged "Does not describe the current build or files" and kept as a record only.
* **Traceability** lists the service, the executable in use, the comparison on screen (ids, run ids, configuration ids, result digests, whether the engine was rebuilt since) and the last validation (whether it was the same executable), and the number of engine launches per feature in this tab.

## 9. What was checked (2026-10-09 and the 2026-10-10 integration review, this machine)

| Check | Result |
|---|---|
| `tsc -b` (web) | clean |
| `vitest run` (web) | **327 pass**, 22 files (182 before Compare; 295 before the 2026-10-10 review). New: Compare model 26, store 19 + run lifecycle 18, exports 10, validation 15, recorded runs 10, ZIP 5, **real engine through the real gateway 10** (skip with a reason if the executable or Python is missing) |
| Python suite from `python/strategy_lab` (`.venv`): `unittest discover -s tests` | **480 pass** (464 before Compare; 473 before the 2026-10-10 review: +7 request-body tests in `test_gateway_bodies.py`) |
| `vite build` | succeeds |
| Real-engine expectations | hand-derived: SMA 3/5 on the documented fixture asks Buy at event 6 and Sell at event 9 (SMA 2/3: events 5 and 8), so of 5 comparable rows 1 agrees and 4 differ and 4 rows are left out; the two-strategy, two-symbol scenario `10_interleaved_symbols_two_strategies` matches row by row; equal-timestamp, constant-price, insufficient warm-up, invalid CSV, rejected configuration, different-dataset refusal |
| Mutation check | 28 deliberate defects in the Compare / validation / ZIP / recorded-run logic (alignment ignoring a field, a one-sided "comparable", an unscoped signal key, late answers accepted, a duplicate launch, one finished side shown as a comparison, stale ignoring the dataset or a rebuilt engine, prefix export holding the full run, a re-serialized raw run, a wrong CRC, a traversal name, decimals compared as text, ...) all killed (one needed a new race test); 5 gateway defects all killed. Sources restored byte-identical |
| Real browser (Chrome via Playwright MCP, 1440 / 1280 / 1000 / 390 px, dark and light) | Setup, quick starts, run on both reference strategies, aligned charts with separate z-score panel, shared hover and tooltips, Next difference, the inspector, keyboard (arrow keys on a chart, focus ring, named chart regions), stale banner after an edit with the result kept, double click = exactly two replays, stepping/tabs/filter = none, partial failure (a simulated refusal of one side), a 20,000-row upload (about 1.5 s for both sides; slider jumps 0.05-0.35 s), the ZIP built in the page (`PK` header, 27,894 bytes for the 9-row fixture), recorded runs flagged "Not comparable", Advanced: validation (all passed, then with the demonstration showing expected vs actual), Use B in new backtest (windows 3 and 5 arrive), presets saved in Configure and loaded into B only, no horizontal overflow at 390 px, no console errors besides the simulated 422. Overview, Orders, Risk, Report (figures unchanged), System, Backtests, New backtest, Configure and Explore unaffected |

### Integration review, 2026-10-10 (findings and fixes, most important first)

| Finding | Fix | Evidence |
|---|---|---|
| **The backtest handoff did not say where it leads.** The form never mentioned that it runs the in-browser demo simulator and does not replay the Lab dataset | Said next to every handoff button (Configure, each editor, the Financial panel) and on the form itself, with the dataset named, plus what is and is not carried | Real-browser check at 1440 and 390 px, dark and light; `backtestHandoff.test.ts` |
| **A settings match was shown as a green "Same settings"**, although the API records no data fingerprint | Matching runs are "Unverified · settings match" (neutral); only differing runs are ruled out ("Not comparable") | Browser check with two runs of identical settings; `recordedCompare.test.ts` |
| **The handoff could change a value without saying so**: the Lab accepts `2.` and `1e-1`, which the form reads as `NaN`; whole numbers above 2^53-1 were checked only for the quantity; a padded id was trimmed; a parameter the form lacks would have been dropped | Each is refused at the button with its reason; the form's decimal reader (`PLAIN_DECIMAL`) is now the one the handoff checks against | 11 new tests; 6 deliberate defects killed |
| **It was unclear which settings a Compare handoff carried** once a side was edited after the run | The button states its basis; after an edit it splits into "as edited" and "as compared" (the settings the comparison actually ran) | Browser check: "as compared" delivered short 2 / long 3 while the editor said 1 |
| **A refused POST could be lost, and a body could hold a thread forever.** A refusal raised inside the body reader (wrong content type, oversize) was not drained (25 of 40 requests lost their status on Windows); an incomplete body waited with no limit; a trickled body held the old drain open (its timeout was per read); a **truncated body that happened to be valid JSON was processed as if complete** | One bounded, total-time body reader (`gateway_body.py`), drained in `_send` so no answer goes out with an unread body; `Connection: close` | Reproduced before the fix (scripted), then 7 new tests; 7 deliberate defects in the body code all killed |
| **Timestamps sorted as text** in Compare's and Explore's tables, which orders `...:00Z` after `...:00.5Z` | `timeKey` pads the fraction to nine digits | Unit test, and the Events table of an upload with 0, 1, 2 and 9 fractional digits sorted chronologically in the browser (Compare and Explore) |
| **Two replays on different executables would have been paired under one engine identity** (the gateway looks the executable up per replay) | `buildCompareModel` refuses it | Test |
| Alignment tests covered only `price` and `symbol`; one message read "the replay replay order differs" | A test per aligned field, the clock, the order and the bus fault; message fixed | 11 new tests |
| The temp-directory test failed whenever an unrelated `strategy_lab_ui_*` directory existed (and could race a running lab) | It judges only the directory its own run made, in a private base, with an unrelated one planted beside it that must survive | Passes with two planted decoys in the real temp directory; the decoys were untouched and removed by me |
| The Report refactor had no test | `reportFigures.test.ts` pins every headline figure to the page's original inline definitions | 6 tests |

Checked and found sound (no change): the alignment check (dataset SHA-256, clock, order, bus fault, count and every input field of every row; the parser already forces `index` to equal the position); a launch captures its request before any await, so edits during a flight cannot attach to a newer draft, and exports read the snapshot, never the editor (now pinned by a test that edits both sides and the dataset while both replays are out, then checks the stored request, the stale reasons and the exported provenance; 2 deliberate defects killed); no `Number()`, `parseInt` or `toFixed` touches a price, quantity, id or timestamp outside chart coordinates; the standalone Streamlit Lab (documented launcher, port 8612): Explore (Buy at event 5, `crossover_buy`), Compare (two SMA variants, two runs) and Validate (18 passed) all work against the current executable.
Targeted mutation check of this review's web fixes: 15 deliberate defects, 14 killed; the survivor (a table column's `sort:` left as raw text) is wiring that node-only tests cannot reach and was verified in the browser instead.

## 10. Limitations

* **MSVC and Windows only** were available: GCC/Clang and POSIX were not run. Chrome (Playwright) was the only browser; no screen reader was used (the charts have text alternatives and the controls are keyboard-operable, which is not the same as tested).
* **No financial comparison** (section 7). The recorded-run inspector reads the Console's engine API, which today means the in-browser demo simulator; it was exercised only against that. Nothing was run against a real engine API because none exists here.
* The gateway is a second process and a production build has none. Replays are signal-only through the lab's synchronous bus: no risk, execution, portfolio, timing or backpressure. Results are not kept across a page reload (presets are). Uploads are capped at 8 MiB and 20,000 rows.
* Compare charts one symbol at a time, with no zoom or pan; the SVG chart is not lightweight-charts (which needs unique integer-second times). The agreement figure treats Buy and Sell as the only requests; a row where one side asks for both is compared by its set of sides.
* The bundle is not compressed (deterministic, but about the size of the runs); ZIP64 is not supported and a bundle that would need it is refused.
* The "Crossover vs mean reversion" quick start uses the dataset's example parameters or the engine's defaults; on a dataset without examples the symbols box starts empty.
* A browser logs every non-2xx response as a console error, so a refused configuration or an invalid CSV shows one there even though the console handles it.
* The backtest handoff states where it leads, but it cannot make the demo simulator replay the Lab dataset; that needs the engine API (section 11). The "as compared" handoff rebuilds the draft from the parameter text that was sent, so it is exactly what ran, not what the engine derived from it.
* A request body declared above 12 MiB, or sent chunked, may leave the client with a connection reset rather than the explanatory `413`/`400` (section 3). The gateway is for one local console, not an internet-facing server.
* The component tests are node-only (no DOM): the wording is pure functions and is tested, while the wiring of a component (a button's text, a column's sort key) was verified in the browser.

## 11. Possible follow-ups

* A real backend path (the engine API of ADR 0006 served by the C++ engine) would let Compare run each side as an independent backtest over the same data snapshot; the handoff and the recorded-run checks are written so that connecting it needs no UI change.
* `bridge.run_replay_multi` could run both sides in one engine run; it was not used because it shares one signal-id space and one bus, which is exactly what Compare keeps separate.
* A "pre-roll" control (scoring only after both sides are warm) would be a real feature, not a label; it needs an agreed definition first.

## 12. Reviews required before merge (NOT done)

Per [COMPONENT_OWNERSHIP.md](COMPONENT_OWNERSHIP.md), none of these has happened:

| Changed | Required review |
|---|---|
| `web/` (Strategies section incl. Compare and Advanced; **shared files**: `NewBacktest.tsx` pre-fill, notice and `PLAIN_DECIMAL`, `Report.tsx` figures moved to `lib/reportFigures.ts`, `TimeChart.tsx` second tone, `Shell.tsx` navigation, `icons.tsx`, `main.tsx` routes, `vite.config.ts` proxy and preview block, `package.json` `lab` script) | the console's owner (Adam, who wrote `web/` and ADR 0006) + one other. `COMPONENT_OWNERSHIP.md` does not name `web/`, so this follows the git history |
| `python/strategy_lab/strategy_lab_ui/gateway.py`, `gateway_body.py`, `gateway_validation.py`, `lab_gateway.py`, `tests/test_gateway.py`, `tests/test_gateway_bodies.py`, `tests/test_bridge.py` (temp-directory test), and the **shared** `bridge.py` helper (`describe_with_output`, used by the Streamlit Lab too) | area C (strategy; the Lab is a strategy tool) + one other |
| This file, the one-line pointer in `docs/STRATEGY_LAB.md`, `web/README.md` | the docs' maintainer (A, per `COMPONENT_OWNERSHIP.md`) |

No C++, CMake or `domain/` file was changed, so none of those approvals apply.
