"""The Validate view: run the checked-in scenarios through the real executable, then read what matched and what did not.

Nothing runs until the Run button is pressed (`validate.run_all`). Leaving the view and coming back shows the retained results,
each stamped with the dataset bytes and the executable it used; it never reruns. This view checks scenarios. It is not the
C++ CTest/GoogleTest suite.
"""

from __future__ import annotations

import pandas as pd
import streamlit as st

from . import persist, validate, validation_report
from .bridge import Runner
from .errors import LabUiError
from .scenarios import ScenarioSet, load_scenarios
from .validate import (ERROR, FAILED, NOT_RUN, PASSED, SCOPE_NOTE, STATUS_TEXT, ScenarioResult, ValidationRun)

K_RESULT = "val_displayed"
K_LAUNCHES = "val_launches"
K_DEMO = "val_include_demo"
BADGE_COLOR = {PASSED: "green", FAILED: "red", ERROR: "orange", NOT_RUN: "gray"}


def _sidebar(sset: ScenarioSet) -> bool:
    with st.sidebar:
        st.markdown("### Validation")
        st.caption(f"{len(sset.scenarios)} scenarios from tests/fixtures/strategy_lab/scenarios ({len(sset.files)} files).")
        st.caption(SCOPE_NOTE)
        persist.checkbox("Also show a deliberate mismatch (demonstration)", key=K_DEMO,
                         help="Adds one in-memory scenario whose expectations are deliberately wrong, so you can see what a "
                              "failed check looks like. It must fail, and it is not counted in the totals.")
        with st.container(key="run_bar"):
            run_clicked = st.button("Run all scenarios", key="val_run", type="primary", width="stretch",
                                    help="Starts the real replay executable once per scenario, then compares each answer with "
                                         "its hand-derived expectation. Only this button does: switching views and "
                                         "downloading never rerun anything.")
    return bool(run_clicked)


def _launch(runner: Runner, sset: ScenarioSet) -> None:
    number = st.session_state.get(K_LAUNCHES, 0) + 1
    st.session_state[K_LAUNCHES] = number
    with st.status("Running the validation scenarios against the real executable...", expanded=True) as box:
        def progress(scenario_id: str, stage: str) -> None:
            if stage == "running":
                box.write(f"Running {scenario_id} ...")

        run = validate.run_all(runner, sset, number, include_demo=bool(st.session_state.get(K_DEMO)), progress=progress)
        st.session_state[K_RESULT] = run
        counts = run.counts()
        box.update(label=f"Finished: {counts[PASSED]} passed, {counts[FAILED]} failed, {counts[ERROR]} error",
                   state="complete" if not (counts[FAILED] or counts[ERROR]) else "error", expanded=False)


def _status_chip(run: ValidationRun | None, runner: Runner, sset: ScenarioSet) -> None:
    if run is None:
        st.badge("Not run yet", color="gray")
        return
    counts = run.counts()
    stale = run.runner.sha256 != runner.sha256 or run.set_identity != sset.identity
    if stale:
        st.badge("Results are from an older executable or scenario files", color="orange")
    elif counts[ERROR]:
        st.badge(f"{counts[ERROR]} error", color="orange")
    elif counts[FAILED]:
        st.badge(f"{counts[FAILED]} failed", color="red")
    else:
        st.badge(f"All {counts[PASSED]} passed", color="green")


def _signal_text(strategy: str, index: int, symbol: str, side: str, signal_id: int | None) -> str:
    return (f"{strategy}: {side.capitalize()} request, {symbol}, event {index + 1}"
            + (f", id {signal_id}" if signal_id is not None else ""))


