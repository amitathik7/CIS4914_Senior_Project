# Visualization

Charts one finished run from the results file `trading_engine run --report`
writes (schema in
[`results_export.hpp`](../../include/trading_engine/analytics/results_export.hpp)).
Reads only that file, never the database or a running engine.

```bash
pip install -r requirements.txt
trading_engine run --bars tests/fixtures/pipeline/demo_bars.csv --report run.json
python plot_performance.py --input run.json --output charts/
```

Writes `charts/<name>_report.png`: equity with each fill marked, drawdown,
per-sample returns and the summary table. `--show` also opens a window.

Tests: `python -m unittest discover -s tests` (the PNG test skips without
matplotlib).
