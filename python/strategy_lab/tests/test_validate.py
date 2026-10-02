"""Validate's classification, with controlled fakes: when is a scenario PASSED, FAILED or ERROR?

  PASSED  the tool answered and every expectation matched (a rejection scenario passes only on the EXPECTED rejection).
  FAILED  the tool answered coherently and differently (it accepted what should be refused, refused what should run, ...).
  ERROR   the check could not be carried out: timeout, crash, missing/unstartable executable, malformed or inconsistent output,
          an internal error (exit 4), an unexpected exit status, a changed fixture. Never a pass, never "an expected rejection".

The bridge call is replaced by a fake that returns a hand-built document or raises the LabUiError the bridge would raise.
"""

import copy
import hashlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from support import ev, make_document, outcome_of

from strategy_lab_ui import bridge, scenarios, validate
from strategy_lab_ui.errors import LabUiError, Problem
from strategy_lab_ui.scenarios import ScenarioSet
from strategy_lab_ui.validate import ERROR, FAILED, PASSED

DATA = b"symbol,exchange_time,type,price\nAAPL,2026-01-05T14:30:00Z,bar,3\n"
RUNNER = bridge.Runner(("fake",), "fake.exe", 1, 1, "e" * 64)


def result_dict() -> dict:
    return {
        "schema": "strategy_lab.scenario/1", "id": "tiny", "title": "t", "purpose": "p", "covers": ["x"],
        "dataset": {"path": "datasets/d.csv", "sha256_lf": hashlib.sha256(DATA).hexdigest()},
        "strategies": [{"kind": "sma_crossover", "params": {"strategy_id": "s1", "short_window": "2", "long_window": "3", "symbols": "AAPL"}}],
        "provenance": {"derived_by": "by hand", "sources": ["doc"], "derivation": ["step"], "cross_checks": []},
        "tolerances": {"loose": {"abs_tol": "1e-9", "why": "test tolerance"}},
        "expect": {"outcome": "result", "warnings": [],
                   "signals": [{"strategy": "s1", "event_index": 2, "symbol": "AAPL", "side": "buy", "signal_id": 1}],
                   "events": [
                       {"strategy": "s1", "event_index": 0, "unavailable": {"short_sma": "warming_up"}},
                       {"strategy": "s1", "event_index": 2, "verdict": "evaluated", "reason": "crossover_buy", "action": "buy",
                        "window_fill": 3, "indicators": {"short_sma": {"exact": "5/2"}, "long_sma": {"exact": "2"}}}]}}


def rejection_dict() -> dict:
    raw = result_dict()
    raw["id"] = "refused"
    raw["expect"] = {"outcome": "rejected", "exit_status": 3, "error_code": "dataset_error", "message_contains": ["price"],
                     "problems": [{"line": 3, "column": "price"}], "total_problems": 1}
    return raw


def good_document() -> dict:
    return make_document([
        ev("AAPL", 30, 3, verdict="warming_up", reason="warming_up", unavailable={"short_sma": "warming_up"}),
        ev("AAPL", 31, 2, reason="baseline_established", indicators={"short_sma": 1.5, "long_sma": 2}),
        ev("AAPL", 32, 1, reason="crossover_buy", signals=[(0, 1, "buy")], indicators={"short_sma": 2.5, "long_sma": 2})],
        strategies=("s1",))


def dataset_error(**kw) -> LabUiError:
    fields = dict(exit_code=3, runner_code="dataset_error", problems=(Problem("price 'abc' is not a number", 3, "price"),),
                  total_problems=1)
    fields.update(kw)
    return LabUiError("dataset_invalid", "the price must be a number", **fields)


