"""The Explore view: one strategy, one dataset, one replay you can step through.

The replay starts in one place, `session.launch()`, and only when the Run button was pressed in this script run.
"""

from __future__ import annotations

import streamlit as st

from . import details, session, sidebar, state, views
from .bridge import Runner
from .catalog import Catalog


def render(runner: Runner, catalog: Catalog) -> None:
    session.ensure_initialized(catalog)

    side = sidebar.render(catalog, runner)
    if side.run_clicked and side.source is not None and side.spec is not None:
        with st.spinner("Running strategy_lab_replay..."):
            session.launch(runner, side.source, side.spec)

    displayed = st.session_state.get(session.K_DISPLAYED)
    failure = st.session_state.get(session.K_FAILURE)
    current = session.settings_snapshot(runner, side.source, side.spec)
    status = state.run_status(displayed, failure, current)

    views.render_header(displayed, status, side.source.display_name if side.source else None)
    if failure is not None:
        details.render_failure(failure, displayed)
    details.render_status_banner(status, displayed)
    if displayed is None:
        views.render_empty(failure)
    else:
        views.render_results(displayed, catalog.strategy(displayed.request.strategy_kind))
