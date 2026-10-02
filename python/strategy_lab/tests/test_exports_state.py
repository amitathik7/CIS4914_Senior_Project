"""Exports (scope labels, exactness, agreement with what the table shows) and the stale/failed status logic."""

import csv
import io
import json
import unittest
from types import SimpleNamespace

from support import dumps, ev, make_document, parse

from strategy_lab_ui import exports, tables
from strategy_lab_ui.errors import LabUiError
from strategy_lab_ui.replay import ReplayModel
from strategy_lab_ui.state import RunRequest, failed, run_status


def rows_of(data: bytes) -> list[list[str]]:
    return list(csv.reader(io.StringIO(data.decode("utf-8"))))


def model_with_signals() -> ReplayModel:
    return ReplayModel(parse(make_document([
        ev("AAPL", 30), ev("MSFT", 30, signals=[(0, 1, "buy")]), ev("AAPL", 31, signals=[(0, 2, "sell")]),
        ev("MSFT", 32, signals=[(0, 3, "sell")])])))


class SignalsCsv(unittest.TestCase):
    def test_header_scope_and_rows_for_the_full_run(self):
        model = model_with_signals()
        rows = rows_of(exports.signals_csv(list(model.signals), exports.scope_full(model)))
        self.assertEqual(rows[0][:12], list(exports.BASE_COLUMNS))
        self.assertEqual(rows[0][12:], ["metadata.trigger"])
        self.assertEqual(len(rows) - 1, 3)
        self.assertTrue(all(r[0] == "full run: all 4 events, all symbols" for r in rows[1:]))
        self.assertEqual([r[1] for r in rows[1:]], ["1", "2", "3"])
        self.assertEqual([r[4] for r in rows[1:]], ["1", "2", "3"])          # event_index (0-based)
        self.assertEqual([r[5] for r in rows[1:]], ["2", "3", "4"])          # event_number (1-based, as the UI says)

    def test_the_prefix_export_is_exactly_what_the_table_shows_and_says_so(self):
        model = model_with_signals()
        for cursor, symbol in ((0, None), (2, None), (3, None), (4, "MSFT"), (4, "AAPL")):
            shown = tables.signals_table(model, cursor, symbol)
            scope = exports.scope_prefix(model, cursor, symbol)
            rows = rows_of(exports.signals_csv(model.signals_through(cursor, symbol), scope))
            self.assertEqual([r[1] for r in rows[1:]], list(shown["Signal"]), (cursor, symbol))
            self.assertTrue(all(r[0] == scope for r in rows[1:]))
            self.assertIn("visible replay prefix", scope)

    def test_scopes_name_what_they_cover(self):
        model = model_with_signals()
        self.assertEqual(exports.scope_prefix(model, 0, None), "visible replay prefix: none revealed of 4, all symbols")
        self.assertEqual(exports.scope_prefix(model, 3, "MSFT"), "visible replay prefix: events 1-3 of 4, symbol MSFT")
        self.assertTrue(exports.scope_full(model).startswith("full run"))

    def test_an_empty_prefix_exports_a_header_only(self):
        rows = rows_of(exports.signals_csv([], "visible replay prefix: none revealed of 4, all symbols"))
        self.assertEqual(len(rows), 1)

    def test_huge_ids_and_sequences_are_written_exactly(self):
        sid = 2**63 - 1
        model = ReplayModel(parse(make_document([ev("A", 30, signals=[(0, sid, "buy")])], run_id="feedfacefeedface")))
        row = rows_of(exports.signals_csv(list(model.signals), "x"))[1]
        self.assertEqual(row[1], str(sid))
        self.assertEqual(row[2], f"feedfacefeedface:{sid}")
        self.assertEqual(tables.signals_table(model, 1, None)["Signal"][0], str(sid))   # text in the UI table too

    def test_numbers_round_trip_and_timestamps_keep_nanoseconds(self):
        document = make_document([ev("A", 30, time="2026-01-05T14:30:00.123456789Z", signals=[(0, 1, "buy")])])
        document["result"]["signals"][0]["requested_quantity"] = 0.1 + 0.2
        row = rows_of(exports.signals_csv(list(parse(document).signals), "x"))[1]
        self.assertEqual(float(row[9]), 0.1 + 0.2)
        self.assertEqual(row[10], "2026-01-05T14:30:00.123456789Z")

    def test_metadata_columns_are_the_sorted_union(self):
        document = make_document([ev("A", 30, signals=[(0, 1, "buy")]), ev("A", 31, signals=[(0, 2, "sell")])])
        document["result"]["signals"][0]["metadata"] = {"zeta": "1", "alpha": "2"}
        document["result"]["signals"][1]["metadata"] = {"beta": "3"}
        rows = rows_of(exports.signals_csv(list(parse(document).signals), "x"))
        self.assertEqual(rows[0][12:], ["metadata.alpha", "metadata.beta", "metadata.zeta"])
        self.assertEqual(rows[1][12:], ["2", "", "1"])
        self.assertEqual(rows[2][12:], ["", "3", ""])

    def test_file_names_say_which_scope_and_are_safe(self):
        self.assertEqual(exports.csv_filename_full("abc"), "strategy_lab_abc_signals_full_run.csv")
        self.assertEqual(exports.csv_filename_prefix("abc", 4, 9, "A/B C"), "strategy_lab_abc_signals_prefix_4of9_A_B_C.csv")
        self.assertEqual(exports.csv_filename_prefix("abc", 0, 9, None), "strategy_lab_abc_signals_prefix_0of9_all.csv")
        self.assertEqual(exports.json_filename("../x"), "strategy_lab_.._x_full_run.json".replace("/", "_"))
        self.assertNotIn("/", exports.json_filename("a/b\\c"))

    def test_the_json_export_is_the_runner_bytes_verbatim(self):
        raw = dumps(make_document([ev("A", 30)]))
        doc = json.loads(raw)
        self.assertEqual(raw, json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode() + b"\n")


