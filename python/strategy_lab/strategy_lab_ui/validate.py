"""Scenario validation: run the checked-in scenarios through the REAL strategy_lab_replay and compare with the expectations.

What this is: a demo-scale check that the executable behaves as the strategy documents say, on small hand-derived cases. What it
is not: the C++ GoogleTest/CTest suite. Passing every scenario says nothing about the repository's other tests.

Classification, the part that must never be loose:
  PASSED  the tool answered and the answer matched every expectation (a rejection scenario passes only on the EXPECTED rejection).
  FAILED  the tool answered coherently and the answer differs from the expectation (including: it accepted what should be
          refused, or refused what should run).
  ERROR   the check could not be carried out reliably: the executable is missing or cannot start, it timed out, crashed,
          exited with an unexpected status, wrote malformed or inconsistent output, reported an internal error (exit 4), or the
          scenario's own dataset is not the file the scenario was written for. An ERROR is never a pass, never a failure of the
          strategies, and never an "expected rejection".

Only `run_all` (behind the Run button) starts a process. Nothing here computes a strategy decision: it only compares recorded
output with expected values written by hand.
"""

from __future__ import annotations

import hashlib
import math
import time
from dataclasses import dataclass
from fractions import Fraction
from typing import Any, Callable, Mapping, Sequence

from . import bridge, state
from .bridge import Runner
from .errors import LabUiError
from .scenarios import (DEMO_BASE, EventExpect, ExpectedRejection, NumberExpect, Scenario, ScenarioSet, injected_mismatch)
from .schema import Event, ReplayDocument, StrategyEventResult

NOT_RUN, RUNNING, PASSED, FAILED, ERROR = "not_run", "running", "passed", "failed", "error"
STATUS_TEXT = {NOT_RUN: "Not run", RUNNING: "Running", PASSED: "Passed", FAILED: "Failed", ERROR: "Error"}
REJECTION_KINDS = {"runner_rejected": 2, "dataset_invalid": 3}      # a well-formed error document from the tool
SCOPE_NOTE = ("These are scenario checks: a few checked-in demo scenarios run through the real strategy_lab_replay executable "
              "and compared with expectations derived by hand. They are NOT the C++ CTest/GoogleTest suite, and passing them "
              "does not show that the repository's tests pass.")


@dataclass(frozen=True)
class Mismatch:
    where: str
    expected: str
    actual: str
    note: str = ""


@dataclass(frozen=True)
class ActualSignal:
    strategy: str
    event_index: int
    symbol: str
    side: str
    signal_id: int


@dataclass(frozen=True, eq=False)
class ScenarioResult:
    """One scenario's outcome, self-describing: which dataset bytes and which executable it used."""

    scenario: Scenario
    status: str
    summary: str
    checks: int
    mismatches: tuple[Mismatch, ...]
    actual_signals: tuple[ActualSignal, ...] | None
    actual_rejection: Mapping[str, Any] | None
    error: LabUiError | None
    dataset_sha256: str                   # of the bytes the tool was given
    executable_sha256: str
    run_id: str | None
    result_sha256: str | None
    duration_s: float
    finished_at: str
    tool: Mapping[str, Any]               # tool, version, compiler and build the executable reported in its own output

    @property
    def demonstration(self) -> bool:
        return self.scenario.demonstration


@dataclass(frozen=True, eq=False)
class ValidationRun:
    results: tuple[ScenarioResult, ...]
    runner: Runner
    set_identity: str                     # of the scenario files that were run
    number: int
    started_at: str
    finished_at: str
    include_demo: bool
    tool: Mapping[str, Any]               # provenance of the executable's own output (tool, version, compiler, build)

    def counts(self) -> dict[str, int]:
        """Normal scenarios only: the opt-in demonstration is reported on its own, never mixed into the totals."""
        out = {PASSED: 0, FAILED: 0, ERROR: 0, NOT_RUN: 0}
        for r in self.results:
            if not r.demonstration:
                out[r.status] = out.get(r.status, 0) + 1
        return out

    def demonstrations(self) -> tuple[ScenarioResult, ...]:
        return tuple(r for r in self.results if r.demonstration)


# ---- comparing -----------------------------------------------------------------------------------------------