class Harness(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="sl_validate_"))
        self.addCleanup(shutil.rmtree, self.root, True)
        (self.root / "datasets").mkdir()
        (self.root / "datasets" / "d.csv").write_bytes(DATA)
        self.given = []

    def scenario(self, raw=None, **mutate):
        raw = copy.deepcopy(raw or result_dict())
        for key, value in mutate.items():
            raw[key] = value
        return scenarios.parse_scenario(raw, "t.json")

    def run_with(self, scenario, outcome=None, raises=None):
        def fake(runner, *, dataset, dataset_sha256, strategies, **kw):
            self.given.append((dataset, dataset_sha256, strategies))
            if raises is not None:
                raise raises
            return outcome
        with mock.patch.object(bridge, "run_replay_multi", fake):
            return validate.run_scenario(RUNNER, scenario, self.root)

    def doc(self, mutate=None):
        document = good_document()
        if mutate:
            mutate(document["result"])
        return outcome_of(document)

    def where(self, result):
        return [m.where for m in result.mismatches]


class Matching(Harness):
    def test_a_matching_answer_passes_and_counts_its_checks(self):
        result = self.run_with(self.scenario(), self.doc())
        self.assertEqual(result.status, PASSED, result.mismatches)
        self.assertGreater(result.checks, 10)
        self.assertEqual(result.mismatches, ())
        self.assertIn(f"All {result.checks} checks matched", result.summary)

    def test_the_result_is_stamped_with_the_dataset_bytes_and_the_executable_it_used(self):
        result = self.run_with(self.scenario(), self.doc())
        self.assertEqual(result.dataset_sha256, hashlib.sha256(DATA).hexdigest())
        self.assertEqual(result.executable_sha256, "e" * 64)
        self.assertEqual(result.run_id, "0123456789abcdef")
        self.assertEqual(result.tool["tool"], "strategy_lab_replay")

    def test_the_tool_is_given_the_scenarios_dataset_bytes_and_each_strategy_with_its_parameters(self):
        self.run_with(self.scenario(), self.doc())
        data, sha, strategies = self.given[0]
        self.assertEqual((data, sha), (DATA, hashlib.sha256(DATA).hexdigest()))
        self.assertEqual(strategies[0][0], "sma_crossover")
        self.assertEqual(dict(strategies[0][1])["short_window"], "2")


def variant(**changes):
    """The good document's events, with per-event replacements (e1=dict(...) replaces keywords of the second event), parsed."""
    specs = [dict(symbol="AAPL", minute=30, price=3, verdict="warming_up", reason="warming_up",
                  unavailable={"short_sma": "warming_up"}),
             dict(symbol="AAPL", minute=31, price=2, reason="baseline_established", indicators={"short_sma": 1.5, "long_sma": 2}),
             dict(symbol="AAPL", minute=32, price=1, reason="crossover_buy", signals=[(0, 1, "buy")],
                  indicators={"short_sma": 2.5, "long_sma": 2})]
    for index, replacement in changes.items():
        specs[int(index[1:])].update(replacement)
    events = [ev(s.pop("symbol"), s.pop("minute"), s.pop("price"), **s) for s in specs]
    return outcome_of(make_document(events, strategies=("s1",)))


class SignalMismatches(Harness):
    def failed(self, outcome, expect_where):
        result = self.run_with(self.scenario(), outcome)
        self.assertEqual(result.status, FAILED, "a wrong answer must fail")
        self.assertIn(expect_where, self.where(result))
        return result

    def test_a_signal_on_the_wrong_event(self):
        self.failed(variant(e1=dict(signals=[(0, 1, "buy")]), e2=dict(signals=[])), "signal 1 event_index")

    def test_the_wrong_side(self):
        self.failed(variant(e2=dict(signals=[(0, 1, "sell")])), "signal 1 side")

    def test_the_wrong_signal_id(self):
        self.failed(variant(e2=dict(signals=[(0, 7, "buy")])), "signal 1 signal_id")

    def test_a_missing_signal(self):
        result = self.failed(variant(e2=dict(signals=[])), "number of signal requests")
        self.assertEqual(result.actual_signals, ())

    def test_an_extra_signal(self):
        self.failed(variant(e1=dict(signals=[(0, 2, "sell")])), "number of signal requests")

    def test_the_wrong_symbol(self):
        self.failed(variant(e2=dict(symbol="MSFT")), "signal 1 symbol")

    def test_extra_or_missing_warnings(self):
        def warn(r):
            r["warnings"].append({"code": "fewer_bars_than_warmup", "message": "m"})
        self.failed(self.doc(warn), "warning codes")