class Status(unittest.TestCase):
    def request(self, **changes):
        base = dict(dataset_kind="builtin", dataset_key="sma_crossover", dataset_sha256="aa", strategy_kind="sma_crossover",
                    params=(("short_window", "2"), ("long_window", "3")), executable_sha256="e1")
        base.update(changes)
        return RunRequest(**base)

    def test_identical_requests_have_no_differences(self):
        self.assertEqual(self.request().differences(self.request()), [])

    def test_each_kind_of_change_is_named(self):
        shown = self.request()
        self.assertIn("parameters (short_window)", self.request(params=(("short_window", "1"), ("long_window", "3"))).differences(shown)[0])
        self.assertTrue(any("strategy" in r for r in self.request(strategy_kind="mean_reversion").differences(shown)))
        self.assertTrue(any("dataset (" in r for r in self.request(dataset_key="mr_rearm").differences(shown)))
        self.assertEqual(self.request(dataset_sha256="bb").differences(shown), ["dataset contents"])
        self.assertTrue(any("executable" in r for r in self.request(executable_sha256="e2").differences(shown)))

    def test_a_changed_parameter_text_counts_even_if_numerically_equal(self):
        shown = self.request()
        edited = self.request(params=(("short_window", "02"), ("long_window", "3")))
        self.assertNotEqual(edited.differences(shown), [])

    def displayed(self, request):
        return SimpleNamespace(request=request, number=1, finished_at="t")

    def test_status_table(self):
        shown = self.request()
        self.assertEqual(run_status(None, None, shown).kind, "empty")
        self.assertEqual(run_status(self.displayed(shown), None, shown).kind, "current")
        stale = run_status(self.displayed(shown), None, self.request(strategy_kind="mean_reversion"))
        self.assertEqual(stale.kind, "stale")
        self.assertTrue(stale.reasons)
        self.assertEqual(run_status(self.displayed(shown), None, None).kind, "stale")        # nothing runnable on screen

    def test_a_failed_run_is_never_reported_as_current(self):
        shown = self.request()
        failure = failed(self.request(strategy_kind="mean_reversion"), LabUiError("runner_rejected", "no"), 2)
        status = run_status(self.displayed(shown), failure, shown)
        self.assertEqual(status.kind, "failed")
        self.assertEqual(status.reasons, ())                       # the settings still match the retained result
        status = run_status(self.displayed(shown), failure, self.request(dataset_key="x"))
        self.assertEqual(status.kind, "failed")
        self.assertTrue(status.reasons)                            # and when they do not, the banner can say so
        self.assertEqual(run_status(None, failure, shown).kind, "failed")


if __name__ == "__main__":
    unittest.main()
