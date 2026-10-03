"""The Compare view: two configurations of the real strategies over one shared dataset, one shared replay cursor.

Everything shown is read from the two completed runs. Only the Run comparison button starts the engine (two independent
runs, `compare_session.launch`); stepping, the slider, the display filter, tabs, downloads and editing settings never do.
Signal requests only: no fills, returns or portfolio, and no ranking of the two configurations.
"""

from __future__ import annotations

import streamlit as st

from . import (charts, compare_details, compare_exports, compare_session as cs, compare_sidebar, compare_tables, playback,
               state, table_order)
from .bridge import Runner
from .catalog import Catalog
from .compare import CompletedComparison, FailedComparison
from .schema import exact_text
from .state import Status

_STATUS_BADGE = {"empty": ("No comparison yet", "gray"), "current": ("Comparison complete - settings match", "green"),
                 "stale": ("Settings changed - result is from the previous settings", "orange"),
                 "failed": ("Last comparison failed", "red")}
NOT_QUALITY = "Counts are counts of recorded requests, not a measure of quality. Signal requests are not trades."


def _launch(runner: Runner, side: compare_sidebar.CompareSidebar) -> None:
    assert side.source is not None and side.members is not None
    labels = {m.member: m.label for m in side.members}
    with st.status("Running the comparison: two independent replays of the same dataset...", expanded=True) as box:
        def progress(member: str, stage: str) -> None:
            box.write(f"{labels[member]}: {stage}")

        outcome = cs.launch(runner, side.source, side.members, progress)
        if isinstance(outcome, FailedComparison):
            box.update(label="The comparison failed (details below)", state="error", expanded=False)
        else:
            box.update(label="Both replays finished", state="complete", expanded=False)


def _header(displayed: CompletedComparison | None, status: Status, selected_name: str | None) -> None:
    if displayed is not None:
        src = displayed.source
        st.markdown(f"**Dataset (shared):** {src.display_name}")
        st.caption(("Synthetic fixture" if src.synthetic else "Uploaded file, provenance unknown")
                   + f" - SHA-256 {src.sha256[:12]}... - comparison {displayed.number}")
    else:
        st.markdown(f"**Dataset (shared):** {selected_name or 'none selected'}")
        st.caption("Selected, not run yet")
    label, color = _STATUS_BADGE[status.kind]
    st.badge(label, color=color)               # under the dataset line, so it is never cut off in a narrow window


def _cards(comparison: CompletedComparison) -> None:
    for column, run in zip(st.columns(2), comparison.runs):
        doc = run.document
        buys = sum(s.side == "buy" for s in doc.signals)
        params = " | ".join(f"{k}={compare_tables.plain(v)}" for k, v in doc.strategies[0].parameters.items())
        with column.container(key=f"cmp_card_{run.config.member.lower()}", border=True):
            st.markdown(f"**{run.config.label}**")
            st.caption(params)
            st.caption(f"run {doc.run_id} - full run: {len(doc.signals)} signal requests ({buys} buy, {len(doc.signals) - buys} sell)")


def _event_panel(model, labels: dict[str, str], cursor: int, symbol: str | None) -> None:
    st.markdown("#### Decisions at the selected event")
    event = model.selected_event(cursor)
    if event is None:
        st.info("No event selected: at position 0 nothing has been revealed. Step forward to compare the first recorded decisions.")
        return
    st.markdown(f"**Event {event.index + 1} of {model.total}** - {event.symbol} - {event.type} - `{event.exchange_time}`")
    st.caption(("no price" if event.price is None else f"close {exact_text(event.price)}") + f"; source line "
               f"{event.source_line if event.source_line is not None else 'unknown'}.")
    if symbol is not None and event.symbol != symbol:
        st.info(f"This event belongs to {event.symbol}, not the displayed symbol ({symbol}). The cursor follows the recorded "
                "order of all symbols; the chart and tables show the displayed symbol only.")
    st.table(compare_tables.decision_table(model, labels, event), hide_index=True)
    st.markdown("**Signal requests on this event**")
    st.markdown(compare_tables.agreement_sentence(model, event.index))
    found = compare_tables.signals_at_event(model, labels, event.index)
    if not found.empty:
        st.table(found, hide_index=True)


def _counts_panel(model, labels: dict[str, str], cursor: int, symbol: str | None) -> None:
    scope = compare_exports.scope_prefix(model, cursor, symbol)
    st.markdown("#### Replay so far")
    st.caption(f"Visible prefix only ({scope.split(': ', 1)[1]})")
    st.table(compare_tables.counts_table(model, labels, cursor, symbol), hide_index=True)
    st.markdown("**Warm-up and readiness** (from each strategy's recorded window)")
    st.table(compare_tables.readiness_table(model, labels, cursor, symbol), hide_index=True)
    st.caption("A ready strategy may still emit nothing: readiness is read from the recorded window, never from signals. "
               + NOT_QUALITY)