class NumbersAndUnavailableValues(Harness):
    def test_exact_means_exact(self):
        def nudge(r):
            r["events"][2]["results"][0]["indicators"]["short_sma"] = 2.5000000000000004
        result = self.run_with(self.scenario(), self.doc(nudge))
        self.assertEqual(result.status, FAILED)
        m = next(m for m in result.mismatches if m.where.endswith("short_sma"))
        self.assertIn("5/2", m.expected)
        self.assertEqual(m.actual, "2.5000000000000004")

    def test_an_approximation_passes_inside_its_tolerance_and_fails_outside(self):
        raw = result_dict()
        raw["expect"]["events"][1]["indicators"]["short_sma"] = {"approx": "2.5", "tolerance": "loose"}
        inside, outside = [self.run_with(self.scenario(raw), self.doc(lambda r, v=v: r["events"][2]["results"][0]["indicators"].update(short_sma=v)))
                           for v in (2.5 + 5e-10, 2.5 + 1e-8)]
        self.assertEqual(inside.status, PASSED, inside.mismatches)
        self.assertEqual(outside.status, FAILED)
        self.assertIn("test tolerance", outside.mismatches[0].note)            # the tolerance's justification travels with it

    def test_a_value_expected_to_be_unavailable_must_be_absent_never_zero(self):
        def zero(r):
            r["events"][0]["results"][0]["indicators"]["short_sma"] = 0
            del r["events"][0]["results"][0]["unavailable"]["short_sma"]
        result = self.run_with(self.scenario(), self.doc(zero))
        self.assertEqual(result.status, FAILED)
        self.assertIn("never zero", [m.note for m in result.mismatches][0])

    def test_an_unavailable_value_must_carry_the_documented_reason(self):
        def other_reason(r):
            r["events"][0]["results"][0]["unavailable"]["short_sma"] = "event_ignored"
        result = self.run_with(self.scenario(), self.doc(other_reason))
        self.assertEqual(result.status, FAILED)
        self.assertEqual((result.mismatches[0].expected, result.mismatches[0].actual), ("warming_up", "event_ignored"))

    def test_a_number_expected_where_the_tool_says_unavailable_is_a_failure_that_says_why(self):
        def vanish(r):
            res = r["events"][2]["results"][0]
            del res["indicators"]["short_sma"]
            res["unavailable"]["short_sma"] = "constant_window"
        result = self.run_with(self.scenario(), self.doc(vanish))
        self.assertEqual(result.status, FAILED)
        self.assertIn("unavailable (constant_window)", [m.actual for m in result.mismatches])

    def test_verdict_reason_action_window_and_event_range_are_checked(self):
        for field, value in (("verdict", "ignored"), ("reason", "same_side"), ("action", "none")):
            def change(r, f=field, v=value):
                r["events"][2]["results"][0][f] = v
            self.assertEqual(self.run_with(self.scenario(), self.doc(change)).status, FAILED, field)

        def fill(r):
            r["events"][2]["results"][0]["window"]["fill"] = 2
        self.assertEqual(self.run_with(self.scenario(), self.doc(fill)).status, FAILED)
        raw = result_dict()
        raw["expect"]["events"][0]["event_index"] = 99
        result = self.run_with(self.scenario(raw), self.doc())
        self.assertEqual(result.status, FAILED)
        self.assertIn("the result has only 3 events", result.mismatches[0].actual)

    def test_a_wrong_strategy_id_in_the_result_fails(self):
        raw = result_dict()
        raw["strategies"][0]["params"]["strategy_id"] = "other"
        raw["expect"]["signals"][0]["strategy"] = "other"
        raw["expect"]["events"] = []
        result = self.run_with(self.scenario(raw), self.doc())
        self.assertEqual(result.status, FAILED)
        self.assertIn("strategy ids", self.where(result))


