"""The exportable validation report: what ran, against which executable and which files, what matched, and what did not.

JSON is the authoritative form; the Markdown is the same facts for a person. Both state the scope plainly: scenario checks, not
the C++ test suite. Failures and errors are listed with their mismatches or the tool's own error; nothing is summarized away.
"""

from __future__ import annotations

import json
from typing import Any

from .scenarios import SCENARIO_DIR, ScenarioSet
from .validate import ERROR, FAILED, NOT_RUN, PASSED, SCOPE_NOTE, STATUS_TEXT, ScenarioResult, ValidationRun

REPORT = "strategy_lab.validation_report"


def _overall(counts: dict[str, int]) -> str:
    if counts[ERROR]:
        return "errors: some checks could not be carried out"
    if counts[FAILED]:
        return "failures: some results differ from the expectations"
    return "all scenarios passed"


def _scenario_entry(result: ScenarioResult) -> dict[str, Any]:
    scenario = result.scenario
    entry: dict[str, Any] = {
        "id": scenario.id, "title": scenario.title, "purpose": scenario.purpose, "covers": list(scenario.covers),
        "demonstration": scenario.demonstration, "status": result.status, "status_text": STATUS_TEXT[result.status],
        "summary": result.summary, "checks_made": result.checks,
        "mismatches": [{"where": m.where, "expected": m.expected, "actual": m.actual, "note": m.note} for m in result.mismatches],
        "dataset": {"path": scenario.dataset_path, "sha256_lf_expected": scenario.dataset_sha256_lf,
                    "sha256_given_to_tool": result.dataset_sha256},
        "configuration": [{"kind": s.kind, "strategy_id": s.strategy_id, "parameters": dict(s.params)} for s in scenario.strategies],
        "expected": dict(scenario.raw.get("expect", {})),
        "provenance_of_the_expectation": {"derived_by": scenario.provenance.derived_by, "sources": list(scenario.provenance.sources),
                                          "derivation": list(scenario.provenance.derivation),
                                          "cross_checks": list(scenario.provenance.cross_checks)},
        "tolerances": {n: {"abs_tol": str(t.abs_tol), "why": t.why} for n, t in scenario.tolerances.items()},
        "scenario_file": scenario.source_file, "run_id": result.run_id, "result_sha256": result.result_sha256,
        "executable_sha256": result.executable_sha256, "duration_s": round(result.duration_s, 3),
        "finished_at": result.finished_at}
    if result.actual_signals is not None:
        entry["actual_signals"] = [{"strategy": s.strategy, "event_index": s.event_index, "symbol": s.symbol, "side": s.side,
                                    "signal_id": s.signal_id} for s in result.actual_signals]
    if result.actual_rejection is not None:
        entry["actual_rejection"] = dict(result.actual_rejection)
    if result.status == ERROR and result.error is not None:
        entry["error"] = {"kind": result.error.kind, "title": result.error.title, "message": result.error.message,
                          "exit_code": result.error.exit_code, "detail": result.error.detail, "stderr": result.error.stderr}
    return entry


def report_dict(run: ValidationRun, scenario_set: ScenarioSet) -> dict[str, Any]:
    counts = run.counts()
    normal = [r for r in run.results if not r.demonstration]
    return {
        "report": REPORT, "report_version": "1.0", "scope_note": SCOPE_NOTE,
        "validation_number_in_session": run.number, "started_at": run.started_at, "finished_at": run.finished_at,
        "summary": {"scenarios": len(normal), "passed": counts[PASSED], "failed": counts[FAILED], "error": counts[ERROR],
                    "not_run": counts[NOT_RUN], "overall": _overall(counts)},
        "demonstration": {"included": run.include_demo,
                          "note": "An opt-in, in-memory scenario with deliberately wrong expectations. It must FAIL; it is not "
                                  "counted in the summary above.",
                          "results": [{"id": r.scenario.id, "status": r.status, "summary": r.summary,
                                       "failed_as_designed": r.status == FAILED} for r in run.demonstrations()]},
        "executable": {"file_name": run.runner.path.replace("\\", "/").rsplit("/", 1)[-1], "sha256": run.runner.sha256,
                       "size_bytes": run.runner.size, **{k: v for k, v in run.tool.items() if v is not None}},
        "scenario_files": {"directory": str(SCENARIO_DIR.relative_to(SCENARIO_DIR.parents[3])).replace("\\", "/"),
                           "identity_sha256": run.set_identity, "files": list(scenario_set.files)},
        "scenarios": [_scenario_entry(r) for r in run.results]}



