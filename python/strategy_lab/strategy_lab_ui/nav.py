"""The title row and the view switch (Explore, Compare, Validate).

Only the chosen view is drawn, so each view's results live in plain session state and each view's widget values are kept by
`persist` (see there). Switching views never starts the engine.
"""

from __future__ import annotations

import streamlit as st

EXPLORE, COMPARE, VALIDATE = "Explore", "Compare", "Validate"
VIEWS = (EXPLORE, COMPARE, VALIDATE)
K_VIEW = "ui_view"
K_LAST = "ui_last_view"          # the view drawn in the previous script run
K_PREV = "ui_prev_view"

_SUBTITLE = {
    EXPLORE: "Signal replay: one strategy, step through its decisions",
    COMPARE: "Two configurations on the same data, side by side",
    VALIDATE: "Scenario checks against hand-derived expectations",
}


def render() -> str:
    left, right = st.columns([4, 6], vertical_alignment="center")
    with right:
        with st.container(key="view_nav"):
            view = st.radio("View", VIEWS, key=K_VIEW, horizontal=True, label_visibility="collapsed")
    left.markdown("#### Strategy Lab")
    left.caption(_SUBTITLE[view])
    st.session_state[K_PREV] = st.session_state.get(K_LAST)
    st.session_state[K_LAST] = view
    return view


def same_view_as_last_run() -> bool:
    """True when the user is still in the view they were in on the previous run (not just arriving from another one)."""
    return st.session_state.get(K_PREV) == st.session_state.get(K_LAST)