class ExpectedRejections(Harness):
    def test_the_expected_refusal_passes(self):
        result = self.run_with(self.scenario(rejection_dict()), raises=dataset_error())
        self.assertEqual(result.status, PASSED, result.mismatches)
        self.assertIn("expected refusal happened", result.summary)
        self.assertEqual(result.actual_rejection["exit_status"], 3)

    def test_a_refusal_for_the_wrong_reason_fails(self):
        for label, error, expect_where in (
                ("line", dataset_error(problems=(Problem("m", 4, "price"),)), "problem 1 line"),
                ("column", dataset_error(problems=(Problem("m", 3, "exchange_time"),)), "problem 1 column"),
                ("code", dataset_error(runner_code="limit_exceeded"), "error code"),
                ("status", LabUiError("runner_rejected", "the price must be a number", exit_code=2, runner_code="dataset_error",
                                      problems=(Problem("m", 3, "price"),), total_problems=1), "exit status"),
                ("count", dataset_error(problems=(Problem("m", 3, "price"), Problem("m", 4, "price")), total_problems=2),
                 "number of problems listed"),
                ("message", dataset_error(problems=(Problem("m", 3, "price"),)).__class__(
                    "dataset_invalid", "something else", exit_code=3, runner_code="dataset_error",
                    problems=(Problem("m", 3, "price"),), total_problems=1), "message")):
            result = self.run_with(self.scenario(rejection_dict()), raises=error)
            self.assertEqual(result.status, FAILED, label)
            self.assertIn(expect_where, self.where(result), label)

    def test_accepting_what_should_be_refused_is_a_failure(self):
        result = self.run_with(self.scenario(rejection_dict()), self.doc())
        self.assertEqual(result.status, FAILED)
        self.assertIn("accepted input that this scenario expects it to refuse", result.summary)
        self.assertIn("a replay result", result.mismatches[0].actual)

    def test_refusing_what_should_run_is_a_failure(self):
        result = self.run_with(self.scenario(), raises=dataset_error())
        self.assertEqual(result.status, FAILED)
        self.assertIn("refused input that this scenario expects it to replay", result.summary)
        self.assertEqual(result.actual_rejection["error_code"], "dataset_error")


class ErrorsAreNeverPassesAndNeverExpectedRejections(Harness):
    KINDS = ("timeout", "spawn_failed", "executable_missing", "executable_invalid", "output_too_large", "malformed_output",
             "unsupported_schema", "invalid_structure", "result_mismatch", "unexpected_exit", "runner_internal_error",
             "input_unencodable")

    def test_none_of_these_can_pass_even_a_scenario_that_expects_a_refusal(self):
        for kind in self.KINDS:
            for raw in (result_dict(), rejection_dict()):
                error = LabUiError(kind, f"{kind} happened", exit_code=3 if kind != "runner_internal_error" else 4,
                                   runner_code="dataset_error", problems=(Problem("m", 3, "price"),), total_problems=1)
                result = self.run_with(self.scenario(raw), raises=error)
                self.assertEqual(result.status, ERROR, f"{kind} for {raw['expect']['outcome']}")
                self.assertIs(result.error, error)
                self.assertEqual(result.mismatches, ())
                self.assertIsNone(result.actual_rejection)
                self.assertIn(f"{kind} happened", result.summary)

    def test_an_internal_error_with_the_status_a_rejection_expects_is_still_an_error(self):
        raw = rejection_dict()
        raw["expect"]["exit_status"] = 3
        error = LabUiError("runner_internal_error", "boom", exit_code=3, runner_code="dataset_error")
        self.assertEqual(self.run_with(self.scenario(raw), raises=error).status, ERROR)


