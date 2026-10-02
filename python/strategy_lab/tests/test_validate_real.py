"""Validate against the REAL strategy_lab_replay (skipped, with a reason, if it is not built), and the exportable report.

The checked-in scenarios hold expectations derived by hand. Here every one of them is run for real, and then each is corrupted
on purpose, one at a time, to prove the comparison notices (so a pass means something).
"""

import hashlib
import json
import unittest
from dataclasses import replace

from support import real_runner

from strategy_lab_ui import scenarios, validate, validation_report
from strategy_lab_ui.scenarios import ExpectedProblem, ExpectedSignal, ScenarioSet
from strategy_lab_ui.validate import ERROR, FAILED, PASSED


class RealExecutable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()
        cls.set = scenarios.load_scenarios()
        cls.vrun = validate.run_all(cls.runner, cls.set, 1, include_demo=True)

    def test_every_checked_in_scenario_passes(self):
        bad = [(r.scenario.id, r.status, r.summary, [(m.where, m.expected, m.actual) for m in r.mismatches])
               for r in self.vrun.results if not r.demonstration and r.status != PASSED]
        self.assertEqual(bad, [])
        self.assertEqual(self.vrun.counts(), {"passed": len(self.set.scenarios), "failed": 0, "error": 0, "not_run": 0})

    def test_each_result_names_the_dataset_bytes_and_the_executable_it_used(self):
        for r in self.vrun.results:
            data = (self.set.root / r.scenario.dataset_path).read_bytes()
            self.assertEqual(r.dataset_sha256, hashlib.sha256(data).hexdigest(), r.scenario.id)
            self.assertEqual(r.executable_sha256, self.runner.sha256, r.scenario.id)
        ran = [r for r in self.vrun.results if r.scenario.outcome == "result"]
        self.assertTrue(all(r.run_id and len(r.result_sha256 or "") == 64 for r in ran))
        self.assertEqual(self.vrun.tool["tool"], "strategy_lab_replay")
        self.assertEqual(self.vrun.set_identity, self.set.identity)

    def test_the_rejection_scenarios_were_really_refused_by_the_tool(self):
        refused = [r for r in self.vrun.results if r.scenario.outcome == "rejected"]
        self.assertGreaterEqual(len(refused), 4)
        for r in refused:
            self.assertEqual(r.status, PASSED, r.scenario.id)
            self.assertIn(r.actual_rejection["exit_status"], (2, 3))
            self.assertTrue(r.actual_rejection["message"])

    def test_the_injected_demonstration_fails_with_exactly_its_two_wrong_expectations(self):
        (demo,) = self.vrun.demonstrations()
        self.assertEqual(demo.status, FAILED)
        self.assertEqual([m.where for m in demo.mismatches], ["signal 1 event_index", "event_index 2 (sma_2_3) short_sma"])
        signal = demo.mismatches[0]
        self.assertEqual((signal.expected, signal.actual), ("5", "4"))            # one event too late
        self.assertEqual(demo.mismatches[1].actual, "1.5")
        self.assertEqual(self.vrun.counts()[FAILED], 0, "the demonstration is not counted in the totals")

    def test_a_deliberately_wrong_expectation_is_detected_in_every_scenario(self):
        undetected = []
        for scenario in self.set.scenarios:
            if scenario.outcome == "result":
                signals = tuple(scenario.signals or ())
                wrong = (replace(scenario, signals=(replace(signals[0], event_index=signals[0].event_index + 1), *signals[1:]))
                         if signals else
                         replace(scenario, signals=(ExpectedSignal(scenario.strategies[0].strategy_id, 0, "AAPL", "buy"),)))
            else:
                rejection = scenario.rejection
                wrong = replace(scenario, rejection=replace(rejection, error_code=rejection.error_code + "_wrong"))
            result = validate.run_scenario(self.runner, wrong, self.set.root)
            if result.status != FAILED:
                undetected.append((scenario.id, result.status))
        self.assertEqual(undetected, [])

    def test_a_wrong_problem_line_in_a_dataset_rejection_is_detected(self):
        scenario = next(s for s in self.set.scenarios if "dataset_rejection" in s.covers)
        wrong = replace(scenario, rejection=replace(scenario.rejection, problems=(replace(scenario.rejection.problems[0], line=99),)))
        result = validate.run_scenario(self.runner, wrong, self.set.root)
        self.assertEqual(result.status, FAILED)
        self.assertEqual(result.mismatches[0].where, "problem 1 line")

    def test_a_wrong_numeric_expectation_is_detected_even_when_only_one_digit_differs(self):
        scenario = self.set.by_id("mr_rearm_and_flip")
        events = []
        for expectation in scenario.events:
            numbers = dict(expectation.indicators)
            if "z_score" in numbers and expectation.first == 3:
                bad = replace(numbers["z_score"], value=numbers["z_score"].value + scenarios.Fraction(1, 10 ** 9))
                expectation = replace(expectation, indicators=tuple((n, bad if n == "z_score" else v) for n, v in expectation.indicators))
            events.append(expectation)
        result = validate.run_scenario(self.runner, replace(scenario, events=tuple(events)), self.set.root)
        self.assertEqual(result.status, FAILED)
        self.assertIn("z_score", result.mismatches[0].where)
        self.assertIn("irrational", result.mismatches[0].note)                    # the tolerance's written justification


