"""The left configuration panel: dataset, strategy, parameters, Restore defaults, Run.

Every widget here only edits settings (session keys `ui_*`). The one thing that can start a replay is the Run
button, and `render` merely reports whether it was pressed in this script run.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import partial

import streamlit as st

from . import datasets, datasets_ui, persist, session
from .bridge import Runner
from .catalog import Catalog, ParamSpec, StrategySpec
from .datasets import DatasetSource
from .errors import LabUiError
from .explain import HOW_IT_WORKS

SYMBOL_HELP = ("Comma-separated, matched exactly and case-sensitively ('aapl' is not 'AAPL'); spaces around the "
               "commas are ignored. This allowlist decides what the strategy computes. The display filter above "
               "the replay controls only hides rows of a finished result.")


@dataclass(frozen=True)
class SidebarResult:
    source: DatasetSource | None
    problem: LabUiError | None
    spec: StrategySpec | None
    run_clicked: bool


def _dataset_section(catalog: Catalog) -> tuple[DatasetSource | None, LabUiError | None]:
    return datasets_ui.render(datasets_ui.EXPLORE_PICKER, partial(session.apply_demo_settings, catalog))


def _param_widget(spec: StrategySpec, param: ParamSpec) -> None:
    key = session.param_key(spec.kind, param.name)
    default = f" Default: {param.default}." if param.has_default else ""
    help_text = f"{param.name}. {param.constraint}{default} (Validated by the replay tool.)"
    if param.type == "uint":
        persist.number_input(param.name, min_value=0, step=1, format="%d", key=key, help=help_text)
    else:
        persist.text_input(param.name, key=key, help=help_text)


def _parameters_section(catalog: Catalog) -> StrategySpec | None:
    available = catalog.available
    if not available:
        st.error("The replay tool lists no available strategy.")
        return None
    persist.selectbox("Strategy", [s.kind for s in available], key=session.K_STRATEGY,
                      format_func=lambda kind: catalog.strategy(kind).title,
                      on_change=session.apply_demo_settings, args=(catalog,))
    spec = catalog.strategy(st.session_state[session.K_STRATEGY])
    assert spec is not None
    st.caption(spec.summary)
    with st.expander("How this strategy works"):
        st.markdown(HOW_IT_WORKS.get(spec.kind, spec.summary))
        st.caption(f"Full description: {spec.docs}")
    for other in catalog.strategies:
        if not other.available:
            st.caption(f"{other.title}: not available.", help=other.unavailable_reason)

    st.markdown("**Parameters**")
    for param in spec.params:                      # one per row: long names such as entry_threshold do not fit side by side
        if param.name not in session.ADVANCED_PARAMS and param.name != "symbols":
            _param_widget(spec, param)
    persist.text_input("symbols (allowlist)", key=session.K_SYMBOLS, help=SYMBOL_HELP)
    allowlist = [p for p in datasets.symbols_param(st.session_state[session.K_SYMBOLS]).split(",") if p]
    st.caption("Sent to the strategy: " + (" | ".join(allowlist) if allowlist else "nothing yet"))
    with st.expander("Advanced settings"):
        for param in spec.params:
            if param.name in session.ADVANCED_PARAMS:
                _param_widget(spec, param)
    return spec


def render(catalog: Catalog, runner: Runner) -> SidebarResult:
    with st.sidebar:
        st.markdown("### Configuration")
        source, problem = _dataset_section(catalog)
        spec = _parameters_section(catalog)
        with st.container(key="run_bar"):          # sticky at the bottom of the panel (see views.CSS)
            left, right = st.columns(2)
            left.button("Restore defaults", key="ui_restore", on_click=session.restore_defaults, args=(catalog,),
                        width="stretch", help="Reset the parameters to the strategy's own defaults (read from the "
                                              "C++ configuration) and the allowlist to the dataset's symbols.")
            run_clicked = right.button("Run replay", key="ui_run", type="primary", width="stretch",
                                       disabled=source is None or spec is None,
                                       help="Starts the C++ replay once, with exactly these settings. Only this "
                                            "button does: stepping, filtering and downloads never run the engine.")
    return SidebarResult(source, problem, spec, bool(run_clicked))
