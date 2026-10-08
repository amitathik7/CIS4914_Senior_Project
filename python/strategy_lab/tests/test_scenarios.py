"""The checked-in validation scenarios and their loader. No executable is needed here: these tests prove the files are consistent
with the fixtures, carry their provenance, and are loaded strictly (a misspelt key must not silently switch an assertion off).
"""

import copy
import hashlib
import json
import re
import shutil
import tempfile
import unittest
from pathlib import Path

from support import ROOT

from strategy_lab_ui import scenarios
from strategy_lab_ui.errors import LabUiError

SCENARIO_DIR = scenarios.SCENARIO_DIR
FIXTURES = scenarios.FIXTURE_ROOT


def minimal() -> dict:
    """The smallest valid result scenario (one strategy, one signal, one numeric check)."""
    return {
        "schema": "strategy_lab.scenario/1", "id": "tiny", "title": "t", "purpose": "p", "covers": ["x"],
        "dataset": {"path": "datasets/sma_crossover.csv", "sha256_lf": "0" * 64},
        "strategies": [{"kind": "sma_crossover", "params": {"strategy_id": "s", "short_window": "2", "long_window": "3", "symbols": "AAPL"}}],
        "provenance": {"derived_by": "by hand", "sources": ["doc"], "derivation": ["step"], "cross_checks": []},
        "tolerances": {"loose": {"abs_tol": "1e-9", "why": "because"}},
        "expect": {"outcome": "result", "warnings": [], "signals": [{"strategy": "s", "event_index": 4, "symbol": "AAPL", "side": "buy"}],
                   "events": [{"strategy": "s", "event_index": 4, "indicators": {"short_sma": {"exact": "5/2"}}}]}}


def parse(raw: dict):
    return scenarios.parse_scenario(copy.deepcopy(raw), "tiny.json")


class TheCheckedInFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.set = scenarios.load_scenarios()

    def test_there_are_scenarios_with_unique_ids_in_a_stable_order(self):
        ids = [s.id for s in self.set.scenarios]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertGreaterEqual(len(ids), 15)
        self.assertEqual(list(self.set.files), sorted(self.set.files))
        self.assertTrue(all(name[:2].isdigit() for name in self.set.files))

    def test_every_required_kind_of_check_is_represented(self):
        covered = {tag for s in self.set.scenarios for tag in s.covers}
        for needed in ("sma_signal_timing", "mr_rearm", "mr_threshold", "constant_window", "warmup", "mr_suppression",
                       "interleaving", "equal_time_ordering", "dataset_rejection", "config_rejection", "insufficient_data"):
            self.assertIn(needed, covered)

    def test_rejection_scenarios_exist_for_dataset_and_configuration(self):
        rejected = [s for s in self.set.scenarios if s.outcome == "rejected"]
        self.assertGreaterEqual(len(rejected), 4)
        self.assertEqual({s.rejection.exit_status for s in rejected}, {2, 3})
        for s in rejected:
            self.assertTrue(s.rejection.error_code)

    def test_each_scenario_says_how_its_expectation_was_derived(self):
        for s in self.set.scenarios:
            self.assertIn("by hand", s.provenance.derived_by, s.id)
            self.assertIn("not copied from any run", s.provenance.derived_by, s.id)
            self.assertTrue(s.provenance.sources and s.provenance.derivation and s.provenance.cross_checks, s.id)
            self.assertGreaterEqual(len(s.provenance.derivation), 1, s.id)

    def test_the_cross_checks_name_files_that_exist(self):
        repo = ROOT.parents[1]
        for s in self.set.scenarios:
            for line in s.provenance.cross_checks:
                for path in re.findall(r"(tests/[A-Za-z0-9_./]+\.(?:cpp|py))", line):
                    self.assertTrue((repo / path).is_file(), f"{s.id}: {path}")
            for line in s.provenance.sources:
                for path in re.findall(r"(docs/[A-Za-z0-9_./]+\.md)", line):
                    self.assertTrue((repo / path).is_file(), f"{s.id}: {path}")

    def test_every_numeric_tolerance_is_declared_with_a_written_justification(self):
        used = 0
        for s in self.set.scenarios:
            for expectation in s.events:
                for name, number in expectation.indicators:
                    if number.tolerance is not None:
                        used += 1
                        self.assertGreater(number.tolerance.abs_tol, 0)
                        self.assertGreater(len(number.tolerance.why), 30, f"{s.id}/{name}")
        self.assertGreater(used, 5)

    def test_expected_values_are_exact_fractions_never_floats_copied_from_output(self):
        for path in SCENARIO_DIR.glob("*.json"):
            text = path.read_text(encoding="utf-8")
            self.assertNotRegex(text, r'"result_sha256"|"run_id"|"generated_at"', path.name)       # nothing from a tool run
            self.assertNotRegex(text, r'"exact": [0-9]', path.name)                                  # exact values are strings

    def test_every_dataset_pin_matches_the_checked_in_file(self):
        for s in self.set.scenarios:
            data = (FIXTURES / s.dataset_path).read_bytes()
            self.assertEqual(hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest(), s.dataset_sha256_lf, s.id)

    def test_dataset_rejection_scenarios_use_the_invalid_directory(self):
        for s in self.set.scenarios:
            if "dataset_rejection" in s.covers:
                self.assertTrue(s.dataset_path.startswith("invalid/"), s.id)
            else:
                self.assertTrue(s.dataset_path.startswith("datasets/"), s.id)

    def test_expected_signal_strategies_and_events_refer_to_the_scenarios_own_strategies(self):
        for s in self.set.scenarios:
            ids = {c.strategy_id for c in s.strategies}
            for sig in s.signals or ():
                self.assertIn(sig.strategy, ids)
            for event in s.events:
                self.assertIn(event.strategy, ids)

    def test_there_is_no_failing_scenario_among_the_files(self):
        self.assertFalse([f for f in self.set.files if "demo" in f.lower() or "mismatch" in f.lower()])
        self.assertIsNone(self.set.by_id(scenarios.DEMO_ID))