class _Checks:
    """Counts every assertion made and keeps the ones that failed."""

    def __init__(self) -> None:
        self.count = 0
        self.mismatches: list[Mismatch] = []

    def equal(self, where: str, expected: Any, actual: Any, note: str = "") -> None:
        self.count += 1
        if expected != actual:
            self.mismatches.append(Mismatch(where, _show(expected), _show(actual), note))

    def fail(self, where: str, expected: str, actual: str, note: str = "") -> None:
        self.count += 1
        self.mismatches.append(Mismatch(where, expected, actual, note))


def _show(value: Any) -> str:
    return "(none)" if value is None else str(value)


def _fraction_text(value: Fraction) -> str:
    return str(value) if value.denominator != 1 else str(value.numerator)


def _expected_number_text(expect: NumberExpect) -> str:
    base = _fraction_text(expect.value) + f" (= {float(expect.value)!r})"
    return base if expect.tolerance is None else f"{float(expect.value)!r} within {float(expect.tolerance.abs_tol):g}"


def _number_ok(expect: NumberExpect, actual: Any) -> bool:
    if isinstance(actual, bool) or not isinstance(actual, (int, float)) or not math.isfinite(actual):
        return False
    difference = abs(Fraction(actual) - expect.value)                  # exact arithmetic: no rounding in the comparison
    return difference == 0 if expect.tolerance is None else difference <= expect.tolerance.abs_tol


def _check_event(checks: _Checks, expectation: EventExpect, event: Event, result: StrategyEventResult, label: str) -> None:
    for field_name in ("symbol", "exchange_time", "type"):
        wanted = getattr(expectation, field_name)
        if wanted is not None:
            checks.equal(f"{label} {field_name}", wanted, getattr(event, field_name))
    for field_name, actual in (("verdict", result.verdict), ("reason", result.reason), ("action", result.action),
                               ("window_fill", result.window_fill), ("window_size", result.window_size)):
        wanted = getattr(expectation, field_name)
        if wanted is not None:
            checks.equal(f"{label} {field_name}", wanted, actual,
                         "" if result.diagnostics_available else "the strategy reported no diagnostics for this event")
    for name, wanted in expectation.indicators:
        if name in result.indicators:
            actual = result.indicators[name]
            if _number_ok(wanted, actual):
                checks.count += 1
            else:
                checks.fail(f"{label} {name}", _expected_number_text(wanted), repr(actual),
                            "" if wanted.tolerance is None else wanted.tolerance.why)
        elif name in result.unavailable:
            checks.fail(f"{label} {name}", _expected_number_text(wanted),
                        f"unavailable ({result.unavailable[name]})", "the value does not exist on this event")
        else:
            checks.fail(f"{label} {name}", _expected_number_text(wanted), "not reported")
    for name, reason in expectation.unavailable:
        if name in result.indicators:
            checks.fail(f"{label} {name}", f"unavailable ({reason})", f"a value: {result.indicators[name]!r}",
                        "an unavailable value must be absent, never zero")
        else:
            checks.equal(f"{label} {name} (unavailable reason)", reason, result.unavailable.get(name))


def check_result(scenario: Scenario, document: ReplayDocument) -> tuple[int, list[Mismatch], tuple[ActualSignal, ...]]:
    """Every expectation of a result scenario against the parsed document. Returns (checks made, mismatches, actual signals)."""
    checks = _Checks()
    ids = [c.strategy_id for c in document.strategies]
    checks.equal("strategy ids", [s.strategy_id for s in scenario.strategies], ids)
    actual_signals = tuple(ActualSignal(s.strategy_id, s.event_index, s.symbol, s.side, s.signal_id) for s in document.signals)

    if scenario.warnings is not None:
        checks.equal("warning codes", sorted(scenario.warnings), sorted(w.code for w in document.warnings))
    wanted = scenario.signals or ()
    checks.equal("number of signal requests", len(wanted), len(actual_signals))
    for position, (want, got) in enumerate(zip(wanted, actual_signals)):
        where = f"signal {position + 1}"
        checks.equal(f"{where} strategy", want.strategy, got.strategy)
        checks.equal(f"{where} event_index", want.event_index, got.event_index,
                     "event_index counts from 0; the event numbers shown elsewhere count from 1")
        checks.equal(f"{where} symbol", want.symbol, got.symbol)
        checks.equal(f"{where} side", want.side, got.side)
        if want.signal_id is not None:
            checks.equal(f"{where} signal_id", want.signal_id, got.signal_id)

    for expectation in scenario.events:
        if expectation.strategy not in ids:
            checks.fail(f"event {expectation.first} ({expectation.strategy})", "a strategy of this scenario", "none in the result")
            continue
        slot = ids.index(expectation.strategy)
        for index in range(expectation.first, expectation.last + 1):
            if index >= len(document.events):
                checks.fail(f"event_index {index}", "an event", f"the result has only {len(document.events)} events")
                continue
            event = document.events[index]
            _check_event(checks, expectation, event, event.results[slot], f"event_index {index} ({expectation.strategy})")
    return checks.count, checks.mismatches, actual_signals


