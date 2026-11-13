# User guide

A walkthrough of the engine console for people trying it for the first time, for example in a user-testing
session. It covers what each page is for, a set of tasks to try, what is known not to work yet, and how to
report what you find.

> Everything here is **simulated**. The engine replays recorded market data through trading strategies, risk
> checks and a fill simulator. No real orders are placed and no real money is involved.

## 1. Start the system

From the repository root:

```bash
python3 start.py --pace-ms 200
```

If this is the first run on the machine, follow [README: Before the first run](../README.md#before-the-first-run)
first (`python3 start.py --check` tells you what is missing). `--pace-ms 200` replays bars slowly enough that you
have time to watch a run and press Stop or the kill switch; the default (50) finishes a demo run in a few
seconds.

Wait for the line `Everything is up`. The console opens at http://127.0.0.1:5173. If a backtest is refused in
the first second or two with "No engine is listening for commands", the engine has not checked in yet: press
**Run backtest** again.

When you are done, press **Ctrl+C** in the terminal. Your runs are kept for next time; start with
`python3 start.py --reset-kafka` to begin with an empty list.

**Check you are on the real engine:** if the top bar shows a yellow **Demo data** badge, the console is
running its in-browser simulator instead of the engine (it was started without `start.py`). Everything still
works, but the numbers are not from the engine.

## 2. Find your way around

The left-hand menu has two parts.

- **Backtests** lists every run and has the **New backtest** button. Start here.
- The **run pages** show one run at a time. Pick the run with the run selector at the top of the page.

| Page | What it shows |
|---|---|
| **Overview** | Equity curve (this session or all), price chart with fill markers, current positions, recent fills and risk interventions |
| **Strategies** | Try strategy settings on built-in datasets with the real C++ strategy code: Configure, Explore, Compare, Backtest |
| **Orders** | The blotter: every order with its status, plus fills and the raw signals. Click a row to open its trace, from signal to risk decision to fill |
| **Risk** | Usage against each risk limit, the **kill switch**, interventions by policy, and a decision log (approved, resized, rejected) |
| **Report** | The run's results: return, drawdown, trade P&L, the closed trades and the configuration the run used. While a run is replaying it shows progress and a **Stop run** button |
| **System** | Messages per stage, throughput and queue depth on the event bus |

The recorded data `start.py` loads by default is one trading session, **5 January 2026, 9:30 to 11:00 ET**, for
**AAPL** and **MSFT**, in one-minute bars. The New backtest form moves its dates to that session by itself.

## 3. Tasks to try

Each task says what you should see. If you see something else, that is worth reporting (section 5).

### Task 1: Run your first backtest

1. Open **Backtests** and press **New backtest**.
2. Leave the defaults: a moving-average crossover on AAPL, 100 shares per signal, $100,000 starting cash.
3. Press **Run backtest**.

**Expected:** you land on the run's **Report** page. It shows a progress bar while the bars replay, then the
results: equity, drawdown, trade P&L and a list of closed trades. The run appears in the Backtests list.

### Task 2: Follow one order end to end

1. With that run selected, open **Orders**.
2. Click any order in the blotter.

**Expected:** a panel opens with the order's whole life: the strategy's signal, the risk decision, and the
simulated fill with its price and slippage. Close it and look at **Overview** to find the same fill on the
price chart.

### Task 3: Stop a run while it is replaying

1. Start another backtest (Task 1).
2. On the Report page, press **Stop run** while the progress bar is still moving.

**Expected:** the run ends early and reports the bars it replayed so far. If the run finished before you could
press it, restart with a higher `--pace-ms` (for example 500).

### Task 4: Use the kill switch

1. Start another backtest and open **Risk** straight away.
2. Optionally type a reason, then press **Engage kill switch** and confirm.

**Expected:** a "Kill switch engaged" banner appears, and signals from then on are **rejected** in the decision
log. Press **Release kill switch** to let orders through again. The kill switch is only available while a run is
running.

### Task 5: See risk limits at work

1. New backtest: set **Shares per signal** to 500 and, under Risk limits, **Max order size** to 200.
2. Run it and open **Risk**.

**Expected:** the decision log shows orders **Resized** to 200 shares. Then try a **Max position** smaller than
one order's value (for example $1,000) and see orders **Rejected**. Invalid input (a letter in a number field,
a long window shorter than the short window) is refused with the field highlighted.

