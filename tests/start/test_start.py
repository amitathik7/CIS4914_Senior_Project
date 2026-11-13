"""start.py's choice of bars: the year of real bars by default, a chosen calendar year, the demo as the fallback.

Standard library only, no network and no real keys: the download is a stand-in that writes the --out file. CI runs
it on Linux, macOS and Windows.
"""

import argparse
import datetime
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import start  # noqa: E402

DUMMY_KEYS = {"ALPACA_API_KEY": "test-key-id", "ALPACA_API_SECRET": "test-secret"}
NO_KEYS = {"ALPACA_API_KEY": "", "ALPACA_API_SECRET": ""}


class InTempRoot(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        (self.root / start.DEMO_BARS).parent.mkdir(parents=True)
        (self.root / start.DEMO_BARS).write_text("symbol,exchange_time,type,price,volume\n", encoding="utf-8")
        for patch in (mock.patch.object(start, "ROOT", self.root), mock.patch.object(start, "say")):
            patch.start()
            self.addCleanup(patch.stop)
        self.commands: list[list[str]] = []

    def args(self, *argv: str) -> argparse.Namespace:
        return start.parse_args(list(argv))

    def download(self, cmd: list[str]) -> None:
        self.commands.append(cmd)
        out = Path(cmd[cmd.index("--out") + 1])
        assert out.name.endswith(".partial"), out
        out.write_text("symbol,exchange_time,type,price,volume\nAAPL,2025-01-02T14:30:00Z,bar,1,1\n", encoding="utf-8")

    def fail(self, cmd: list[str]) -> None:
        self.commands.append(cmd)
        Path(cmd[cmd.index("--out") + 1]).write_text("half", encoding="utf-8")
        raise start.LaunchError("fetch-alpaca failed with exit code 1")

    def fetch(self, behaviour, year, required, keys=DUMMY_KEYS):
        with mock.patch.object(start, "run_step", lambda name, cmd: behaviour(cmd)), \
                mock.patch.dict(os.environ, keys):
            return start.fetch_year(Path("trading_engine"), year, required)


class ChooseBars(InTempRoot):
    def test_a_fresh_clone_downloads_the_last_365_days(self):
        self.assertEqual(start.choose_bars(self.args()), ([str(start.YEAR_BARS)], True, None))

    def test_year_picks_a_calendar_year(self):
        self.assertEqual(start.choose_bars(self.args("--year", "2025")),
                         ([str(Path("data") / "alpaca_2025.csv")], True, 2025))
        self.assertEqual(start.choose_bars(self.args("--year")), ([str(start.YEAR_BARS)], True, None))

    def test_demo_and_bars_need_no_download(self):
        self.assertEqual(start.choose_bars(self.args("--demo")), ([str(start.DEMO_BARS)], False, None))
        mine = self.root / "mine.csv"
        mine.write_text("x", encoding="utf-8")
        self.assertEqual(start.choose_bars(self.args("--bars", str(mine))), ([str(mine)], False, None))

    def test_bad_choices(self):
        with self.assertRaises(start.LaunchError):
            start.choose_bars(self.args("--demo", "--year", "2025"))
        with self.assertRaises(start.LaunchError):
            start.choose_bars(self.args("--bars", "nowhere.csv"))
        for bad in ("25", "next", "20255"):
            with self.assertRaises(SystemExit), mock.patch("sys.stderr"):
                self.args("--year", bad)

    def test_the_last_choice_is_kept_except_the_demo(self):
        chosen = Path("data") / "alpaca_2024.csv"
        (self.root / chosen).parent.mkdir()
        (self.root / chosen).write_text("x", encoding="utf-8")
        start.remember_bars([str(chosen)])
        self.assertEqual(start.choose_bars(self.args()), ([str(chosen)], False, None))
        start.remember_bars([str(start.DEMO_BARS)])                     # --demo is for one start
        self.assertEqual(start.choose_bars(self.args()), ([str(chosen)], False, None))
        (self.root / start.LAST_BARS).write_text(json.dumps([str(start.DEMO_BARS)]), encoding="utf-8")
        self.assertEqual(start.choose_bars(self.args()), ([str(start.YEAR_BARS)], True, None))  # an older version's


class FetchYear(InTempRoot):
    def test_downloads_once_with_the_dates_worked_out(self):
        self.assertTrue(self.fetch(self.download, 2025, required=True))
        self.assertTrue((self.root / "data" / "alpaca_2025.csv").is_file())
        self.assertFalse(any((self.root / "data").glob("*.partial")))
        cmd = self.commands[0]
        self.assertEqual(cmd[1], "fetch-alpaca")
        self.assertEqual((cmd[cmd.index("--start") + 1], cmd[cmd.index("--timeframe") + 1]), ("2025-01-01", "1Min"))
        self.assertTrue(self.fetch(self.download, 2025, required=True))
        self.assertEqual(len(self.commands), 1)

    def test_without_keys_the_default_falls_back_and_year_stops(self):
        self.assertFalse(self.fetch(self.download, None, required=False, keys=NO_KEYS))
        with self.assertRaises(start.LaunchError):
            self.fetch(self.download, 2025, required=True, keys=NO_KEYS)
        self.assertEqual(self.commands, [])

    def test_a_failed_download_leaves_nothing_behind(self):
        self.assertFalse(self.fetch(self.fail, None, required=False))
        with self.assertRaises(start.LaunchError):
            self.fetch(self.fail, 2025, required=True)
        self.assertEqual(list((self.root / "data").iterdir()), [])
        self.assertTrue(self.fetch(self.download, None, required=False))   # the next start tries again


class YearRange(unittest.TestCase):
    def test_ranges(self):
        today = datetime.date(2026, 11, 13)
        self.assertEqual(start.year_range(None, today), (datetime.date(2025, 11, 12), datetime.date(2026, 11, 12)))
        self.assertEqual(start.year_range(2025, today), (datetime.date(2025, 1, 1), datetime.date(2025, 12, 31)))
        self.assertEqual(start.year_range(2026, today), (datetime.date(2026, 1, 1), datetime.date(2026, 11, 12)))
        with self.assertRaises(start.LaunchError):
            start.year_range(2027, today)
        with self.assertRaises(start.LaunchError):
            start.year_range(2026, datetime.date(2026, 1, 1))


if __name__ == "__main__":
    unittest.main()