def check_rejection(rejection: ExpectedRejection, error: LabUiError) -> tuple[int, list[Mismatch]]:
    """The expected refusal against the tool's own error document (already proven well-formed by the bridge)."""
    checks = _Checks()
    checks.equal("exit status", rejection.exit_status, error.exit_code)
    checks.equal("error code", rejection.error_code, error.runner_code)
    for text in rejection.message_contains:
        checks.count += 1
        if text not in error.message:
            checks.mismatches.append(Mismatch("message", f"mentions '{text}'", error.message))
    checks.equal("number of problems listed", len(rejection.problems), len(error.problems))
    for position, (want, got) in enumerate(zip(rejection.problems, error.problems)):
        for field_name in ("line", "column", "where"):
            expected = getattr(want, field_name)
            if expected is not None:
                checks.equal(f"problem {position + 1} {field_name}", expected, getattr(got, field_name))
    if rejection.total_problems is not None:
        checks.equal("total problems", rejection.total_problems, error.total_problems)
    return checks.count, checks.mismatches


def _describe_rejection(error: LabUiError) -> dict[str, Any]:
    return {"exit_status": error.exit_code, "error_code": error.runner_code, "message": error.message,
            "problems": [{"line": p.line, "column": p.column, "where": p.where, "message": p.message} for p in error.problems],
            "total_problems": error.total_problems}


# ---- one scenario ---------------------------------------------------------------------------------------------

def _result(scenario: Scenario, runner: Runner, status: str, summary: str, started: float, *, checks: int = 0,
            mismatches: Sequence[Mismatch] = (), signals: tuple[ActualSignal, ...] | None = None,
            rejection: Mapping[str, Any] | None = None, error: LabUiError | None = None, data_sha: str = "",
            document: ReplayDocument | None = None) -> ScenarioResult:
    if scenario.demonstration:
        # The demonstration must FAIL on purpose. A pass means the comparison itself cannot see a wrong expectation.
        if status == PASSED:
            status, summary = ERROR, ("The deliberately wrong expectation was NOT detected, so the comparison itself is "
                                      "broken: " + summary)
    return ScenarioResult(scenario, status, summary, checks, tuple(mismatches), signals, rejection, error, data_sha,
                          runner.sha256, document.run_id if document else None,
                          str(document.provenance.get("result_sha256", "")) if document else None,
                          time.perf_counter() - started, state.now(),
                          {k: document.provenance.get(k) for k in ("tool", "project_version", "compiler", "build")}
                          if document else {})