class RealProcessesThatMisbehave(Harness):
    """The real bridge, with a stand-in 'executable' that misbehaves in each way (no mocking of the bridge)."""

    def runner(self, code: str) -> bridge.Runner:
        return bridge.Runner((sys.executable, "-c", code), sys.executable, 1, 1, "f" * 64)

    def classify(self, code: str, raw=None):
        return validate.run_scenario(self.runner(code), self.scenario(raw), self.root)

    def test_an_executable_that_cannot_start_is_an_error(self):
        runner = bridge.Runner((str(self.root / "no_such_program.exe"),), "x", 1, 1, "f" * 64)
        self.assertEqual(validate.run_scenario(runner, self.scenario(rejection_dict()), self.root).status, ERROR)

    def test_a_crash_is_an_error(self):
        result = self.classify("import os; os._exit(3221225477)", rejection_dict())
        self.assertEqual((result.status, result.error.kind), (ERROR, "unexpected_exit"))

    def test_an_unexpected_exit_status_is_an_error(self):
        self.assertEqual(self.classify("import sys; sys.exit(7)").error.kind, "unexpected_exit")

    def test_output_that_is_not_a_json_document_is_an_error(self):
        self.assertEqual(self.classify("print('hello')").error.kind, "malformed_output")

    @staticmethod
    def error_document_code(document_status: int, process_status: int, message: str, problems: str) -> str:
        """A program that writes an error document as UTF-8 bytes with LF (text-mode stdout would write CRLF on Windows)."""
        return ("import json,sys; sys.stdout.buffer.write((json.dumps({'schema':'strategy_lab.error','schema_version':'1.0',"
                f"'error':{{'code':'dataset_error','exit_status':{document_status},'message':'{message}',"
                f"'total_problems':len({problems}),'problems':{problems}}}}})+'\\n').encode()); sys.exit({process_status})")

    def test_a_well_formed_refusal_from_a_real_process_is_recognised(self):
        code = self.error_document_code(3, 3, "the price must be a number", "[{'line':3,'column':'price','message':'x'}]")
        result = self.classify(code, rejection_dict())
        self.assertEqual(result.status, PASSED, result.mismatches)

    def test_an_error_document_whose_status_disagrees_with_the_exit_status_is_an_error(self):
        code = self.error_document_code(2, 3, "m", "[]")
        self.assertEqual(self.classify(code, rejection_dict()).status, ERROR)


class FixtureIdentity(Harness):
    def test_a_changed_dataset_is_an_error_not_a_failure_of_the_strategy(self):
        (self.root / "datasets" / "d.csv").write_bytes(DATA + b"AAPL,2026-01-05T14:31:00Z,bar,4\n")
        result = self.run_with(self.scenario(), self.doc())
        self.assertEqual((result.status, result.error.kind), (ERROR, "fixture_changed"))
        self.assertEqual(self.given, [], "the tool must not be run on a file the expectations were not written for")

    def test_a_crlf_checkout_of_the_same_file_still_matches_its_pin(self):
        (self.root / "datasets" / "d.csv").write_bytes(DATA.replace(b"\n", b"\r\n"))
        result = self.run_with(self.scenario(), self.doc())
        self.assertEqual(result.status, PASSED)
        self.assertEqual(self.given[0][0], DATA.replace(b"\n", b"\r\n"))          # the tool gets the bytes as they are on disk

    def test_a_missing_dataset_is_an_error(self):
        (self.root / "datasets" / "d.csv").unlink()
        self.assertEqual(self.run_with(self.scenario(), self.doc()).error.kind, "input_missing")


