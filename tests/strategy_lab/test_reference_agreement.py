"""The C++ decisions against an INDEPENDENT reference in exact rational arithmetic
(reference_model.py), over the checked-in fixtures: the same signals, the same verdict and reason
for every event, the same window fill, and the same numbers to a tight tolerance.

The reference was written from the strategy documents, with Fractions, and shares no code or
floating-point behaviour with the C++. Agreement is therefore evidence, not an echo.
"""

from __future__ import annotations

import math
import unittest

import labtest
import reference_model as ref
from labtest import LabTestCase, datasets, run_exe

# (fixture, SMA config, MR config): chosen so every documented rule is exercised somewhere.
CASES = [
    ("sma_crossover.csv", ("2", "3"), ("4", "1.5", "0.5")),
    ("sma_oscillation.csv", ("1", "2"), ("3", "1", "0.25")),
    ("mr_rearm.csv", ("2", "3"), ("4", "1.5", "0.5")),
    ("mr_suppression.csv", ("2", "3"), ("4", "1.5", "0.5")),
    ("constant_price.csv", ("5", "20"), ("20", "2", "0.5")),
    ("warmup_boundary.csv", ("2", "6"), ("6", "2", "0.5")),
    ("two_symbols_interleaved.csv", ("2", "3"), ("4", "1.2", "0.5")),
    ("tie_aapl_first.csv", ("1", "2"), ("2", "0.9", "0.1")),
    ("tie_msft_first.csv", ("1", "2"), ("2", "0.9", "0.1")),
    ("bars_and_trades.csv", ("1", "2"), ("2", "0.9", "0.1")),
    ("ohlcv_example.csv", ("1", "2"), ("3", "1", "0.25")),
]


class AgreesWithTheExactReference(LabTestCase):
    def check(self, fixture: str, sma: tuple[str, str], mr: tuple[str, str, str]) -> None:
        rows = ref.load_rows(datasets(fixture))
        symbols = sorted({row.symbol for row in rows})
        listed = ",".join(symbols)
        args = ["run", "--dataset", datasets(fixture),
                "--strategy", "sma_crossover", "--param", "strategy_id=sma", "--param", f"short_window={sma[0]}",
                "--param", f"long_window={sma[1]}", "--param", f"symbols={listed}",
                "--strategy", "mean_reversion", "--param", "strategy_id=mr", "--param", f"lookback={mr[0]}",
                "--param", f"entry_threshold={mr[1]}", "--param", f"rearm_threshold={mr[2]}", "--param", f"symbols={listed}",
                "--no-wall-clock"]
        run = run_exe(args)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = run.json()["result"]

        expected = [ref.sma_crossover(rows, int(sma[0]), int(sma[1]), symbols),
                    ref.mean_reversion(rows, int(mr[0]), mr[1], mr[2], symbols)]
        ids = ["sma", "mr"]

        for slot, decisions in enumerate(expected):
            for index, want in enumerate(decisions):
                got = result["events"][index]["results"][slot]
                where = f"{fixture} event {index} ({ids[slot]})"
                self.assertEqual(got["strategy_id"], ids[slot], where)
                self.assertEqual((got["verdict"], got["reason"], got["action"]), (want.verdict, want.reason, want.action), where)
                self.assertEqual(got["window"].get("fill"), want.window_fill, where)
                self.assertEqual(set(got["indicators"]), set(want.numbers), where)
                for name, value in want.numbers.items():
                    self.assertTrue(math.isclose(got["indicators"][name], value, rel_tol=1e-9, abs_tol=1e-12),
                                    f"{where}: {name} {got['indicators'][name]!r} vs {value!r}")

        # The signals: the same set, and published in registration order within an event.
        want_signals = []
        for index in range(len(rows)):
            for slot in (0, 1):
                if expected[slot][index].action != "none":
                    want_signals.append((ids[slot], rows[index].symbol, expected[slot][index].action, index))
        got_signals = [(s["strategy_id"], s["symbol"], s["side"], s["event_index"]) for s in result["signals"]]
        self.assertEqual(got_signals, want_signals, fixture)
        self.assertEqual([s["signal_id"] for s in result["signals"]], list(range(1, len(want_signals) + 1)))


def _make(case: tuple) -> None:
    fixture, sma, mr = case

    def test(self: AgreesWithTheExactReference) -> None:
        self.check(fixture, sma, mr)

    setattr(AgreesWithTheExactReference, f"test_{fixture.replace('.csv', '')}", test)


for _case in CASES:
    _make(_case)


if __name__ == "__main__":
    unittest.main()
