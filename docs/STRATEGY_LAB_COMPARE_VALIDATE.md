# Strategy Lab: Compare, Validate and the dark theme

> Status (2026-10-02): implemented and checked on this machine only; **nothing is committed or pushed and no ownership review has happened**
> (section 8). This continues [STRATEGY_LAB.md](STRATEGY_LAB.md) (the C++ tool, its JSON and the Explore view; section numbers below
> that read "S14.x" refer to it). The Strategy Lab is still **signal-only**: it shows the requests the real strategies emit, never fills,
> returns, P&L or a portfolio, and nothing here ranks one configuration above another.

## 1. Three views, one switch

The title row has three pills: **Explore** (one strategy, step through its decisions), **Compare** (two configurations, one dataset) and
**Validate** (scenario checks). Only the chosen view is drawn.

| What | Rule |
|---|---|
| Engine launches | Exactly three places, each behind its own button: `session.launch` (Run replay), `compare_session.launch` (Run comparison: two processes) and `validate.run_all` (Run all scenarios). Switching views, stepping, the slider, display filters, tabs, downloads and editing settings start nothing |
| Each view's state | Its own session keys (`ui_*`/`lab_*`, `cmp_*`, `val_*`): a dataset or strategy chosen in one view never changes another |
| Settings survive a visit elsewhere | Streamlit forgets a widget that was not drawn in a run. `persist.py` restores each value before the widget is created and remembers it afterwards. Results, the replay position and the display filter live in plain session state and were never at risk |
| Uploads survive too | A file-uploader widget cannot be restored, so the uploaded name and bytes (at most 8 MiB per view) are kept in session state. Arriving from another view finds the uploader empty and keeps the file ("Using the file uploaded earlier ...", with *Forget this file*); pressing the uploader's own remove button, while still in the view, drops it |
| Shared pieces | `playback.py` (Reset, Previous, Next bar, Next signal, display symbol, slider), `datasets_ui.py` (dataset chooser), `charts.py` (figures), `bridge.py` (the only place a process starts) |

## 2. Dark theme

`python/strategy_lab/.streamlit/config.toml` pins `[theme] base = "dark"` plus the colours below, so every view and every new browser session
starts dark; the operating system's light/dark preference does not change it. The same values live as constants in
`strategy_lab_ui/theme.py`, which also holds the custom CSS and the chart palette; `tests/test_theme.py` checks that the config file and
the constants agree and that every text/background pair meets WCAG 2 contrast. There is no light/dark toggle (not required).

| Role | Colour | Role | Colour |
|---|---|---|---|
| Page | `#0E1117` | Primary text | `#E6EDF3` |
| Sidebar, cards, plot area | `#161B22` | Secondary text | `#A8B3C2` |
| Raised (hover labels, selected pill) | `#1F2630` | Accent, primary button fill | `#60A5FA` |
| Border / chart grid | `#30363D` / `#262D38` | Buy / Sell / Warning | `#4ADE80` / `#FB7185` / `#FBBF24` |

* **Primary button.** White on the accent measured **2.54:1**, so the filled button carries dark text on the accent (**7.43:1**).
* **Sticky action bar** (Restore defaults / Run ...): drawn in the sidebar colour with a top border and shadow, pinned at the bottom of the
  scrolling panel; scrolled to its end, the last widget sits above it (checked at 1024x768 and 1440x900).
* **Charts** (Plotly `plotly_dark`, no Streamlit theme): backgrounds, axes, grid, legend, hover labels and toolbar follow the palette. Buy is a green
  triangle-up labelled "Buy", Sell a rose triangle-down labelled "Sell" (shape and text, never colour alone); warming-up bars are open circles,
  ignored bars an x, trade rows open diamonds; the short average is a solid blue line, the long average dashed amber, the rolling mean dash-dot teal,
  the z-score violet with markers; entry thresholds dashed amber and re-arm thresholds dotted grey, each labelled; the selected event has a white ring.
