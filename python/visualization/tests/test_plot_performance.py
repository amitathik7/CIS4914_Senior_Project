import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import plot_performance as pp  # noqa: E402

HAVE_MATPLOTLIB = importlib.util.find_spec("matplotlib") is not None


def results(**overrides):
    doc = {
        "schema": pp.SCHEMA,
        "name": "te.bt-demo",
        "mode": "backtest",
        "source": "demo_bars.csv",
        "symbols": ["AAPL"],
        "summary": {"starting_equity": 1000, "ending_equity": 1050, "total_return": 0.05,
                    "max_drawdown": -0.1, "sharpe": None, "win_rate": None,
                    "trade_count": 1, "fill_count": 2, "fees_paid": 0.5,
                    "period_start": "2026-01-05T14:30:00.000000000Z",
                    "period_end": "2026-01-05T14:33:00.000000000Z"},
        "equity_curve": [
            {"time": "2026-01-05T14:30:00.000000000Z", "equity": 1000, "cash": 1000},
            {"time": "2026-01-05T14:31:00.000000000Z", "equity": 1100, "cash": 900},
            {"time": "2026-01-05T14:32:00.000000000Z", "equity": 990, "cash": 900},
            {"time": "2026-01-05T14:33:00.123456789Z", "equity": 1050, "cash": 1050},
        ],
        "fills": [
            {"time": "2026-01-05T14:31:00.000000000Z", "order_id": 1, "symbol": "AAPL",
             "quantity": 1, "price": 100.0, "fees": 0.25},
            {"time": "2026-01-05T14:33:00.000000000Z", "order_id": 2, "symbol": "AAPL",
             "quantity": -1, "price": 150.0, "fees": 0.25},
        ],
        "stages": [],
    }
    doc.update(overrides)
    return doc


class LoadTest(unittest.TestCase):
    def write(self, doc):
        d = tempfile.mkdtemp()
        path = Path(d) / "run.json"
        path.write_text(json.dumps(doc) if not isinstance(doc, str) else doc)
        return path

    def test_reads_the_curve_fills_and_derived_series(self):
        data = pp.load_performance_export(self.write(results()))
        self.assertEqual(data.name, "te.bt-demo")
        self.assertEqual(len(data.equity_curve), 4)
        self.assertEqual(data.equity_curve[3][0].microsecond, 123456)
        self.assertEqual([f.quantity for f in data.fills], [1, -1])
        self.assertAlmostEqual(data.returns[0], 0.1)
        self.assertAlmostEqual(min(d for _, d in data.drawdown), 990 / 1100 - 1)

    def test_refuses_what_is_not_a_results_file(self):
        for doc in ("not json", {"schema": "other"}, results(equity_curve=[]),
                    results(fills={}),
                    results(equity_curve=[{"time": "2026-01-05T14:31:00Z", "equity": 1},
                                          {"time": "2026-01-05T14:30:00Z", "equity": 1}]),
                    results(equity_curve=[{"time": "2026-01-05T14:31:00Z", "equity": "1"}])):
            with self.subTest(doc=str(doc)[:40]), self.assertRaises(ValueError):
                pp.load_performance_export(self.write(doc))

    def test_summary_rows_show_unset_ratios_as_a_dash(self):
        rows = dict(pp.summary_rows(pp.load_performance_export(self.write(results()))))
        self.assertEqual(rows["Total return"], "5.00%")
        self.assertEqual(rows["Sharpe (ann.)"], "-")
        self.assertEqual(rows["Trades / fills"], "1 / 2")

    @unittest.skipUnless(HAVE_MATPLOTLIB, "matplotlib is not installed")
    def test_main_writes_a_png(self):
        out = Path(tempfile.mkdtemp())
        self.assertEqual(pp.main(["--input", str(self.write(results())), "--output", str(out)]), 0)
        png = out / "te.bt-demo_report.png"
        self.assertTrue(png.read_bytes().startswith(b"\x89PNG"))

    def test_main_reports_a_bad_file(self):
        self.assertEqual(pp.main(["--input", str(self.write({"schema": "x"}))]), 1)


if __name__ == "__main__":
    unittest.main()
