"""The Compare configuration panel: ONE shared dataset and symbol allowlist, then configuration A and configuration B.

Every widget here only edits settings (session keys `cmp_*`). The one thing that can start the two replays is the Run
comparison button; `render` merely reports whether it was pressed in this script run.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import partial

import streamlit as st

from . import compare, compare_session as cs, datasets, datasets_ui, persist
from .bridge import Runner
from .catalog import Catalog, ParamSpec, StrategySpec
from .compare import MEMBERS, MemberConfig
from .datasets import DatasetSource
from .errors import LabUiError
from .sidebar import SYMBOL_HELP


@dataclass(frozen=True)
class CompareSidebar:
    source: DatasetSource | None
    problem: LabUiError | None
    members: list[MemberConfig] | None
    run_clicked: bool


def _param_widget(member: str, spec: StrategySpec, param: ParamSpec) -> None:
    key = cs.param_key(member, spec.kind, param.name)
    default = f" Default: {param.default}." if param.has_default else ""
    help_text = f"{param.name}. {param.constraint}{default} (Validated by the replay tool.)"
    if param.type == "uint":
        persist.number_input(param.name, min_value=0, step=1, format="%d", key=key, help=help_text)
    else:
        persist.text_input(param.name, key=key, help=help_text)


def _member_section(catalog: Catalog, member: str) -> None:
    available = catalog.available
    with st.expander(f"Configuration {member}", expanded=True):
        persist.selectbox("Strategy", [s.kind for s in available], key=cs.K_KIND[member],
                          format_func=lambda kind: catalog.strategy(kind).title,
                          on_change=partial(cs.apply_member, catalog, member))
        spec = catalog.strategy(st.session_state[cs.K_KIND[member]])
        if spec is None:
            return
        for param in spec.params:                      # one per row: the panel is too narrow for two names side by side
            if param.name not in ("strategy_id", "requested_quantity", compare.SYMBOLS_PARAM):
                _param_widget(member, spec, param)
        st.caption("Label: " + compare.make_label(member, spec, spec.kind, cs.member_params(member, spec)))
        st.caption("strategy_id and requested_quantity use the strategy's defaults in a comparison.")


def render(catalog: Catalog, runner: Runner) -> CompareSidebar:
    with st.sidebar:
        st.markdown("### Comparison setup")
        source, problem = datasets_ui.render(datasets_ui.COMPARE_PICKER, partial(cs.apply_dataset, catalog))
        persist.text_input("symbols (allowlist, shared by A and B)", key=cs.K_SYMBOLS, help=SYMBOL_HELP)
        allowlist = [p for p in datasets.symbols_param(st.session_state[cs.K_SYMBOLS]).split(",") if p]
        st.caption("Sent to both strategies: " + (" | ".join(allowlist) if allowlist else "nothing yet"))

        st.markdown("**Quick start** (demo parameters, not recommendations)")
        one, two = st.columns(2)
        one.button("SMA vs mean reversion", key="cmp_quick_sma_mr", on_click=cs.quick_start,
                   args=(catalog, cs.QUICK_SMA_VS_MR), width="stretch",
                   help="A: moving-average crossover. B: mean reversion. Each with the demo preset for this dataset.")
        two.button("Two SMA variants", key="cmp_quick_two_sma", on_click=cs.quick_start,
                   args=(catalog, cs.QUICK_TWO_SMA), width="stretch",
                   help="A: the demo preset. B: the same strategy with different windows.")
        if not catalog.available:
            st.error("The replay tool lists no available strategy.")
        else:
            for member in MEMBERS:
                _member_section(catalog, member)

        members = cs.current_members(catalog)
        with st.container(key="run_bar"):          # sticky at the bottom of the panel (see theme.CSS)
            left, right = st.columns(2)
            left.button("Restore defaults", key="cmp_restore", on_click=cs.restore_defaults, args=(catalog,),
                        width="stretch", help="Reset both configurations to their strategy's own defaults (read from the "
                                              "C++ configuration) and the allowlist to the dataset's symbols.")
            run_clicked = right.button("Run comparison", key="cmp_run", type="primary", width="stretch",
                                       disabled=source is None or members is None,
                                       help="Starts two independent C++ replays (A, then B) over the same dataset, once. "
                                            "Only this button does: stepping, filtering, downloads and switching views never do.")
    return CompareSidebar(source, problem, members, bool(run_clicked))
