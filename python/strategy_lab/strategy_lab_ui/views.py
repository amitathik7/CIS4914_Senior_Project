"""The main area: header, chart, replay controls, inspector, counters and the three tabs.

Everything shown as "replay state" is read from the visible prefix of a finished result. Anything that describes the
whole run is labelled as such and is identical at every replay position.
"""

from __future__ import annotations

import streamlit as st

from . import charts, details, exports, playback, session, tables
from .catalog import StrategySpec
from .errors import LabUiError
from .inspector import render_inspector
from .replay import ReplayModel
from .state import CompletedRun, FailedRun, Status

_STATUS_BADGE = {"empty": ("No run yet", "gray"), "current": ("Run complete - settings match", "green"),
                 "stale": ("Settings changed - result is from the previous settings", "orange"),
                 "failed": ("Last run failed", "red")}


def render_header(run: CompletedRun | None, status: Status, selected_name: str | None) -> None:
    if run is not None:
        src = run.source
        st.markdown(f"**Dataset:** {src.display_name}")
        st.caption(("Synthetic fixture" if src.synthetic else "Uploaded file, provenance unknown")
                   + f" - SHA-256 {src.sha256[:12]}... - run {run.number}")
    else:
        st.markdown(f"**Dataset:** {selected_name or 'none selected'}")
        st.caption("Selected, not run yet")
    label, color = _STATUS_BADGE[status.kind]
    st.badge(label, color=color)               # under the dataset line, so it is never cut off in a narrow window


def render_setup_error(error: LabUiError) -> None:
    st.markdown("### Strategy Lab")
    st.error(f"**{error.title}.** {error.message}")
    if error.detail:
        st.code(error.detail, language="text")
    if error.stderr:
        st.code(error.stderr, language="text")
    st.button("Check again", key="recheck")


def _replay_counters(model: ReplayModel, cursor: int, symbol: str | None) -> None:
    counts = model.counts(cursor, symbol)
    scope = exports.scope_prefix(model, cursor, symbol)
    st.markdown("#### Replay so far")
    st.caption(f"Visible prefix only ({scope.split(': ', 1)[1]})")
    left, right = st.columns(2)
    left.metric("Events", counts.events)
    right.metric("Evaluated", counts.verdicts.get("evaluated", 0))
    left.metric("Buy requests", counts.buys)
    right.metric("Warming up", counts.verdicts.get("warming_up", 0))
    left.metric("Sell requests", counts.sells)
    right.metric("Ignored", counts.verdicts.get("ignored", 0))


def _full_run_overview(run: CompletedRun) -> None:
    model, doc = run.model, run.document
    buys = sum(s.side == "buy" for s in model.signals)
    st.markdown("#### Full run")
    st.caption("Whole result: does not change with the replay position.")
    st.write(f"{model.total} events - {len(model.symbols)} symbol(s) - {len(model.signals)} signal requests "
             f"({buys} buy, {len(model.signals) - buys} sell) - {len(doc.warnings)} warning(s)")
    if not model.signals:
        st.info("This run produced no signal requests. That is a valid result.")


def _signals_tab(run: CompletedRun, cursor: int, symbol: str | None) -> None:
    model = run.model
    scope = exports.scope_prefix(model, cursor, symbol)
    st.caption(f"Signal requests in the {scope}.")
    table = tables.signals_table(model, cursor, symbol)
    if table.empty:
        st.info("No signal requests in the visible prefix." if cursor else "Nothing revealed yet.")
    else:
        st.dataframe(table, hide_index=True, width="stretch")
    prefix = [s for s in model.signals_through(cursor, symbol)]
    left, right = st.columns(2)
    left.download_button(f"Signals CSV - VISIBLE PREFIX ({len(prefix)} rows)",
                         exports.signals_csv(prefix, scope), on_click="ignore", key="dl_csv_prefix", mime="text/csv",
                         file_name=exports.csv_filename_prefix(run.document.run_id, cursor, model.total, symbol),
                         help=f"Only what the table above shows: {scope}.", width="stretch")
    right.download_button(f"Signals CSV - FULL RUN ({len(model.signals)} rows)",
                          exports.signals_csv(list(model.signals), exports.scope_full(model)), on_click="ignore",
                          key="dl_csv_full", mime="text/csv", file_name=exports.csv_filename_full(run.document.run_id),
                          help="Every signal request of the run, all symbols, whatever the replay position.",
                          width="stretch")


def _diagnostics_tab(run: CompletedRun, spec: StrategySpec | None, cursor: int, symbol: str | None) -> None:
    model = run.model
    config = run.document.strategies[model.slot]
    scope = exports.scope_prefix(model, cursor, symbol)
    st.caption(f"One row per recorded event in the {scope}. Blank indicator cells are values that do not exist on "
               "that row; the Unavailable column says why.")
    reasons = tables.reasons_table(spec, model.counts(cursor, symbol))
    if not reasons.empty:
        st.markdown("**Decision reasons so far**")
        st.dataframe(reasons, hide_index=True, width="stretch")
    table = tables.diagnostics_table(model, cursor, symbol, charts.indicator_names(config.kind) or
                                     (spec.indicators if spec else ()))
    if table.empty:
        st.info("Nothing revealed yet." if cursor == 0 else "No rows for the displayed symbol yet.")
    else:
        st.dataframe(table, hide_index=True, width="stretch")


def render_results(run: CompletedRun, spec: StrategySpec | None) -> None:
    model = run.model
    config = run.document.strategies[model.slot]
    cursor = model.clamp(st.session_state.get(session.K_CURSOR, 0))
    symbol = session.symbol_filter(model)

    for warning in run.document.warnings:
        st.warning(f"`{warning.code}`: {warning.message}")
    note = charts.panel_note(model, symbol)
    if note:
        st.caption(note)
    st.plotly_chart(charts.build_figure(model, config, cursor, symbol, model.selected_event(cursor)),
                    key="lab_chart", width="stretch", theme=None, config={"displaylogo": False})
    playback.render_controls(model, cursor, symbol, playback.EXPLORE_KEYS, session.navigate, session.on_slider)

    inspector_column, counters_column = st.columns([3, 2])
    with inspector_column:
        render_inspector(model, spec, cursor, symbol)
    with counters_column:
        _replay_counters(model, cursor, symbol)
        _full_run_overview(run)

    signals_tab, diagnostics_tab, details_tab = st.tabs(["Signals", "Diagnostics", "Run details"])
    with signals_tab:
        _signals_tab(run, cursor, symbol)
    with diagnostics_tab:
        _diagnostics_tab(run, spec, cursor, symbol)
    with details_tab:
        details.render_run_details(run)


def render_empty(failure: FailedRun | None) -> None:
    if failure is None:
        st.info("Choose a dataset and a strategy in the left panel, then press **Run replay**. The C++ strategies "
                "replay the whole dataset once; you then step through the recorded decisions here.")