class ReportContent(unittest.TestCase):
    """A run with one pass, one real failure and one error, so the report has to carry all three."""

    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()
        cls.set = scenarios.load_scenarios()
        good = cls.set.by_id("sma_crossover_timing")
        bad = replace(cls.set.by_id("mr_rearm_and_flip"), id="deliberately_wrong",
                      signals=(ExpectedSignal("mr_4", 0, "AAPL", "sell"),))
        gone = replace(cls.set.by_id("trade_rows_are_ignored"), id="missing_dataset", dataset_path="datasets/does_not_exist.csv")
        sset = ScenarioSet((good, bad, gone), cls.set.root, cls.set.identity, cls.set.files)
        cls.vrun = validate.run_all(cls.runner, sset, 4, include_demo=False)
        cls.sset = sset
        cls.report = json.loads(validation_report.report_json(cls.vrun, sset))

    def test_the_three_outcomes_are_classified(self):
        self.assertEqual([r.status for r in self.vrun.results], [PASSED, FAILED, ERROR])

    def test_the_report_states_its_scope_and_that_it_is_not_the_cpp_suite(self):
        self.assertIn("NOT the C++ CTest/GoogleTest suite", self.report["scope_note"])
        self.assertIn("does not show that the repository's tests pass", self.report["scope_note"])

    def test_the_summary_counts_and_overall_verdict(self):
        s = self.report["summary"]
        self.assertEqual((s["scenarios"], s["passed"], s["failed"], s["error"]), (3, 1, 1, 1))
        self.assertIn("errors", s["overall"])

    def test_the_report_identifies_the_executable_and_the_scenario_files(self):
        exe = self.report["executable"]
        self.assertEqual(exe["sha256"], self.runner.sha256)
        self.assertEqual(exe["size_bytes"], self.runner.size)
        self.assertEqual(exe["tool"], "strategy_lab_replay")
        self.assertEqual(self.report["scenario_files"]["identity_sha256"], self.set.identity)
        self.assertEqual(self.report["scenario_files"]["directory"], "tests/fixtures/strategy_lab/scenarios")
        self.assertNotIn("\\", exe["file_name"])

    def test_each_scenario_carries_its_dataset_configuration_expectation_and_provenance(self):
        passed = self.report["scenarios"][0]
        self.assertEqual(passed["dataset"]["path"], "datasets/sma_crossover.csv")
        self.assertEqual(len(passed["dataset"]["sha256_given_to_tool"]), 64)
        self.assertEqual(passed["configuration"][0]["parameters"]["short_window"], "2")
        self.assertEqual(passed["expected"]["signals"][0]["event_index"], 4)
        self.assertEqual([s["event_index"] for s in passed["actual_signals"]], [4, 7])
        self.assertIn("by hand", passed["provenance_of_the_expectation"]["derived_by"])
        self.assertTrue(passed["provenance_of_the_expectation"]["cross_checks"])
        self.assertEqual(passed["executable_sha256"], self.runner.sha256)

    def test_a_failure_lists_expected_actual_and_where(self):
        failed = self.report["scenarios"][1]
        self.assertEqual(failed["status"], "failed")
        wheres = [m["where"] for m in failed["mismatches"]]
        self.assertIn("number of signal requests", wheres)
        self.assertTrue(all({"where", "expected", "actual", "note"} <= set(m) for m in failed["mismatches"]))

    def test_an_error_carries_the_error_and_no_pretend_comparison(self):
        error = self.report["scenarios"][2]
        self.assertEqual(error["status"], "error")
        self.assertEqual(error["error"]["kind"], "input_missing")
        self.assertEqual(error["mismatches"], [])

    def test_the_markdown_has_the_caveat_the_table_and_the_failures(self):
        text = validation_report.report_markdown(self.vrun, self.sset).decode("utf-8")
        self.assertIn("NOT the C++ CTest/GoogleTest suite", text)
        self.assertIn("| Passed | `sma_crossover_timing` |", text)
        self.assertIn("| Failed | `deliberately_wrong` |", text)
        self.assertIn("## Failures and errors", text)
        self.assertIn("number of signal requests", text)
        self.assertIn(self.runner.sha256, text)
        self.assertNotIn("\r", text)

    def test_the_same_run_always_gives_the_same_report_bytes(self):
        self.assertEqual(validation_report.report_json(self.vrun, self.sset), validation_report.report_json(self.vrun, self.sset))
        self.assertEqual(validation_report.report_markdown(self.vrun, self.sset), validation_report.report_markdown(self.vrun, self.sset))

    def test_a_demonstration_is_reported_separately_and_never_counted(self):
        run = validate.run_all(self.runner, self.set, 5, include_demo=True)
        report = json.loads(validation_report.report_json(run, self.set))
        self.assertEqual(report["summary"]["scenarios"], len(self.set.scenarios))
        self.assertEqual(report["summary"]["failed"], 0)
        self.assertTrue(report["demonstration"]["included"])
        self.assertTrue(report["demonstration"]["results"][0]["failed_as_designed"])
        self.assertIn("all scenarios passed", report["summary"]["overall"])
        text = validation_report.report_markdown(run, self.set).decode("utf-8")
        self.assertIn("Demonstration (deliberately wrong expectations; expected to fail)", text)


if __name__ == "__main__":
    unittest.main()