* **Keyboard focus:** a 2px accent outline on buttons, inputs, selects, tabs, the slider and links (every focusable control tried showed it).
* **Starting it elsewhere:** Streamlit reads `.streamlit/config.toml` from the directory it is started in. `run_lab.ps1` starts it from
  `python\strategy_lab`, refuses to start if the file is missing, prints the config path it will use, and warns if a `STREAMLIT_THEME*`
  environment variable would override it. Started from another directory, the page shows a notice instead of silently looking different.

## 3. Compare

**Setup (left panel):** one shared dataset (built-in or upload) and **one shared symbol allowlist**, a quick start (*SMA vs mean reversion*, *Two SMA
variants*: demo parameters, not recommendations), then Configuration A and B (strategy and parameters from the same catalog and the same defaults as
Explore; the runner stays the only validator). `strategy_id` and `requested_quantity` use the strategy defaults. Each configuration's label is built
from its exact settings, for example `A: Moving-average crossover (short_window 2, long_window 3)`.

**One explicit action, two independent runs.** *Run comparison* starts the existing `bridge.run_replay` twice (A, then B): separate processes, fresh
engines and strategy state, the identical dataset bytes, the same lab bus, clock policy and replay order. A multi-strategy run in one engine was not used
because the point is isolation. Both are always attempted, so one message can name every configuration that failed.