class DemonstrationAndRunAll(Harness):
    def set_of(self, *raws):
        parsed = tuple(self.scenario(r) for r in raws)
        return ScenarioSet(parsed, self.root, "i" * 64, tuple(f"{i}.json" for i in range(len(parsed))))

    def run_all(self, sset, results, **kw):
        queue = list(results)

        def fake(runner, **kwargs):
            item = queue.pop(0)
            if isinstance(item, Exception):
                raise item
            return item

        with mock.patch.object(bridge, "run_replay_multi", fake):
            return validate.run_all(RUNNER, sset, 1, **kw)

    def demo_ready_set(self):
        raw = result_dict()
        raw["id"] = scenarios.DEMO_BASE
        raw["expect"]["events"][1]["indicators"] = {"short_sma": {"exact": "3/2"}}      # the demo shifts a short_sma it finds
        raw["expect"]["events"] = [{"strategy": "s1", "event_index": 1, "indicators": {"short_sma": {"exact": "3/2"}}}]
        return self.set_of(raw)

    def test_the_demonstration_fails_on_purpose_and_is_not_counted_in_the_totals(self):
        run = self.run_all(self.demo_ready_set(), [self.doc(), self.doc()], include_demo=True)
        self.assertEqual([r.scenario.id for r in run.results], [scenarios.DEMO_BASE, scenarios.DEMO_ID])
        self.assertEqual([r.status for r in run.results], [PASSED, FAILED])
        self.assertTrue(run.results[1].demonstration)
        self.assertEqual(run.counts(), {"passed": 1, "failed": 0, "error": 0, "not_run": 0})
        self.assertEqual(len(run.demonstrations()), 1)

    def test_a_demonstration_that_passes_means_the_comparison_is_broken_and_is_reported_as_an_error(self):
        sset = self.demo_ready_set()
        passing_demo = scenarios.injected_mismatch(sset.scenarios[0])
        # An answer that satisfies the deliberately wrong expectation: the signal on event 3 and short_sma 2.
        document = good_document()
        document["result"]["events"][2]["results"][0].update(signal_ids=[], action="none")
        document["result"]["events"][1]["results"][0]["indicators"]["short_sma"] = 2
        document["result"]["signals"][0]["event_index"] = 3
        result = self.run_with_demo(passing_demo, document)
        self.assertEqual(result.status, ERROR)
        self.assertIn("NOT detected", result.summary)

    def run_with_demo(self, demo, document):
        # The wrong expectation is "signal 1 on event_index 3"; build a result whose signal sits there.
        return self.run_with(demo, outcome_of(self._forged(document)))

    def _forged(self, document):
        # Make the document internally consistent: move signal 1 from event 2 to a new fourth event.
        result = document["result"]
        extra = copy.deepcopy(result["events"][2])
        extra["index"] = 3
        extra["source_line"] = 5
        extra["exchange_time"] = "2026-01-05T14:33:00.000000000Z"
        extra["results"][0].update(signal_ids=[1], action="buy")
        result["events"][2]["results"][0].update(signal_ids=[], action="none")
        result["events"].append(extra)
        result["signals"][0].update(event_index=3, created_at=extra["exchange_time"])
        result["input"]["dataset"]["rows"] = 4
        result["summary"]["events"] = 4
        return document

    def test_without_the_flag_no_demonstration_is_run(self):
        run = self.run_all(self.demo_ready_set(), [self.doc()], include_demo=False)
        self.assertEqual([r.scenario.id for r in run.results], [scenarios.DEMO_BASE])
        self.assertFalse(run.include_demo)

    def test_run_all_reports_progress_in_order_and_keeps_going_after_an_error(self):
        seen = []
        sset = self.set_of(result_dict(), dict(result_dict(), id="second"))
        run = self.run_all(sset, [LabUiError("timeout", "slow"), self.doc()], progress=lambda i, s: seen.append((i, s)))
        self.assertEqual(seen, [("tiny", "running"), ("tiny", "done"), ("second", "running"), ("second", "done")])
        self.assertEqual([r.status for r in run.results], [ERROR, PASSED])
        self.assertEqual(run.counts(), {"passed": 1, "failed": 0, "error": 1, "not_run": 0})
        self.assertEqual(run.tool["tool"], "strategy_lab_replay")
        self.assertEqual(run.set_identity, "i" * 64)


if __name__ == "__main__":
    unittest.main()
