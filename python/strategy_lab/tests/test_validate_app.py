"""The Validate view through AppTest against the REAL strategy_lab_replay: real results, a controlled wrong expectation shown as a
failed check, errors shown as errors, and no run unless the button is pressed.

A temporary copy of the scenario files (in a temp directory, discarded afterwards) carries the deliberately wrong expectation; the
checked-in scenarios are never edited.
"""

import dataclasses
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from support import APP, ProcessCounter, open_view, page_text, real_runner

from streamlit.testing.v1 import AppTest

from strategy_lab_ui import bridge, scenarios, validate_page
from strategy_lab_ui.errors import LabUiError


class ValidateCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()
        cls.total = len(scenarios.load_scenarios().scenarios)

    def setUp(self):
        self.counter = ProcessCounter()
        self.counter.__enter__()
        self.addCleanup(self.counter.__exit__)
        self.at = AppTest.from_file(APP, default_timeout=120).run()
        open_view(self.at, "Validate")

    def run_all(self):
        self.at.button(key="val_run").click().run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])

    @property
    def latest(self):
        return self.at.session_state["val_displayed"]

    def badges(self):
        return " ".join(m.value for m in self.at.markdown if "-badge[" in m.value)


class BeforeAnythingRuns(ValidateCase):
    def test_visiting_the_view_runs_nothing_and_every_scenario_is_not_run(self):
        self.assertEqual(self.counter.runs, 0)
        self.assertNotIn("val_displayed", self.at.session_state)
        text = page_text(self.at)
        self.assertIn("Nothing has been run in this session", text)
        statuses = self.at.table[0].value["Status"].tolist()
        self.assertEqual(statuses, ["Not run"] * self.total)
        self.assertFalse(self.at.button(key="val_run").disabled)
        self.assertIn("Not run yet", self.badges())

    def test_it_says_these_are_scenario_checks_and_not_the_cpp_test_suite(self):
        text = page_text(self.at)
        self.assertIn("NOT the C++ CTest/GoogleTest suite", text)
        self.assertIn("does not show that the repository's tests pass", text)


class AfterARealRun(ValidateCase):
    def setUp(self):
        super().setUp()
        self.run_all()

    def test_one_process_per_scenario_and_all_pass_against_the_real_executable(self):
        self.assertEqual(self.counter.runs, self.total)
        self.assertEqual(self.latest.counts(), {"passed": self.total, "failed": 0, "error": 0, "not_run": 0})
        self.assertIn(f"All {self.total} passed", self.badges())
        self.assertEqual(self.at.session_state["val_launches"], 1)

    def test_each_scenario_has_its_own_expander_with_expected_and_actual_and_provenance(self):
        labels = [e.label for e in self.at.expander]
        self.assertEqual(len([label for label in labels if label.startswith("PASSED - ")]), self.total)
        text = page_text(self.at)
        self.assertIn("Signal requests: expected and actual", text)
        self.assertIn("Buy request, AAPL, event 5", text)                           # expected and actual agree on event 5
        self.assertIn("Where this expectation comes from", labels)

    def test_an_expected_rejection_is_shown_as_the_refusal_that_happened(self):
        text = page_text(self.at)
        self.assertIn("The expected refusal happened", text)
        self.assertIn("The refusal: expected and actual", text)
        self.assertIn("dataset_error", text)
        self.assertIn("config_error", text)
        rejected = [r for r in self.latest.results if r.scenario.outcome == "rejected"]
        self.assertTrue(rejected and all(r.status == "passed" for r in rejected))

    def test_going_to_another_view_and_back_shows_the_same_results_without_rerunning(self):
        before = self.latest
        runs = self.counter.runs
        open_view(self.at, "Explore")
        open_view(self.at, "Compare")
        open_view(self.at, "Validate")
        self.assertIs(self.latest, before)
        self.assertEqual(self.counter.runs, runs)
        self.assertIn(f"All {self.total} passed", self.badges())

    def test_the_results_identify_the_executable_and_scenario_files_they_used(self):
        text = page_text(self.at)
        self.assertIn(self.runner.sha256[:12], text)
        self.assertIn(self.latest.set_identity[:12], text)
        self.assertEqual({r.executable_sha256 for r in self.latest.results}, {self.runner.sha256})

    def test_both_report_downloads_are_offered(self):
        labels = [b.proto.label for b in self.at.get("download_button")]
        self.assertEqual(sorted(labels), ["Download validation report - JSON", "Download validation report - Markdown"])

    def test_pressing_run_again_starts_the_scenarios_again(self):
        self.run_all()
        self.assertEqual(self.counter.runs, 2 * self.total)
        self.assertEqual(self.at.session_state["val_launches"], 2)


class TheDemonstration(ValidateCase):
    def test_off_by_default_so_a_normal_run_has_no_failure(self):
        self.run_all()
        self.assertEqual(self.latest.demonstrations(), ())
        self.assertNotIn("deliberately wrong", page_text(self.at))

    def test_when_asked_it_fails_visibly_and_is_kept_out_of_the_totals(self):
        self.at.checkbox(key="val_include_demo").set_value(True).run()
        self.run_all()
        self.assertEqual(self.counter.runs, self.total + 1)
        (demo,) = self.latest.demonstrations()
        self.assertEqual(demo.status, "failed")
        self.assertEqual(self.latest.counts()["failed"], 0)
        self.assertIn(f"All {self.total} passed", self.badges())
        text = page_text(self.at)
        self.assertIn("what a mismatch looks like", text)
        self.assertIn("signal 1 event_index", text)
        self.assertIn("event_index 2 (sma_2_3) short_sma", text)
        self.assertIn("the expectations below are deliberately wrong", text)
        self.assertTrue(any(e.label.startswith("FAILED - DEMONSTRATION") for e in self.at.expander))

    def test_the_choice_survives_a_visit_to_another_view(self):
        self.at.checkbox(key="val_include_demo").set_value(True).run()
        open_view(self.at, "Explore")
        open_view(self.at, "Validate")
        self.assertTrue(self.at.checkbox(key="val_include_demo").value)