def _signals_tab(comparison: CompletedComparison, labels: dict[str, str], cursor: int, symbol: str | None) -> None:
    model = comparison.model
    scope = compare_exports.scope_prefix(model, cursor, symbol)
    st.caption(f"Signal requests of both configurations in the {scope}. Signal ids repeat across the two runs: "
               "the scoped reference is the unique one.")
    order = table_order.control("cmp_order_signals", compare_tables.SIGNAL_ORDER_COLUMNS)
    table = compare_tables.signals_table(model, labels, cursor, symbol, order)
    if table.empty:
        st.info("No signal requests in the visible prefix." if cursor else "Nothing revealed yet.")
    else:
        st.dataframe(table, hide_index=True, width="stretch", column_config=table_order.column_config(table))
    prefix = model.signals_through(cursor, symbol)
    left, right = st.columns(2)
    left.download_button(f"Signals CSV - VISIBLE PREFIX ({len(prefix)} rows)",
                         compare_exports.signals_csv(comparison, prefix, scope), on_click="ignore", key="cmp_dl_signals",
                         mime="text/csv", file_name=compare_exports.signals_filename_prefix(comparison, cursor, symbol),
                         help=f"Only what the table above shows: {scope}.", width="stretch")
    right.download_button("Summary CSV - VISIBLE PREFIX (counts per configuration)",
                          compare_exports.summary_csv(comparison, cursor, symbol, scope), on_click="ignore",
                          key="cmp_dl_summary", mime="text/csv",
                          file_name=compare_exports.summary_filename_prefix(comparison, cursor, symbol),
                          help=f"Counts through the cursor: {scope}. The full-run versions are in the package "
                               "(Run details tab).", width="stretch")


def _diagnostics_tab(model, cursor: int, symbol: str | None) -> None:
    scope = compare_exports.scope_prefix(model, cursor, symbol)
    st.caption(f"One row per recorded event in the {scope}, with both configurations' decisions side by side.")
    order = table_order.control("cmp_order_diagnostics", compare_tables.DIAGNOSTIC_ORDER_COLUMNS)
    table = compare_tables.diagnostics_table(model, cursor, symbol, order)
    if table.empty:
        st.info("Nothing revealed yet." if cursor == 0 else "No rows for the displayed symbol yet.")
    else:
        st.dataframe(table, hide_index=True, width="stretch", column_config=table_order.column_config(table))


def _results(comparison: CompletedComparison) -> None:
    model = comparison.model
    labels = {r.config.member: r.config.short or r.config.label for r in comparison.runs}      # short names for narrow tables
    cursor = model.clamp(st.session_state.get(cs.K_CURSOR, 0))
    symbol = cs.symbol_filter(model)

    for run in comparison.runs:
        for warning in run.document.warnings:
            st.warning(f"{run.config.member}: `{warning.code}`: {warning.message}")
    if len({r.config.params for r in comparison.runs}) == 1 and len({r.config.kind for r in comparison.runs}) == 1:
        st.info("The two configurations are identical, so their results are identical: that is a valid comparison with no "
                "differences to show.")
    _cards(comparison)

    note = charts.compare_panel_note(model.symbols, symbol)
    if note:
        st.caption(note)
    members = [charts.Member(r.model, r.document.strategies[0], prefix=f"{r.config.member}: ",
                             title=f"{r.config.member}: {r.document.strategies[0].kind}") for r in comparison.runs]
    st.plotly_chart(charts.build_compare_figure(members, model.symbols, cursor, symbol, model.selected_event(cursor), model.total),
                    key="cmp_chart", width="stretch", theme=None, config={"displaylogo": False})
    playback.render_controls(model, cursor, symbol, playback.COMPARE_KEYS, cs.navigate, cs.on_slider, both=True)

    left, right = st.columns([3, 2])
    with left:
        _event_panel(model, labels, cursor, symbol)
    with right:
        _counts_panel(model, labels, cursor, symbol)

    signals_tab, diagnostics_tab, details_tab = st.tabs(["Signals", "Diagnostics", "Run details"])
    with signals_tab:
        _signals_tab(comparison, labels, cursor, symbol)
    with diagnostics_tab:
        _diagnostics_tab(model, cursor, symbol)
    with details_tab:
        compare_details.render_run_details(comparison)


def render(runner: Runner, catalog: Catalog) -> None:
    cs.ensure_initialized(catalog)
    side = compare_sidebar.render(catalog, runner)
    if side.run_clicked and side.source is not None and side.members is not None:
        _launch(runner, side)

    displayed = st.session_state.get(cs.K_DISPLAYED)
    failure = st.session_state.get(cs.K_FAILURE)
    status = state.run_status(displayed, failure, cs.current_request(runner, side.source, catalog))

    _header(displayed, status, side.source.display_name if side.source else None)
    if failure is not None:
        compare_details.render_failure(failure, displayed)
    compare_details.render_status_banner(status, displayed)
    if displayed is None:
        if failure is None:
            st.info("Choose a dataset and two configurations in the left panel, then press **Run comparison**. Both run on "
                    "the same data in separate engines; you then step through the recorded decisions of both here. "
                    "Signal requests only: no fills, returns or portfolio.")
    else:
        _results(displayed)

