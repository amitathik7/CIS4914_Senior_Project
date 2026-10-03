# Strategy Lab: demo guide (about 5 minutes)

A walkthrough for presenting the optional Strategy Lab. Facts and limits are in [STRATEGY_LAB.md](STRATEGY_LAB.md) (the tool and Explore) and
[STRATEGY_LAB_COMPARE_VALIDATE.md](STRATEGY_LAB_COMPARE_VALIDATE.md) (Compare, Validate, the dark theme, what was checked). Everything below was
run on the real executable on 2026-10-02; the numbers quoted are what the screen shows.

## Before you start

```powershell
# from the repository root; once: build the replay tool, create the environment (needs internet once)
cmake -S . -B out/strategy_lab -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON
cmake --build out/strategy_lab --config Release --target strategy_lab_replay
powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\setup.ps1
# every time: start the lab (this window must stay open; Ctrl+C stops it)
powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\run_lab.ps1
```

Open <http://127.0.0.1:8501> in a window at least 1024 px wide. The page is dark whatever the operating system's setting is; no internet is needed after setup.
If the launcher refuses ("CANNOT START ...") it says what to do. The data are **synthetic teaching fixtures** and the parameters **demo parameters**, not market data and not recommendations.

## The walkthrough

| Time | Say | Do | What you will see |
|---|---|---|---|
| 0:00 | "The Strategy Lab runs the **real C++ strategies** over recorded bars and shows what they decide. The browser computes nothing." | Explore is open. Dataset *SMA crossover demo (AAPL, 9 bars)*, strategy *Moving-average crossover*, 2 and 3. Press **Run replay** | A dark page; the chart says "Replay position 0 of 9: nothing revealed yet" |
| 0:45 | "Let's step to a real signal and see why it fired." | **Next signal** | *Event 5 of 9*, a green **Buy** triangle. The inspector: reason `crossover_buy`, short SMA 2.5 above long SMA 2, "Buy request #1". Explain: event 3 only set a *baseline* and never signals; the first change of side signals once |
| 1:30 | "Going back removes the future: nothing is peeked at." | **Previous** twice, then **Next signal** twice | The Buy disappears and returns; the second stop is *Event 8*, a red **Sell** (short 2.5 now below long 3) |
| 1:45 | "Now two configurations on the same data." | **Compare**, **SMA vs mean reversion**, **Run comparison**, **Next signal** | A: Buy at event 5. B (mean reversion) is quiet: its z-score peaks at 1.414, below the 1.5 entry. The readiness table says both are *Ready*: **ready does not mean a signal** |
| 2:30 | "Same strategy, two parameter variants." | **Two SMA variants**, **Run comparison**, **Next signal** repeatedly | Stops at events 4, 5, 7, 8: "Only B produced a signal request here (Buy request)". Point at the *Scoped reference* column: both runs number their signals 1, 2, so each is shown as `A/<run>:1`, `B/<run>:1`. Both ran on one dataset (same SHA-256), in separate engines. Counts are not a quality score |
| 3:15 | "Is the program doing what the strategy documents say? Scenarios, derived by hand." | **Validate**, tick *Also show a deliberate mismatch*, **Run all scenarios** | **All 18 passed**. Open *A price that is not a number rejects the whole dataset*: refused at line 3, column `price`, exactly as expected. Open *Where this expectation comes from*: the hand derivation and the independent cross-checks |
| 4:00 | "And this is what a mismatch looks like." | Scroll to *Demonstration* | **Failed**: "signal 1 event_index expected 5, actual 4" and "short_sma expected 2 (= 2.0), actual 1.5". Say plainly: *these are scenario checks, not the C++ test suite* |
| 4:30 | "A signal is a **request**, not a trade." | Point at "Buy request" and "No request (a silent bar is not a hold signal)" in the inspector | Nothing downstream exists yet: no risk check, fills, positions or P&L. The lab is signal-only, on a lab-local bus, not the production queue |

## If you are asked

* **Where is the decision made?** In `src/strategy/` (C++). Python parses the tool's JSON and draws it; every price, average, z-score, marker and reason on screen is a value the tool wrote.
* **How do you know it is right?** Hand-derived scenarios (this walkthrough), an independent exact-rational reference model, the C++ unit tests, and 433 Python tests including a mutation check. Details and limits: [STRATEGY_LAB_COMPARE_VALIDATE.md](STRATEGY_LAB_COMPARE_VALIDATE.md) section 6.
* **What is not claimed?** MSVC only; one browser checked; the synthetic fixtures are tiny; the ownership reviews listed in section 8 of that document have **not** happened; nothing is committed or pushed.
* **Something looks stale?** The banner names what changed; press the Run button. After rebuilding C++, press Run again (a different executable marks old results stale).