| Rule | Behaviour |
|---|---|
| Pairing | `check_aligned` refuses a pair whose dataset hash, replay clock/order/fault or event sequence differ (`comparison_mismatch`) |
| Failure | If either run fails, **no comparison is produced**: the error names the configuration (the tool's own message and problem rows), says the other finished but is not shown, and the **last completed pair** stays on screen labelled as such. A new result is never paired with an old one |
| Stale | The snapshot (`CompareRequest`) holds the dataset identity, both configurations and the executable's hash. The banner names what changed, per side ("B: parameters (long_window)", "shared symbols allowlist", "replay executable was rebuilt or replaced") |
| Identities | Signal ids count 1, 2, 3 ... in **every** independent run, so they collide. Raw ids are kept untouched and every join/export adds the member and the run-scoped reference: `member_signal_key = A/<run_id>:<id>`. Two identical configurations even share a `run_id` (a content hash); the member keeps them apart |
| Shared cursor and filter | One cursor counts events in the recorded order (equal timestamps and interleaved symbols stay distinct) and one display-symbol filter applies to both sides. **Next signal** stops at the next event, after the cursor, where *either* configuration requested something (one stop when both did) |
| Backward replay | Removes the future from both sides: charts, counters, tables and the selected event |
| Readiness | Read from each strategy's *recorded window* (full or not), as of the latest revealed row of the symbol that reports one. It is never inferred from signals: a ready strategy may emit nothing. Unavailable values are named with their reason; "not reported by this strategy" is shown where a metric does not apply |
| Selected event | Both decisions side by side (verdict, reason, outcome, window, every indicator or its unavailable reason), a "Differs" column, all signal requests on that event, and a sentence that only describes them (both same side, opposing, only A, only B, neither) |

**Display.** One figure with shared x axes (zoom or pan moves both): for each shown symbol (at most 2 at once; choose a symbol for others) each
configuration's price panel and, for mean reversion, its z-score panel. Legend entries for shared marker kinds appear once and toggle both sides; each side's indicator lines
carry its prefix. Counters per configuration, readiness per symbol, signals and a side-by-side diagnostics table in tabs.

**Exports** (each says whether it covers the FULL RUN or the VISIBLE PREFIX, on every row and in the file name):
`Download comparison package - FULL RUN (zip)`: `comparison_provenance.json` (dataset identity, both configurations as passed and as run, run ids, result hashes,
executable name/hash/size/tool/compiler, scope, no local paths), **both original runner results byte for byte** (their own `result_sha256` still verifies),
`comparison_summary_full_run.csv` and `comparison_signals_full_run.csv`; the zip is deterministic. `Signals CSV - VISIBLE PREFIX` and
`Summary CSV - VISIBLE PREFIX` cover exactly what the cursor and filter show. Counts are counts of recorded requests, with no rank, winner or quality column.

## 4. Validate

**What it is.** It runs the checked-in scenario files against the **real executable** and compares each answer with an expectation **derived by hand**
from the strategy documents and the fixture bars. **It is not the C++ CTest/GoogleTest suite**, and passing every scenario does not show that the repository's
tests pass; the page, the report and this document say so.

**Scenario files:** `tests/fixtures/strategy_lab/scenarios/NN_<id>.json` (18 files; schema `strategy_lab.scenario/1`) over `datasets/*.csv` and
`invalid/*.csv`. Each has: identity, purpose and `covers` tags; the dataset (path and the SHA-256 of its **LF-normalized** bytes, so a CRLF checkout still
matches); the strategies with their exact parameters; the expected outcome; and `provenance` (derived by hand, sources, the step-by-step derivation, independent
cross-checks in `tests/unit/strategy_lab_replay_test.cpp` and `tests/strategy_lab/test_reference_agreement.py`). **No expected value was generated from the
implementation under test**: irrational values (sqrt 3, sqrt 5 ...) were computed in exact arithmetic (`Fraction`/`Decimal`) from the fixture bars. The loader
refuses an unknown key (a misspelt key must not switch an assertion off), a missing derivation, a path that leaves the fixture directory and duplicate JSON keys.

| Check (examples) | Scenarios |
|---|---|
| SMA signal timing; baseline never signals | `sma_crossover_timing` (Buy bar 5, Sell bar 8), `sma_whipsaw_flips_every_bar` (first signal at long_window + 1) |
| Mean-reversion thresholds and re-arming | `mr_rearm_and_flip` (Buy, re-arm, Buy, direct flip to Sell) |
| Repeated-trigger suppression | `mr_repeated_trigger_suppressed` (a z beyond the entry on the requested side repeats nothing; constant window re-arms) |
| Constant-price windows | `constant_price_windows` (equal averages, constant window; z unavailable, never 0) |
| Insufficient warm-up and its boundary | `warmup_sma_insufficient`, `warmup_mr_one_bar_short`, `warmup_mr_first_full_window_signals`, `warmup_sma_first_comparison_is_baseline` |
| Interleaved symbols, simultaneous signals, ids | `interleaved_symbols_two_strategies` (8 signals in publish order) |
| Equal-time ordering | `equal_time_aapl_first`, `equal_time_msft_first` (file order, not the alphabet) |
| Ignored rows | `trade_rows_are_ignored` |
| Dataset and configuration rejection | `rejects_non_numeric_price`, `rejects_bar_time_not_increasing`, `rejects_utc_offset` (line and column), `rejects_equal_sma_windows`, `rejects_mean_reversion_lookback_one` (exit status 2, `config_error`) |

**Numbers.** An expected value is an exact fraction. `exact` must be a value a double represents exactly (the loader refuses 1/3); everything else is
`approx` with a **named tolerance declared in the same file with a written justification** (1e-12: far above an ulp of a value below 40, far below any difference that
could change a decision). The comparison uses exact rational arithmetic. A value expected to be unavailable must be **absent with the documented reason, never 0**; a
number expected where the tool reports "unavailable (reason)" is a failure that says so.

**Classification** (`validate.py`):

| Status | When |
|---|---|
| **Passed** | The tool answered and every expectation matched. A rejection scenario passes only on the **expected** rejection: exit status, error code, problem lines/columns and message text |
| **Failed** | The tool answered coherently and differently: wrong signal, number or warning; it accepted what should be refused; it refused what should run; wrong refusal details |
| **Error** | The check could not be carried out: executable missing or unstartable, timeout, crash or any exit status the tool never uses, malformed or inconsistent output, an internal error (exit 4), or a dataset file that is not the one the scenario was written for. **Never a pass, never "an expected rejection"** |
| Not run / Running | Before the button; while it runs (progress shown) |

**Running and retaining.** Only *Run all scenarios* starts anything (one process per scenario, a second or two in all). Leaving and returning shows the retained results without
rerunning. Every result carries the dataset bytes' SHA-256, the pinned fixture hash, the configuration, the executable's SHA-256, the `run_id` and `result_sha256`; the page labels
results from a different executable or different scenario files as older.

**Showing a mismatch.** *Also show a deliberate mismatch (demonstration)* adds one in-memory scenario (`injected_mismatch`: the first Buy expected one event late and one average
half a unit too high). It must **fail**, is shown apart and **is not counted** in the totals; if it ever passed, that would be reported as an Error (the comparison itself would be broken).
It is never a file, so no failing scenario sits among the passing ones.

**Report.** `Download validation report - JSON` (authoritative) and `- Markdown`: scope statement, executable identity, scenario-file identity, per-scenario dataset/configuration/expected/
actual/provenance/tolerances, mismatches, errors, the demonstration reported separately. Both are deterministic for a given run.

## 5. Commands and prerequisites (Windows 11, PowerShell, from the repository root)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\setup.ps1    # once: venv with streamlit 1.64.0 and plotly 7.1.0 (needs internet this once)
powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\run_lab.ps1   # http://127.0.0.1:8501 ; options -NoBrowser -Port -Exe
cd python\strategy_lab; .venv\Scripts\python.exe -m unittest discover -s tests         # 433 tests, about 90 s
```

The replay tool must be built first ([STRATEGY_LAB.md](STRATEGY_LAB.md) section 4; the two `cmake` commands, no GoogleTest needed). **Offline:** after setup nothing is downloaded; the page loaded 173
resources, all from `127.0.0.1:8501`. No C++ file was changed in this stage, so no rebuild was needed: the executable used is `out\strategy_lab\bin\Release\strategy_lab_replay.exe` (SHA-256 `5400b3c8...`, built earlier the same day).

## 6. What was run (2026-10-02, this machine only)

| Check | Result |
|---|---|
| Python suite (`unittest discover -s tests`) | **401 tests passed at this stage** (433 now: the int64 price/quantity change added 3 schema tests, its review 4 exactness tests and the exact-order control 25 tests; 186 before this stage; new: theme 22, Compare core 36, Compare exports 13, Compare app 24, scenarios 29, Validate classification 36, Validate against the real exe 17, Validate app 19, navigation 19; no existing test file was edited, `tests/support.py` gained shared helpers) |
| Scenarios against the real executable | 18 of 18 pass. A deliberately wrong expectation injected into **each** scenario (first signal one event late, an expected signal that does not exist, a wrong refusal code) is detected as Failed in every one |
| Existing process-level tests against the same executable | `test_reference_agreement` + `test_cli_contract`: 38 pass (the JSON-probe module was not run: no probe binary was built) |
| Targeted mutation check (17 deliberate defects, one at a time, each file restored byte for byte) | all **17 killed**: the other member's event read for a decision, next-signal ignoring B, scoped key dropping the member, pair-alignment off, a failed run shown half-paired, readiness from signals, a stale side missed; a timeout/crash treated as a rejection, tolerance ignored, unavailable allowed to be zero, a short signal list passing, a passing demonstration accepted, fixture pin unchecked, any exit status accepted for a rejection, the zip no longer carrying raw bytes, the prefix export covering the full run, the report counting the demonstration |
| Defects the new tests or the browser found, now fixed | the Compare model read B's decision from A's event object; switching the dataset source from Upload back to Built-in raised a `KeyError` in a callback (it predates this stage); the uploader's remove button left the kept copy in use; white text on the primary button (2.54:1); a status chip cut off at 1024px; a long tooltip spilling over the page; parameter labels breaking mid-word |
| Not run | C++ builds and tests (no C++ changed), GCC/Clang/POSIX, Python 3.10-3.12 (only 3.13.0), other browsers |

**Browser verification** used the Playwright MCP (`mcp__playwright__*`: `browser_navigate`, `browser_click`, `browser_find`, `browser_file_upload`, `browser_resize`, `browser_evaluate`, `browser_take_screenshot`, `browser_console_messages`,
`browser_emulate_media`, `browser_run_code_unsafe` for multi-step flows and a fresh isolated context) against the real launcher: Chrome 154 for Windows, **1440x900 and 1024x768**.

* **Fresh context** (empty storage, `prefers-color-scheme: light` emulated): the first paint is dark (page `rgb(14,17,23)`, sidebar `rgb(22,27,34)`).
* **Explore** SMA and mean reversion: the Plotly traces were read back and equal the hand-derived values (SMA 2/3: closes 3 2 1 2 3, short 1.5 1.5 2.5, long 2 5/3 2, Buy on event 5, Sell on event 8; mean
  reversion on `mr_rearm`: means 9, 8.75, 6.75, 11.75, z -1.7320508, 0.1524986, -1.5261167, 1.6858628, Buys on events 4 and 6, a Sell on 7, the four labelled threshold lines). Stepping back four events removed the Buy and the Sell.
* **Compare**: both types. SMA vs mean reversion; two SMA variants (B: short 1/long 2: Buy event 4, Sell event 7; the combined Next signal stopped at events 4, 5, 7, 8; both panels always held the same number of points). Presentation interactions,
  a round trip through Explore and back left the finish time and the launch counter unchanged (no rerun). On the two-symbol fixture with equal timestamps the combined stops were events 8, 9, 12, 14, 15 and,
  filtered to MSFT, 8, 12, 14 (equal to the hand derivation); the filter did not move the cursor, both configurations' panels shared one x sequence, and again nothing reran. A pending edit showed "B: parameters (long_window)" with the old pair kept; an invalid B showed the one-sided failure with the last completed pair retained.
* **Validate**: 18 passed with real results, the expected refusals shown as refusals, and the demonstration shown as a failed check (expected 5 / actual 4; expected 2 / actual 1.5).
* **Uploads** through the real file chooser (`browser_file_upload`): a valid CSV (its SHA-256 on the page equals `sha256sum` of the local file), an invalid one (line 3, column `price`), in Explore and Compare.
* **Downloads** completed as saved files (Playwright's download event) and their bytes were inspected: the Explore run JSON (LF only, its `result_sha256` verifies), both Explore CSVs (scope on each row), the Compare package
  (valid zip; both raw results byte-exact and verifying; provenance without local paths), both Compare prefix CSVs, and both validation reports.
* **Readability and layout**: measured contrast on the live page (text 15.99:1, primary button 7.43:1, secondary 13.87:1, disabled buttons 6.58:1 effective at 62% opacity, alerts 11.8-13.3:1, labels 14.6:1, tooltip 15.99:1), keyboard focus outlines, no horizontal overflow, the sticky bar pinned at both widths, the status chip fully visible at 1024px.
* **Console:** no error or warning messages across a complete tour of the three views (the "errors" the pane reported earlier were the page losing its connection while I restarted the server).
* Screenshots: `out/strategy_lab/screenshots/` (git-ignored): `explore_`, `compare_`, `validate_` with `1440` or `1024` (plus `_lower` and `_mismatch`), and `00_before_light_explore.png` (the light theme before this stage).

## 7. Limitations

* Verified in Chrome 154 on Windows 11 only; no Firefox, Safari, phone or tablet width, screen reader or all-keyboard session. Validate's **Error** display was exercised in automated tests (timeouts, crashes, malformed output, a missing file), not in the browser.
* Compare shows two configurations of the two available strategies (the ML kind is unavailable). The chart shows at most 2 symbols at once with both panels; datasets are tiny synthetic fixtures, the demo parameters are teaching values, **not recommended trading parameters**.
* Compare uses the strategy defaults for `strategy_id` and `requested_quantity`. Streamlit's `st.dataframe` cells are drawn on a canvas (CSV exports and HTML tables carry the same numbers).
* Validate's expectations cover the cases listed in section 4 only; they say nothing about the other C++ tests, other compilers or production behaviour.

## 8. Ownership reviews (NOT done)

| Changed or added | Required review | Status |
|---|---|---|
| `tests/fixtures/strategy_lab/scenarios/` and `invalid/` (new fixture data; a lab-local schema) | fixtures owner (**B**) informed | **Not reviewed** |
| `python/strategy_lab/` additions (Compare, Validate, theme, launcher check, tests) | next to `python/visualization/` (area **C**): **C + one other** | **Not reviewed** |
| These two documents and the demo guide | the docs' maintainer | **Not reviewed** |