class Identity(unittest.TestCase):
    def copy_dir(self) -> Path:
        tmp = Path(tempfile.mkdtemp(prefix="sl_scen_"))
        self.addCleanup(shutil.rmtree, tmp, True)
        for path in SCENARIO_DIR.glob("*.json"):
            shutil.copy(path, tmp / path.name)
        return tmp

    def test_the_identity_is_stable_across_a_crlf_checkout(self):
        original = scenarios.load_scenarios().identity
        tmp = self.copy_dir()
        for path in tmp.glob("*.json"):
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(scenarios.load_scenarios(tmp).identity, original)

    def test_the_identity_changes_when_an_expectation_changes(self):
        tmp = self.copy_dir()
        first = sorted(tmp.glob("*.json"))[0]
        first.write_bytes(first.read_bytes().replace(b'"buy"', b'"sell"', 1))
        self.assertNotEqual(scenarios.load_scenarios(tmp).identity, scenarios.load_scenarios().identity)


class TheLoaderIsStrict(unittest.TestCase):
    def assert_refused(self, raw: dict, fragment: str = ""):
        with self.assertRaises(LabUiError) as caught:
            parse(raw)
        self.assertIn(fragment, caught.exception.message)

    def test_the_minimal_scenario_is_valid(self):
        s = parse(minimal())
        self.assertEqual(s.id, "tiny")
        self.assertEqual(s.strategies[0].strategy_id, "s")
        self.assertEqual(dict(s.events[0].indicators)["short_sma"].value.numerator, 5)

    def test_an_unknown_member_is_refused_so_a_typo_cannot_switch_an_assertion_off(self):
        raw = minimal()
        raw["expect"]["events"][0]["verdit"] = "evaluated"
        self.assert_refused(raw, "unknown member(s) verdit")
        raw = minimal()
        raw["expct"] = {}
        self.assert_refused(raw, "expct")

    def test_exact_must_be_representable_and_approx_needs_a_declared_tolerance(self):
        raw = minimal()
        raw["expect"]["events"][0]["indicators"]["short_sma"] = {"exact": "1/3"}
        self.assert_refused(raw, "not exactly representable")
        raw = minimal()
        raw["expect"]["events"][0]["indicators"]["short_sma"] = {"approx": "0.1", "tolerance": "missing"}
        self.assert_refused(raw, "not declared")
        raw = minimal()
        raw["expect"]["events"][0]["indicators"]["short_sma"] = {"approx": "0.1"}
        self.assert_refused(raw, "tolerance")

    def test_a_tolerance_must_be_positive_and_justified(self):
        raw = minimal()
        raw["tolerances"]["loose"]["abs_tol"] = "0"
        self.assert_refused(raw, "greater than zero")
        raw = minimal()
        raw["tolerances"]["loose"]["why"] = " "
        self.assert_refused(raw, "must not be empty")
        raw = minimal()
        del raw["tolerances"]["loose"]["why"]
        self.assert_refused(raw, "why")

    def test_a_derivation_is_required(self):
        raw = minimal()
        raw["provenance"]["derivation"] = []
        self.assert_refused(raw, "must say how it was derived")

    def test_dataset_paths_cannot_escape_the_fixture_directory(self):
        for bad in ("../secrets.csv", "/etc/passwd", "datasets/../../x.csv", "other/x.csv", "datasets\\x.csv"):
            raw = minimal()
            raw["dataset"]["path"] = bad
            self.assert_refused(raw, "relative path under datasets/ or invalid/")

    def test_values_are_checked(self):
        raw = minimal()
        raw["expect"]["signals"][0]["side"] = "hold"
        self.assert_refused(raw, "not buy or sell")
        raw = minimal()
        raw["expect"]["signals"][0]["strategy"] = "other"
        self.assert_refused(raw, "not one of this scenario's strategy ids")
        raw = minimal()
        raw["expect"]["events"][0]["event_range"] = [1, 2]
        self.assert_refused(raw, "exactly one of event_index and event_range")
        raw = minimal()
        raw["expect"]["outcome"] = "maybe"
        self.assert_refused(raw, "not result or rejected")
        raw = minimal()
        raw["strategies"].append(copy.deepcopy(raw["strategies"][0]))
        self.assert_refused(raw, "unique")
        raw = minimal()
        raw["id"] = "Bad-Id"
        self.assert_refused(raw, "identifier")

    def test_a_rejection_must_be_exit_status_2_or_3(self):
        raw = minimal()
        raw["expect"] = {"outcome": "rejected", "exit_status": 4, "error_code": "internal_error"}
        self.assert_refused(raw, "exit status 2 or 3")
        raw["expect"]["exit_status"] = 3
        raw["expect"]["error_code"] = "dataset_error"
        self.assertEqual(parse(raw).rejection.exit_status, 3)


