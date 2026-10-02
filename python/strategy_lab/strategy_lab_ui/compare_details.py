"""Compare banners (failure, stale) and the Run details tab. Provenance of the whole comparison, kept apart from the
prefix-dependent numbers; the full-run package download lives here.
"""

from __future__ import annotations

import pandas as pd
import streamlit as st

from . import compare_exports, compare_session as cs, compare_tables
from .compare import CompletedComparison, FailedComparison
from .state import Status


def render_failure(failure: FailedComparison, displayed: CompletedComparison | None) -> None:
    """Say which configuration failed. A member that finished while the other failed is NOT shown as half a comparison."""
    labels = {m.member: m.label for m in failure.request.members} if failure.request else {}
    st.error(f"**Comparison {failure.number} failed ({failure.at}).** No comparison was produced.")
    for member, label in labels.items():
        if member in failure.errors:
            error = failure.errors[member]
            st.markdown(f"- **{label}: failed.** {error.title}. {error.message}")
        elif member in failure.succeeded:
            st.markdown(f"- **{label}:** its own replay finished, but a comparison needs both, so its result is not shown.")
    if "" in failure.errors:
        st.markdown(f"- **The two runs could not be paired.** {failure.errors[''].message}")
    for member, error in failure.errors.items():
        if error.problems:
            st.caption(f"Problems reported for {labels.get(member, member) or 'the pair'} (the tool calls the dataset 'dataset.csv'):")
            st.table(pd.DataFrame([{"Line": p.line, "Column": p.column or "", "Where": p.where or "", "Problem": p.message}
                                   for p in error.problems]), hide_index=True)
    if displayed is not None:
        st.warning(f"Showing the **last completed comparison** (comparison {displayed.number}, {displayed.finished_at}): both of "
                   "its runs belong together. It is not the settings that failed.")
    with st.expander("Technical details"):
        for member, error in failure.errors.items():
            st.write(f"{labels.get(member, 'pair')}: kind `{error.kind}`; exit status `{error.exit_code}`")
            if error.detail:
                st.code(error.detail, language="text")
            if error.stderr:
                st.markdown("stderr of the replay tool:")
                st.code(error.stderr, language="text")


def render_status_banner(status: Status, displayed: CompletedComparison | None) -> None:
    if status.kind == "stale" and displayed is not None:
        st.warning("**These settings have changed since the displayed comparison.** Everything below still shows comparison "
                   f"{displayed.number}, which used the previous settings: " + "; ".join(status.reasons)
                   + ". Press **Run comparison** to update both sides together.")
    elif status.kind == "failed" and status.reasons and displayed is not None:
        st.warning("The settings on screen also differ from the last completed comparison: " + "; ".join(status.reasons) + ".")


def _kv(rows: list[tuple[str, object]]) -> None:
    st.table(pd.DataFrame([{"Item": k, "Value": str(v)} for k, v in rows]), hide_index=True)


def render_run_details(comparison: CompletedComparison) -> None:
    src, first = comparison.source, comparison.runs[0].document
    st.markdown("##### Shared by both configurations")
    _kv([("Dataset", src.display_name), ("Kind", "Built-in synthetic fixture" if src.synthetic else "Uploaded file"),
         ("Provenance", src.provenance), ("SHA-256", first.dataset.sha256),
         ("Bytes / rows", f"{first.dataset.bytes:,} / {first.dataset.rows:,}"),
         ("Symbols allowlist", comparison.request.members[0].param("symbols")),
         ("Replay order", first.context.get("replay_order", "")), ("Clock", first.context.get("clock", "")),
         ("Bus", first.context.get("event_bus", "")),
         ("Engine runs", "two independent runs (a fresh engine and fresh strategy state each); their signal ids both start "
                         "at 1, so every id here is shown with its configuration and run-scoped reference")])
    for run in comparison.runs:
        doc, config = run.document, run.document.strategies[0]
        st.markdown(f"##### {run.config.label}")
        _kv([("Strategy", f"{config.kind} (strategy_id {config.strategy_id})"), ("run_id", doc.run_id),
             ("result_sha256", doc.provenance.get("result_sha256", "")), ("Schema version", doc.schema_version),
             ("Signal requests (full run)", len(doc.signals)),
             ("Warnings", ", ".join(w.code for w in doc.warnings) or "none")])
        st.table(compare_tables.member_rows(run), hide_index=True)
    prov = first.provenance
    st.markdown("##### Executable and comparison identity")
    _kv([("Executable", comparison.runner.path), ("Executable SHA-256", comparison.runner.sha256),
         ("Tool / version", f"{prov.get('tool', '?')} {prov.get('project_version', '?')}"),
         ("Compiler / build", f"{prov.get('compiler', '?')} / {prov.get('build', '?')}"),
         ("Comparison number", f"{comparison.number} (this browser session has launched "
                               f"{st.session_state.get(cs.K_LAUNCHES, 0)} comparison(s) so far)"),
         ("Finished (UI clock)", comparison.finished_at)])
    st.caption(first.notice)

    st.markdown("##### Download")
    st.download_button("Download comparison package - FULL RUN (zip)", compare_exports.package_zip(comparison),
                       file_name=compare_exports.package_filename(comparison), mime="application/zip", on_click="ignore",
                       key="cmp_dl_package",
                       help="Both original runner results (byte for byte), the configuration and provenance, a labelled "
                            "summary and every signal request. Covers the whole run, not just the visible replay prefix.")