def _signals_comparison(result: ScenarioResult) -> None:
    wanted = result.scenario.signals or ()
    got = result.actual_signals or ()
    rows = []
    for position in range(max(len(wanted), len(got))):
        w = wanted[position] if position < len(wanted) else None
        g = got[position] if position < len(got) else None
        want_text = _signal_text(w.strategy, w.event_index, w.symbol, w.side, w.signal_id) if w else "(none expected)"
        got_text = _signal_text(g.strategy, g.event_index, g.symbol, g.side, g.signal_id) if g else "(none produced)"
        rows.append({"#": position + 1, "Expected": want_text, "Actual": got_text,
                     "Match": "yes" if want_text == got_text else "NO"})
    if rows:
        st.table(pd.DataFrame(rows), hide_index=True)
    else:
        st.caption("No signal requests expected, none produced." if result.actual_signals is not None else "No signals to compare.")


def _rejection_comparison(result: ScenarioResult) -> None:
    want = result.scenario.rejection
    got = result.actual_rejection
    if want is None:
        return
    rows = [{"Item": "exit status", "Expected": want.exit_status, "Actual": "" if got is None else got["exit_status"]},
            {"Item": "error code", "Expected": want.error_code, "Actual": "" if got is None else got["error_code"]}]
    for position, problem in enumerate(want.problems):
        actual = got["problems"][position] if got and position < len(got["problems"]) else {}
        for field in ("line", "column", "where"):
            expected = getattr(problem, field)
            if expected is not None:
                rows.append({"Item": f"problem {position + 1} {field}", "Expected": expected, "Actual": actual.get(field, "")})
    for text in want.message_contains:
        rows.append({"Item": "message mentions", "Expected": text,
                     "Actual": "(yes)" if got and text in got["message"] else "(no)" if got else ""})
    st.table(pd.DataFrame(rows).astype(str), hide_index=True)
    if got is not None:
        st.caption(f"The tool's own message: {got['message']}")


def _render_scenario(result: ScenarioResult, runner: Runner) -> None:
    scenario = result.scenario
    title = f"{STATUS_TEXT[result.status].upper()} - {scenario.title}"
    with st.expander(title, expanded=result.status in (FAILED, ERROR)):
        st.badge(STATUS_TEXT[result.status], color=BADGE_COLOR[result.status])
        if scenario.demonstration:
            st.caption("Demonstration: the expectations below are deliberately wrong. A failure here is the point.")
        st.markdown(f"**{result.summary}**")
        st.caption(scenario.purpose)
        if result.error is not None and result.status == ERROR:
            st.error(f"{result.error.title}: {result.error.message}")
            if result.error.stderr:
                st.code(result.error.stderr, language="text")
        if result.mismatches:
            st.markdown("**What did not match**")
            st.table(pd.DataFrame([{"Where": m.where, "Expected": m.expected, "Actual": m.actual, "Note": m.note}
                                   for m in result.mismatches]), hide_index=True)
        if scenario.outcome == "result":
            st.markdown("**Signal requests: expected and actual** (event numbers count from 1)")
            _signals_comparison(result)
        else:
            st.markdown("**The refusal: expected and actual**")
            _rejection_comparison(result)
        st.markdown("**Dataset and configuration used**")
        cfg = " | ".join(f"{s.strategy_id}: {s.kind} " + ", ".join(f"{k}={v}" for k, v in s.params if k != "strategy_id")
                         for s in scenario.strategies)
        st.caption(f"Dataset {scenario.dataset_path} (SHA-256 of the bytes given to the tool {result.dataset_sha256[:12] or 'n/a'}...; "
                   f"pinned LF-normalized SHA-256 {scenario.dataset_sha256_lf[:12]}...)")
        st.caption(f"Configuration: {cfg}")
        st.caption(f"Executable SHA-256 {result.executable_sha256[:12]}...; run_id {result.run_id or 'n/a'}; "
                   f"{result.checks} checks; {result.duration_s * 1000:.0f} ms; finished {result.finished_at}.")
        if result.executable_sha256 != runner.sha256:
            st.warning("This result came from a different executable than the one now in use.")
        with st.expander("Where this expectation comes from"):
            prov = scenario.provenance
            st.markdown(f"**Derived:** {prov.derived_by}.")
            st.markdown("**Sources:** " + "; ".join(prov.sources))
            st.markdown("**Derivation:**\n" + "\n".join(f"{i + 1}. {step}" for i, step in enumerate(prov.derivation)))
            st.markdown("**Independent cross-checks:** " + "; ".join(prov.cross_checks))
            for tolerance in scenario.tolerances.values():
                st.caption(f"Tolerance `{tolerance.name}` = {float(tolerance.abs_tol):g}: {tolerance.why}.")