### Task 6: Two strategies in one run

1. New backtest: press **Add strategy**. The second one is a mean-reversion strategy.
2. Under its Symbols, make sure **MSFT** is selected, then run.

**Expected:** Orders and the Report's trades show both strategy ids; each strategy only trades its own symbols.

### Task 7: Explore a strategy in the Strategies section

1. Open **Strategies**. Choose a strategy (crossover, mean reversion or ML logistic regression) and a dataset,
   for example **Large: regime switching**.
2. Press **Run replay**, then step through the result in **Explore**. Click a row to see why the strategy did or
   did not signal at that bar.
3. In **Compare**, set up two configurations (A and B; **Copy A to B** is a quick start), change one setting on
   B, and press **Run comparison**.
4. Press **Backtest on the real engine** to see the same configuration with risk checks, fills and P&L.

**Expected:** every result carries a "Real engine" badge. Replays and comparisons show signals only; the
Backtest view adds fills, positions, equity and drawdown.

If the Strategies pages say the service or the engine is unavailable, the Strategy Lab tools have not been
built: see [README: Before the first run](../README.md#before-the-first-run).

### Task 8: Take a strategy from Strategies to a full run

1. In Strategies (Configure), set up a crossover or mean-reversion strategy and press **Use in new backtest**.
2. Review the prefilled New backtest form and press **Run backtest**.

**Expected:** the form is filled with your strategy's settings, with a note saying what was carried over
(the strategy) and what was not (dataset, capital, fees and risk limits keep the form's defaults).

## 4. Known limitations (please don't report these)

- **Market data is recorded, not live.** Only the files given to the engine (by default one session for AAPL and
  MSFT) can be backtested. There is no live Alpaca feed yet.
- **ML strategy:** it works in the Strategies section (replay, compare, backtest) but is not offered in the
  New backtest form, and **Use in new backtest** is disabled for it.
- **Latency and participation cap** fields in New backtest are accepted and stored, but the fill simulator does
  not apply them yet: orders fill at the next bar's open plus slippage and fees, in full.
- **System page** latency is the time a message spent on the bus, not each stage's processing time.
- Runs live in the Kafka broker. `--reset-kafka` (or removing the Docker container) deletes them; there is no
  database yet.
- In the first second or two after start-up, New backtest can say "No engine is listening for commands". Try
  again.

## 5. Report what you find

For each problem or confusing moment, note:

- **What you were trying to do** (for example "Task 4, engage the kill switch").
- **What you did**, step by step, and the run name or run id (shown on the Report page).
- **What you expected** and **what happened instead**, with a screenshot if you can.
- Anything printed in the terminal in red, or the matching file in `out/logs/` (`engine.log`, `gateway.log`,
  `console.log` or `lab.log`).

Also worth writing down: anything you could not find, any wording you didn't understand, and anything that took
longer than you expected.

## 6. Troubleshooting

| Problem | What to do |
|---|---|
| `start.py` says no `trading_engine` build was found | Build it once: [README: Before the first run](../README.md#before-the-first-run) |
| "Docker is not running" | Start Docker Desktop and run `start.py` again (on Windows it tries to start it for you) |
| "port 5173 is in use" | A console (or an earlier `start.py`) is still running. Close it, or press Ctrl+C in that terminal |
| "No engine is listening for commands" | Wait two seconds and press Run backtest again. If it keeps happening, look at `out/logs/engine.log` |
| The **Demo data** badge is showing | The console was started without the engine. Close it and use `python3 start.py` |
| Strategies pages say the service is unavailable | Build the Strategy Lab tools (README), and make sure `start.py` was not run with `--no-lab` |
| A run finishes too fast to stop | Restart with `python3 start.py --pace-ms 500` |
| Old runs clutter the list | Restart with `python3 start.py --reset-kafka` |