def report_json(run: ValidationRun, scenario_set: ScenarioSet) -> bytes:
    return (json.dumps(report_dict(run, scenario_set), indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def _md(text: object) -> str:
    return str(text).replace("|", "\\|").replace("\n", " ")


def report_markdown(run: ValidationRun, scenario_set: ScenarioSet) -> bytes:
    data = report_dict(run, scenario_set)
    summary, exe = data["summary"], data["executable"]
    lines = ["# Strategy Lab validation report", "", f"> {SCOPE_NOTE}", "",
             f"- Validation {run.number} of this browser session, {run.started_at} to {run.finished_at}",
             f"- **Result: {summary['overall']}** - {summary['passed']} passed, {summary['failed']} failed, "
             f"{summary['error']} error, of {summary['scenarios']} scenarios",
             f"- Executable `{exe['file_name']}`, SHA-256 `{exe['sha256']}`, {exe['size_bytes']:,} bytes"
             + (f", {exe.get('tool')} {exe.get('project_version')}, {exe.get('compiler')} / {exe.get('build')}" if exe.get("tool") else ""),
             f"- Scenario files: `{data['scenario_files']['directory']}` ({len(scenario_set.files)} files), identity SHA-256 "
             f"`{run.set_identity}`", "",
             "| Status | Scenario | Dataset | Checks | Summary |", "|---|---|---|---|---|"]
    for entry in data["scenarios"]:
        lines.append(f"| {entry['status_text']}{' (demonstration)' if entry['demonstration'] else ''} | `{entry['id']}` | "
                     f"`{_md(entry['dataset']['path'])}` | {entry['checks_made']} | {_md(entry['summary'])} |")
    problems = [e for e in data["scenarios"] if e["status"] in (FAILED, ERROR) and not e["demonstration"]]
    if problems:
        lines += ["", "## Failures and errors", ""]
    for entry in problems:
        lines += [f"### {entry['status_text']}: {entry['title']} (`{entry['id']}`)", "", entry["summary"], ""]
        if entry.get("error"):
            lines += [f"- Error kind `{entry['error']['kind']}`, exit status `{entry['error']['exit_code']}`",
                      f"- {entry['error']['message']}", ""]
        if entry["mismatches"]:
            lines += ["| Where | Expected | Actual | Note |", "|---|---|---|---|"]
            lines += [f"| {_md(m['where'])} | {_md(m['expected'])} | {_md(m['actual'])} | {_md(m['note'])} |" for m in entry["mismatches"]]
            lines.append("")
    demo = [e for e in data["scenarios"] if e["demonstration"]]
    if demo:
        lines += ["## Demonstration (deliberately wrong expectations; expected to fail)", ""]
        for entry in demo:
            lines += [f"- {entry['status_text']}: {entry['summary']}"]
            lines += [f"  - {_md(m['where'])}: expected {_md(m['expected'])}, actual {_md(m['actual'])}" for m in entry["mismatches"]]
        lines.append("")
    lines += ["## Where the expectations come from", "",
              "Each scenario file states how its expected values were derived (by hand, from the strategy documents and the "
              "fixture bars), the sources, and the independent cross-checks. The JSON report carries them per scenario.", ""]
    for entry in data["scenarios"]:
        if not entry["demonstration"]:
            lines.append(f"- `{entry['id']}`: " + "; ".join(entry["provenance_of_the_expectation"]["sources"]))
    return ("\n".join(lines) + "\n").encode("utf-8")