def _results(run: ValidationRun, runner: Runner, sset: ScenarioSet) -> None:
    counts = run.counts()
    columns = st.columns(4)
    columns[0].metric("Passed", counts[PASSED])
    columns[1].metric("Failed", counts[FAILED])
    columns[2].metric("Error", counts[ERROR])
    columns[3].metric("Not run", counts[NOT_RUN])
    st.caption(f"Validation {run.number} of this browser session, finished {run.finished_at}. Executable SHA-256 "
               f"{run.runner.sha256[:12]}...; scenario files identity {run.set_identity[:12]}....")
    if run.runner.sha256 != runner.sha256 or run.set_identity != sset.identity:
        st.warning("These results are from an older executable or older scenario files than the ones now in use. Press "
                   "**Run all scenarios** to check the current ones.")
    if counts[ERROR]:
        st.error("An ERROR means a check could not be carried out (missing or crashed executable, timeout, malformed output, "
                 "or a changed dataset file). It is not a pass and not a failure of the strategies.")
    summary = pd.DataFrame([{"Status": STATUS_TEXT[r.status], "Scenario": r.scenario.id, "Covers": ", ".join(r.scenario.covers),
                             "Dataset": r.scenario.dataset_path, "Result": r.summary}
                            for r in run.results if not r.demonstration])
    st.table(summary, hide_index=True)
    for result in run.results:
        if not result.demonstration:
            _render_scenario(result, runner)
    demos = run.demonstrations()
    if demos:
        st.markdown("#### Demonstration: what a mismatch looks like")
        for result in demos:
            _render_scenario(result, runner)
    left, right = st.columns(2)
    left.download_button("Download validation report - JSON", validation_report.report_json(run, sset), on_click="ignore",
                         file_name=f"strategy_lab_validation_{run.number}_{run.set_identity[:8]}.json", mime="application/json",
                         key="val_dl_json", width="stretch",
                         help="Provenance, every scenario's expected and actual values, mismatches and errors.")
    right.download_button("Download validation report - Markdown", validation_report.report_markdown(run, sset),
                          on_click="ignore", file_name=f"strategy_lab_validation_{run.number}_{run.set_identity[:8]}.md",
                          mime="text/markdown", key="val_dl_md", width="stretch",
                          help="The same facts for a person to read.")


def render(runner: Runner) -> None:
    try:
        sset = load_scenarios()            # a few small files: re-read on each run, so a change on disk shows as stale
    except LabUiError as error:
        st.error(f"**{error.title}.** {error.message}")
        if error.detail:
            st.code(error.detail, language="text")
        return
    if _sidebar(sset):
        _launch(runner, sset)
    run: ValidationRun | None = st.session_state.get(K_RESULT)

    left, right = st.columns([8, 4], vertical_alignment="center")
    left.markdown("**Scenario validation**")
    left.caption("Hand-derived expectations, checked against the real executable.")
    with right:
        _status_chip(run, runner, sset)
    st.info(SCOPE_NOTE)
    if run is None:
        st.markdown("Nothing has been run in this session. Press **Run all scenarios** in the left panel. Each scenario "
                    "starts the real executable on one checked-in dataset and compares the recorded signals and diagnostics "
                    "with expectations written by hand, including cases where the tool should refuse bad input.")
        st.table(pd.DataFrame([{"Status": STATUS_TEXT[NOT_RUN], "Scenario": s.id, "Covers": ", ".join(s.covers),
                                "Dataset": s.dataset_path} for s in sset.scenarios]), hide_index=True)
    else:
        _results(run, runner, sset)
