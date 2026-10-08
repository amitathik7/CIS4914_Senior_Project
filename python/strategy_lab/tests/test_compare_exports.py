"""Compare exports: configuration and provenance, both ORIGINAL runner results byte for byte, a labelled summary, and a clear
difference between FULL-RUN and VISIBLE-PREFIX files. Ids repeat across the two independent runs, so every row is member-scoped."""

import csv
import io
import json
import unittest
import zipfile

from support import RUN_ID, ev, make_document, outcome_of

from strategy_lab_ui import bridge, compare_exports as ce
from strategy_lab_ui.compare import CompareModel, CompareRequest, CompletedComparison, MemberConfig, MemberRun
from strategy_lab_ui.datasets import DatasetSource
from strategy_lab_ui.replay import ReplayModel

B_RUN_ID = "fedcba9876543210"


def make_comparison() -> CompletedComparison:
    a_specs = [ev("AAPL", 30, 1), ev("AAPL", 31, 2, signals=[(0, 1, "buy")]), ev("AAPL", 32, 3, signals=[(0, 2, "sell")])]
    b_specs = [ev("AAPL", 30, 1, signals=[(0, 1, "sell")]), ev("AAPL", 31, 2), ev("AAPL", 32, 3, signals=[(0, 2, "buy")])]
    runs = []
    for member, specs, run_id, kind in (("A", a_specs, RUN_ID, "sma_crossover"), ("B", b_specs, B_RUN_ID, "mean_reversion")):
        document = make_document(specs, run_id=run_id, kind=kind)
        document["provenance"]["result_sha256"] = ("0a" if member == "A" else "0b") * 32
        outcome = outcome_of(document)
        params = (("short_window", "2"), ("symbols", "AAPL")) if member == "A" else (("lookback", "4"), ("symbols", "AAPL"))
        config = MemberConfig(member, kind, params, f"{member}: {kind} label, with | a pipe", f"{member}: {kind}")
        runs.append(MemberRun(config, outcome, ReplayModel(outcome.document)))
    request = CompareRequest("builtin", "k", "ab" * 32, tuple(r.config for r in runs), "e" * 64)
    source = DatasetSource("builtin", "k", "k.csv", "ab" * 32, 100, "Synthetic fixture", True, ("AAPL",))
    runner = bridge.Runner(("x",), r"C:\Users\someone\secret folder\strategy_lab_replay.exe", 4242, 1, "e" * 64)
    return CompletedComparison(request, source, runner, tuple(runs), CompareModel(runs), 3, "2026-10-02T12:00:00+00:00")


def read_csv(data: bytes) -> list[dict[str, str]]:
    return list(csv.DictReader(io.StringIO(data.decode("utf-8"), newline="")))


class SignalsCsv(unittest.TestCase):
    def setUp(self):
        self.c = make_comparison()
        self.scope = ce.scope_full(self.c.model)

    def test_raw_ids_repeat_but_the_scoped_key_and_run_reference_tell_the_rows_apart(self):
        rows = read_csv(ce.signals_csv(self.c, self.c.model.signals_through(3), self.scope))
        self.assertEqual(len(rows), 4)
        self.assertEqual(sorted((r["member"], r["signal_id"]) for r in rows), [("A", "1"), ("A", "2"), ("B", "1"), ("B", "2")])
        self.assertEqual(len({r["member_signal_key"] for r in rows}), 4)
        by_key = {r["member_signal_key"]: r for r in rows}
        self.assertEqual(by_key[f"A/{RUN_ID}:1"]["run_id"], RUN_ID)
        self.assertEqual(by_key[f"B/{B_RUN_ID}:1"]["signal_ref"], f"{B_RUN_ID}:1")
        self.assertEqual(by_key[f"A/{RUN_ID}:1"]["event_number"], "2")           # 1-based, as on screen
        self.assertEqual(by_key[f"A/{RUN_ID}:1"]["event_index"], "1")            # 0-based, as recorded

    def test_every_row_repeats_its_scope_and_the_member_label_and_kind(self):
        rows = read_csv(ce.signals_csv(self.c, self.c.model.signals_through(3), self.scope))
        self.assertEqual({r["export_scope"] for r in rows}, {self.scope})
        self.assertIn("full run: all 3 events", self.scope)
        self.assertIn("both configurations", self.scope)
        self.assertEqual({r["strategy_kind"] for r in rows if r["member"] == "B"}, {"mean_reversion"})
        self.assertTrue(all("a pipe" in r["member_label"] for r in rows))

    def test_the_prefix_export_holds_only_what_is_revealed_and_says_so(self):
        prefix_scope = ce.scope_prefix(self.c.model, 2, None)
        rows = read_csv(ce.signals_csv(self.c, self.c.model.signals_through(2), prefix_scope))
        self.assertEqual(sorted((r["member"], r["signal_id"]) for r in rows), [("A", "1"), ("B", "1")])
        self.assertEqual({r["export_scope"] for r in rows}, {prefix_scope})
        self.assertIn("visible replay prefix: events 1-2 of 3", prefix_scope)
        self.assertNotEqual(prefix_scope, self.scope)

    def test_a_nothing_revealed_prefix_is_an_empty_file_with_a_header(self):
        scope = ce.scope_prefix(self.c.model, 0, None)
        self.assertIn("none revealed", scope)
        self.assertEqual(read_csv(ce.signals_csv(self.c, [], scope)), [])

    def test_a_symbol_filter_is_part_of_the_stated_scope_and_the_file_name(self):
        scope = ce.scope_prefix(self.c.model, 3, "AAPL")
        self.assertIn("symbol AAPL", scope)
        self.assertIn("prefix_3of3_AAPL", ce.signals_filename_prefix(self.c, 3, "AAPL"))
        self.assertIn("prefix_3of3_all", ce.summary_filename_prefix(self.c, 3, None))
        self.assertTrue(ce.package_filename(self.c).endswith("_full_run.zip"))


