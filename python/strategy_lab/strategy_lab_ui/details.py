"""Banners and the Run details tab: provenance of the whole run, kept apart from prefix-dependent numbers."""

from __future__ import annotations

from datetime import datetime

import pandas as pd
import streamlit as st

from . import exports, session
from .state import CompletedRun, FailedRun, Status


# ---- banners --------------------------------------------------------------------------------------------

def render_failure(failure: FailedRun, displayed: CompletedRun | None) -> None:
    error = failure.error
    st.error(f"**Run {failure.number} failed ({failure.at}). {error.title}.** {error.message}")
    if error.kind == "dataset_invalid" and failure.request is not None:
        st.caption(f"In these messages the replay tool calls the dataset 'dataset.csv': that is a private copy of "
                   f"'{failure.request.dataset_key}'. Line numbers refer to your file.")
    if error.problems:
        rows = [{"Line": p.line, "Column": p.column or "", "Where": p.where or "", "Problem": p.message}
                for p in error.problems]
        st.table(pd.DataFrame(rows), hide_index=True)
        if error.total_problems > len(error.problems):
            st.caption(f"{error.total_problems - len(error.problems)} more problems not listed.")
    if displayed is not None:
        st.warning(f"Showing the **last successful run** (run {displayed.number}, {displayed.finished_at}), "
                   "not the settings that failed.")
    with st.expander("Technical details"):
        st.write(f"kind: `{error.kind}`; exit status: `{error.exit_code}`")
        if error.detail:
            st.code(error.detail, language="text")
        if error.stderr:
            st.markdown("stderr of the replay tool (human-readable diagnostics):")
            st.code(error.stderr, language="text")
        if error.stdout_excerpt:
            st.markdown("start of stdout:")
            st.code(error.stdout_excerpt, language="text")


def render_status_banner(status: Status, displayed: CompletedRun | None) -> None:
    if status.kind == "stale" and displayed is not None:
        st.warning("**These settings have changed since the displayed run.** The chart and tables below still show "
                   f"run {displayed.number}, which used the previous settings: " + "; ".join(status.reasons)
                   + ". Press **Run replay** to update.")
    elif status.kind == "failed" and status.reasons and displayed is not None:
        st.warning("The settings on screen also differ from the last successful run: " + "; ".join(status.reasons) + ".")


# ---- the Run details tab ----------------------------------------------------------------------------

def _kv(rows: list[tuple[str, object]]) -> None:
    st.table(pd.DataFrame([{"Item": k, "Value": str(v)} for k, v in rows]), hide_index=True)


def _local(mtime_ns: int) -> str:
    return datetime.fromtimestamp(mtime_ns / 1e9).astimezone().isoformat(timespec="seconds")


def render_run_details(run: CompletedRun) -> None:
    doc, src, proc = run.document, run.source, run.outcome.process
    config = doc.strategies[0]

    st.markdown("##### Dataset (whole run)")
    _kv([("Name", src.display_name), ("Kind", "Built-in synthetic fixture" if src.synthetic else "Uploaded file"),
         ("Provenance", src.provenance), ("SHA-256", doc.dataset.sha256), ("Bytes / rows", f"{doc.dataset.bytes:,} / {doc.dataset.rows:,}"),
         ("Format", doc.dataset.format), ("First / last time", f"{doc.dataset.first_exchange_time}  to  {doc.dataset.last_exchange_time}"),
         ("Time zone", "UTC only: every time is RFC 3339 with a 'Z'; the lab does no time-zone conversion"),
         ("Bar interval", "Not carried by the data and not checked; the strategies assume one consistent interval per symbol"),
         ("Name the tool saw", f"{doc.dataset.name} (a private copy; the original file name is shown above)")])
    st.table(pd.DataFrame([{"Symbol": s.symbol, "Bars": s.bar_rows, "Trades": s.trade_rows,
                            "First": s.first_exchange_time, "Last": s.last_exchange_time} for s in doc.dataset.symbols]),
             hide_index=True)

    st.markdown("##### Configuration that was run")
    _kv([("Strategy", f"{config.kind} (strategy_id {config.strategy_id})"), ("Window size", config.window_size),
         *[(f"param {k}", v) for k, v in config.parameters.items()],
         *[(f"derived {k}", v) for k, v in config.derived.items()], ("max_rows applied", doc.max_rows),
         ("Bus fault", doc.bus_fault)])

    if doc.warnings:
        st.markdown("##### Warnings from the replay tool")
        for warning in doc.warnings:
            st.warning(f"`{warning.code}`: {warning.message}")

    st.markdown("##### Run and build identity")
    prov = doc.provenance
    _kv([("run_id", doc.run_id), ("result_sha256", prov.get("result_sha256", "")), ("Schema version", doc.schema_version),
         ("Tool / version", f"{prov.get('tool', '?')} {prov.get('project_version', '?')}"),
         ("Compiler / build", f"{prov.get('compiler', '?')} / {prov.get('build', '?')}"),
         ("Generated at (tool clock)", prov.get("generated_at", "not recorded")),
         ("Executable", run.runner.path), ("Executable size / modified", f"{run.runner.size:,} bytes / {_local(run.runner.mtime_ns)}"),
         ("Executable SHA-256", run.runner.sha256), ("Launch number", f"{run.number} (this browser session has launched "
                                                                  f"{st.session_state.get(session.K_LAUNCHES, 0)} so far)"),
         ("Finished (UI clock) / duration", f"{run.finished_at} / {proc.duration_s:.2f} s"),
         ("Engine", doc.context.get("engine", "")), ("Bus", doc.context.get("event_bus", "")),
         ("Replay order", doc.context.get("replay_order", ""))])
    st.caption(doc.notice)

    st.markdown("##### Whole-run summary (not affected by the replay position)")
    summary = doc.summary
    strat = (summary.get("strategies") or [{}])[0] if isinstance(summary.get("strategies"), list) else {}
    _kv([("Events / bars / signal requests", f"{summary.get('events')} / {summary.get('bars')} / {summary.get('signals')}"),
         ("Buy / sell requests", f"{(strat.get('signals') or {}).get('buy')} / {(strat.get('signals') or {}).get('sell')}"),
         ("Verdicts", strat.get("verdicts")), ("Reasons", strat.get("reasons"))])
    engine_stats = (doc.engine.get("stats") if isinstance(doc.engine.get("stats"), dict) else {}) or {}
    if engine_stats:
        st.caption("Engine counters: " + ", ".join(f"{k} {v}" for k, v in engine_stats.items()))
    if doc.publication_failures:
        st.error(f"{len(doc.publication_failures)} signal request(s) were refused by the bus (see the JSON export).")

    st.markdown("##### Download")
    st.download_button("Download run JSON - FULL RUN (the replay tool's output, unmodified)", run.raw_json,
                       file_name=exports.json_filename(doc.run_id), mime="application/json", on_click="ignore",
                       key="dl_json", help="Covers the whole run, not just the visible replay prefix.")

    with st.expander("Technical details (command, exit status, stderr)"):
        st.write(f"command: `{' '.join(proc.argv)}` (run from a private temporary directory, no shell)")
        st.markdown("argument file given to the tool:")
        st.code(proc.args_file, language="text")
        st.write(f"exit status: `{proc.exit_code}`; stderr truncated: `{proc.stderr_truncated}`")
        st.code(proc.stderr or "(stderr was empty)", language="text")