class FilesOnDisk(unittest.TestCase):
    def load(self, **files: str):
        tmp = Path(tempfile.mkdtemp(prefix="sl_scen_"))
        self.addCleanup(shutil.rmtree, tmp, True)
        for name, text in files.items():
            (tmp / f"{name}.json").write_text(text, encoding="utf-8")
        return scenarios.load_scenarios(tmp)

    def test_duplicate_json_keys_and_nan_are_refused(self):
        with self.assertRaises(LabUiError) as caught:
            self.load(a='{"id": "x", "id": "y"}')
        self.assertIn("duplicate object key", caught.exception.message)
        with self.assertRaises(LabUiError) as caught:
            self.load(a='{"v": NaN}')
        self.assertIn("NaN", caught.exception.message)

    def test_text_after_the_document_is_refused(self):
        with self.assertRaises(LabUiError) as caught:
            self.load(a=json.dumps(minimal()) + " junk")
        self.assertIn("after the JSON document", caught.exception.message)

    def test_an_empty_directory_is_an_error_not_an_all_passed(self):
        with self.assertRaises(LabUiError) as caught:
            self.load()
        self.assertEqual(caught.exception.kind, "scenario_invalid")

    def test_the_same_id_in_two_files_is_refused(self):
        with self.assertRaises(LabUiError) as caught:
            self.load(a=json.dumps(minimal()), b=json.dumps(minimal()))
        self.assertIn("more than one file", caught.exception.message)

    def test_a_problem_names_its_file(self):
        raw = minimal()
        raw["expect"]["signals"][0]["side"] = "hold"
        with self.assertRaises(LabUiError) as caught:
            self.load(broken=json.dumps(raw))
        self.assertTrue(caught.exception.message.startswith("broken.json"))


class InjectedMismatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.set = scenarios.load_scenarios()

    def test_it_is_a_new_in_memory_scenario_with_two_wrong_expectations(self):
        base = self.set.by_id(scenarios.DEMO_BASE)
        demo = scenarios.injected_mismatch(base)
        self.assertTrue(demo.demonstration)
        self.assertEqual(demo.id, scenarios.DEMO_ID)
        self.assertEqual(demo.signals[0].event_index, base.signals[0].event_index + 1)
        self.assertEqual(demo.signals[1:], base.signals[1:])
        wrong = next(dict(e.indicators)["short_sma"].value for e in demo.events if "short_sma" in dict(e.indicators))
        right = next(dict(e.indicators)["short_sma"].value for e in base.events if "short_sma" in dict(e.indicators))
        self.assertEqual(wrong - right, 0.5)
        self.assertIn("EXPECTED to fail", demo.purpose)

    def test_the_base_scenario_is_untouched_and_the_report_text_shows_the_wrong_value(self):
        base = self.set.by_id(scenarios.DEMO_BASE)
        before = copy.deepcopy(dict(base.raw))
        demo = scenarios.injected_mismatch(base)
        self.assertEqual(dict(base.raw), before)
        self.assertEqual(demo.raw["expect"]["signals"][0]["event_index"], before["expect"]["signals"][0]["event_index"] + 1)

    def test_only_a_scenario_that_expects_signals_can_be_corrupted(self):
        rejected = next(s for s in self.set.scenarios if s.outcome == "rejected")
        with self.assertRaises(ValueError):
            scenarios.injected_mismatch(rejected)


if __name__ == "__main__":
    unittest.main()