def run_scenario(runner: Runner, scenario: Scenario, root) -> ScenarioResult:
    """Run ONE scenario in its own process and classify the outcome. Never raises for a problem with the scenario's run."""
    started = time.perf_counter()
    path = root / scenario.dataset_path
    try:
        data = path.read_bytes()
    except OSError as error:
        problem = LabUiError("input_missing", f"The scenario's dataset {scenario.dataset_path} could not be read.", detail=str(error))
        return _result(scenario, runner, ERROR, f"{problem.title}: {problem.message}", started, error=problem)
    raw_sha = hashlib.sha256(data).hexdigest()
    lf_sha = hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()
    if lf_sha != scenario.dataset_sha256_lf:
        problem = LabUiError("fixture_changed", f"{scenario.dataset_path} has SHA-256 {lf_sha[:16]}... (line endings normalized) but the "
                             f"scenario was written for {scenario.dataset_sha256_lf[:16]}.... The expectations no longer apply to this file.")
        return _result(scenario, runner, ERROR, f"{problem.title}: {problem.message}", started, error=problem, data_sha=raw_sha)

    try:
        outcome = bridge.run_replay_multi(runner, dataset=data, dataset_sha256=raw_sha,
                                          strategies=[(s.kind, s.params) for s in scenario.strategies])
    except LabUiError as error:
        return _after_error(scenario, runner, error, started, raw_sha)

    document = outcome.document
    if scenario.outcome == "rejected":
        assert scenario.rejection is not None
        return _result(scenario, runner, FAILED, "The tool accepted input that this scenario expects it to refuse.", started,
                       checks=1, mismatches=[Mismatch("outcome", f"a refusal (exit status {scenario.rejection.exit_status}, "
                                                       f"{scenario.rejection.error_code})",
                                                       f"a replay result with {len(document.events)} events and "
                                                       f"{len(document.signals)} signal requests")],
                       signals=tuple(ActualSignal(s.strategy_id, s.event_index, s.symbol, s.side, s.signal_id) for s in document.signals),
                       data_sha=raw_sha, document=document)
    count, mismatches, signals = check_result(scenario, document)
    if mismatches:
        return _result(scenario, runner, FAILED, f"{len(mismatches)} of {count} checks did not match.", started, checks=count,
                       mismatches=mismatches, signals=signals, data_sha=raw_sha, document=document)
    return _result(scenario, runner, PASSED, f"All {count} checks matched.", started, checks=count, signals=signals,
                   data_sha=raw_sha, document=document)


def _after_error(scenario: Scenario, runner: Runner, error: LabUiError, started: float, data_sha: str) -> ScenarioResult:
    """The bridge raised. Only a well-formed refusal (exit status 2 or 3, a valid error document) is a coherent answer."""
    if error.kind not in REJECTION_KINDS:
        return _result(scenario, runner, ERROR, f"{error.title}: {error.message}", started, error=error, data_sha=data_sha)
    described = _describe_rejection(error)
    if scenario.outcome == "rejected":
        assert scenario.rejection is not None
        count, mismatches = check_rejection(scenario.rejection, error)
        if mismatches:
            return _result(scenario, runner, FAILED, f"The tool refused, but {len(mismatches)} of {count} checks of the "
                           "refusal did not match.", started, checks=count, mismatches=mismatches, rejection=described,
                           error=error, data_sha=data_sha)
        return _result(scenario, runner, PASSED, f"The expected refusal happened; all {count} checks matched.", started,
                       checks=count, rejection=described, error=error, data_sha=data_sha)
    return _result(scenario, runner, FAILED, "The tool refused input that this scenario expects it to replay.", started, checks=1,
                   mismatches=[Mismatch("outcome", "a replay result", f"a refusal (exit status {error.exit_code}, "
                                        f"{error.runner_code}): {error.message}")], rejection=described, error=error,
                   data_sha=data_sha)


# ---- all of them ---------------------------------------------------------------------------------------------

Progress = Callable[[str, str], None]               # (scenario id, "running" | "done")


def run_all(runner: Runner, scenario_set: ScenarioSet, number: int, *, include_demo: bool = False,
            progress: Progress | None = None) -> ValidationRun:
    """Run every scenario (and, if asked, the injected-mismatch demonstration). Called only from the Run validation button."""
    started = state.now()
    todo = list(scenario_set.scenarios)
    demo_base = scenario_set.by_id(DEMO_BASE)
    if include_demo and demo_base is not None:
        todo.append(injected_mismatch(demo_base))
    results: list[ScenarioResult] = []
    for scenario in todo:
        if progress:
            progress(scenario.id, "running")
        results.append(run_scenario(runner, scenario, scenario_set.root))
        if progress:
            progress(scenario.id, "done")
    tool = next((r.tool for r in results if r.tool), {})
    return ValidationRun(tuple(results), runner, scenario_set.identity, number, started, state.now(), include_demo, tool)