class AControlledWrongExpectation(ValidateCase):
    """The same flow with one expectation edited in a TEMPORARY copy of the scenario files."""

    def setUp(self):
        super().setUp()
        self.tmp = Path(tempfile.mkdtemp(prefix="sl_scen_app_"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        for path in scenarios.SCENARIO_DIR.glob("*.json"):
            shutil.copy(path, self.tmp / path.name)
        target = self.tmp / "01_sma_crossover_timing.json"
        text = target.read_text(encoding="utf-8")
        self.assertIn('"event_index": 4, "symbol": "AAPL", "side": "buy", "signal_id": 1', text)
        target.write_text(text.replace('"event_index": 4, "symbol": "AAPL", "side": "buy", "signal_id": 1',
                                       '"event_index": 5, "symbol": "AAPL", "side": "buy", "signal_id": 1'), encoding="utf-8")
        patcher = mock.patch.object(validate_page, "load_scenarios", lambda: scenarios.load_scenarios(self.tmp))
        patcher.start()
        self.addCleanup(patcher.stop)
        self.at = AppTest.from_file(APP, default_timeout=120).run()
        open_view(self.at, "Validate")
        self.run_all()

    def test_the_wrong_expectation_is_shown_as_a_failed_check_with_expected_and_actual(self):
        self.assertEqual(self.latest.counts(), {"passed": self.total - 1, "failed": 1, "error": 0, "not_run": 0})
        self.assertIn("1 failed", self.badges())
        failed = next(r for r in self.latest.results if r.status == "failed")
        self.assertEqual(failed.scenario.id, "sma_crossover_timing")
        text = page_text(self.at)
        self.assertIn("What did not match", text)
        self.assertIn("signal 1 event_index", text)
        self.assertTrue(any(e.label.startswith("FAILED - SMA crossover") for e in self.at.expander))
        mismatch = failed.mismatches[0]
        self.assertEqual((mismatch.expected, mismatch.actual), ("5", "4"))
        self.assertIn("Buy request, AAPL, event 6", text)                  # what was (wrongly) expected, event numbers from 1
        self.assertIn("Buy request, AAPL, event 5", text)                  # what the tool actually recorded

    def test_the_failure_is_in_the_exported_report(self):
        from strategy_lab_ui import validation_report
        report = validation_report.report_dict(self.latest, validate_page.load_scenarios())
        self.assertEqual(report["summary"]["failed"], 1)
        failed = next(s for s in report["scenarios"] if s["status"] == "failed")
        self.assertEqual(failed["mismatches"][0]["where"], "signal 1 event_index")

    def test_the_checked_in_files_were_not_touched(self):
        text = (scenarios.SCENARIO_DIR / "01_sma_crossover_timing.json").read_text(encoding="utf-8")
        self.assertIn('"event_index": 4, "symbol": "AAPL", "side": "buy", "signal_id": 1', text)


class ErrorsAreShownAsErrors(ValidateCase):
    def test_a_tool_that_cannot_be_run_gives_errors_not_passes_and_not_failures(self):
        def broken(*args, **kwargs):
            raise LabUiError("timeout", "The replay tool did not finish within 60 seconds and was stopped.")
        with mock.patch.object(bridge, "run_replay_multi", broken):
            self.run_all()
        self.assertEqual(self.latest.counts(), {"passed": 0, "failed": 0, "error": self.total, "not_run": 0})
        self.assertIn(f"{self.total} error", self.badges())
        text = page_text(self.at)
        self.assertIn("An ERROR means a check could not be carried out", text)
        self.assertIn("did not finish within 60 seconds", text)
        self.assertFalse([e for e in self.at.expander if e.label.startswith("PASSED")])

    def test_a_rejection_scenario_does_not_pass_when_the_tool_times_out(self):
        def broken(*args, **kwargs):
            raise LabUiError("timeout", "slow")
        with mock.patch.object(bridge, "run_replay_multi", broken):
            self.run_all()
        rejected = [r for r in self.latest.results if r.scenario.outcome == "rejected"]
        self.assertTrue(rejected and all(r.status == "error" for r in rejected))


class Staleness(ValidateCase):
    def test_results_from_another_executable_are_labelled_as_older(self):
        self.run_all()
        other = dataclasses.replace(self.runner, sha256="0" * 64)
        with mock.patch.object(bridge, "find_runner", return_value=other):
            self.at.run()
        self.assertIn("older executable or scenario files", page_text(self.at) + self.badges())
        self.assertEqual(self.counter.runs, self.total, "showing a stale label does not rerun anything")

    def test_results_from_other_scenario_files_are_labelled_as_older(self):
        self.run_all()
        tmp = Path(tempfile.mkdtemp(prefix="sl_scen_app_"))
        self.addCleanup(shutil.rmtree, tmp, True)
        for path in scenarios.SCENARIO_DIR.glob("*.json"):
            shutil.copy(path, tmp / path.name)
        (tmp / "01_sma_crossover_timing.json").write_bytes(
            (tmp / "01_sma_crossover_timing.json").read_bytes().replace(b"Buy on bar 5", b"Buy on bar five"))
        with mock.patch.object(validate_page, "load_scenarios", lambda: scenarios.load_scenarios(tmp)):
            self.at.run()
        self.assertIn("older executable or scenario files", page_text(self.at) + self.badges())


if __name__ == "__main__":
    unittest.main()