class SummaryCsv(unittest.TestCase):
    def test_counts_per_member_through_the_cursor_and_no_ranking_column(self):
        c = make_comparison()
        full = read_csv(ce.summary_csv(c, 3, None, ce.scope_full(c.model)))
        self.assertEqual([(r["member"], r["buy_requests"], r["sell_requests"], r["signal_requests"]) for r in full],
                         [("A", "1", "1", "2"), ("B", "1", "1", "2")])
        prefix = read_csv(ce.summary_csv(c, 1, None, ce.scope_prefix(c.model, 1, None)))
        self.assertEqual([(r["member"], r["signal_requests"], r["events_counted"]) for r in prefix], [("A", "0", "1"), ("B", "1", "1")])
        self.assertTrue({"rank", "winner", "score", "better", "profit"}.isdisjoint(full[0]))
        self.assertEqual({r["run_id"] for r in full}, {RUN_ID, B_RUN_ID})


class Package(unittest.TestCase):
    def setUp(self):
        self.c = make_comparison()
        self.data = ce.package_zip(self.c)
        self.zip = zipfile.ZipFile(io.BytesIO(self.data))

    def test_it_is_a_valid_zip_with_the_expected_files(self):
        self.assertIsNone(self.zip.testzip())
        self.assertEqual(sorted(self.zip.namelist()), sorted([
            "comparison_provenance.json", "comparison_summary_full_run.csv", "comparison_signals_full_run.csv",
            f"A_{RUN_ID}_run.json", f"B_{B_RUN_ID}_run.json"]))

    def test_both_original_runner_results_are_byte_for_byte_what_the_tool_wrote(self):
        for run in self.c.runs:
            name = f"{run.config.member}_{run.document.run_id}_run.json"
            self.assertEqual(self.zip.read(name), run.raw_json)
        a = self.zip.read(f"A_{RUN_ID}_run.json")
        self.assertTrue(a.endswith(b"}\n"))
        self.assertNotIn(b"\r", a)

    def test_the_provenance_names_configuration_dataset_executable_and_scope(self):
        prov = json.loads(self.zip.read("comparison_provenance.json"))
        self.assertIn("full run", prov["scope"])
        self.assertIn("Counts are not a measure of strategy quality", prov["note"])
        self.assertIn("no fills, returns or portfolio", prov["note"])
        self.assertEqual(prov["dataset"]["sha256"], "ab" * 32)
        self.assertEqual(prov["shared_by_both_members"]["dataset_sha256"], "ab" * 32)
        self.assertEqual([m["member"] for m in prov["members"]], ["A", "B"])
        self.assertEqual([m["run_id"] for m in prov["members"]], [RUN_ID, B_RUN_ID])
        self.assertEqual([m["result_sha256"] for m in prov["members"]], ["0a" * 32, "0b" * 32])
        self.assertEqual(prov["members"][0]["parameters_as_passed"], [{"name": "short_window", "value": "2"},
                                                                       {"name": "symbols", "value": "AAPL"}])
        self.assertEqual([m["raw_result_file"] for m in prov["members"]], [f"A_{RUN_ID}_run.json", f"B_{B_RUN_ID}_run.json"])
        self.assertEqual(prov["executable"]["sha256"], "e" * 64)
        self.assertEqual(prov["executable"]["size_bytes"], 4242)
        self.assertEqual(prov["comparison_number_in_session"], 3)

    def test_the_provenance_does_not_leak_the_local_folder_path(self):
        text = self.zip.read("comparison_provenance.json").decode("utf-8")
        self.assertIn("strategy_lab_replay.exe", text)
        self.assertNotIn("secret folder", text)
        self.assertNotIn("someone", text)

    def test_the_full_run_files_cover_every_event_and_say_so(self):
        scope = ce.scope_full(self.c.model)
        signals = read_csv(self.zip.read("comparison_signals_full_run.csv"))
        self.assertEqual(len(signals), 4)
        self.assertEqual({r["export_scope"] for r in signals}, {scope})
        summary = read_csv(self.zip.read("comparison_summary_full_run.csv"))
        self.assertEqual({r["export_scope"] for r in summary}, {scope})

    def test_the_same_comparison_always_gives_the_same_bytes(self):
        self.assertEqual(ce.package_zip(make_comparison()), self.data)

    def test_no_file_is_named_like_a_prefix_export(self):
        self.assertFalse([n for n in self.zip.namelist() if "prefix" in n])


if __name__ == "__main__":
    unittest.main()
