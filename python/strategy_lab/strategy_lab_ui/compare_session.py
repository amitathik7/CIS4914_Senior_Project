"""Compare's session state: which key holds what, and every callback that changes it.

Keys, by the same four kinds of state as Explore (state.py):
  editable settings    cmp_dataset_mode, cmp_builtin_key, cmp_upload*, cmp_symbols, cmp_a_*, cmp_b_*   (widget values)
  run snapshot/result  cmp_displayed (CompletedComparison)   cmp_failure (FailedComparison)   cmp_launches (int)
  playback + filters   cmp_cursor, cmp_slider, cmp_symbol

The engine starts in exactly one place, `launch()`, which only the Run comparison button's branch calls. Every callback here
edits widget values or moves the cursor; none can start a replay.
"""

from __future__ import annotations

from typing import Any, Sequence

import streamlit as st

from . import compare, datasets, datasets_ui, persist, session, state
from .bridge import Runner
from .catalog import Catalog, StrategySpec
from .compare import MEMBERS, CompareRequest, CompletedComparison, ComparisonFailure, FailedComparison, MemberConfig
from .datasets import DatasetSource

K_INIT = "cmp_initialized"
K_SYMBOLS = "cmp_symbols"
K_KIND = {"A": "cmp_a_kind", "B": "cmp_b_kind"}
K_DISPLAYED = "cmp_displayed"
K_FAILURE = "cmp_failure"
K_LAUNCHES = "cmp_launches"
K_CURSOR = "cmp_cursor"
K_SLIDER = "cmp_slider"
K_SYMBOL = "cmp_symbol"

# Quick starts: the two kinds of comparison the lab supports. A is each strategy's demo preset for the chosen fixture; B is
# a clearly different setting of the same strategy (demo parameters for teaching, not recommendations).
VARIANT = {"sma_crossover": {"short_window": "1", "long_window": "2"},
           "mean_reversion": {"entry_threshold": "1", "rearm_threshold": "0.25"}}
VARIANT_FALLBACK = {"sma_crossover": {"short_window": "2", "long_window": "3"},
                    "mean_reversion": {"entry_threshold": "1.5", "rearm_threshold": "0.5"}}
QUICK_SMA_VS_MR = "sma_vs_mr"
QUICK_TWO_SMA = "two_sma"


def param_key(member: str, kind: str, name: str) -> str:
    return f"cmp_{member.lower()}_param.{kind}.{name}"


# ---- widget values <-> parameter texts ----------------------------------------------------------------

def member_params(member: str, spec: StrategySpec) -> list[tuple[str, str]]:
    """Every parameter of the member's strategy as the text that will be passed to the tool, catalog order. The symbols
    allowlist is one shared value: both configurations get exactly the same universe."""
    out = []
    for param in spec.params:
        if param.name == compare.SYMBOLS_PARAM:
            out.append((param.name, datasets.symbols_param(st.session_state.get(K_SYMBOLS, ""))))
        else:
            out.append((param.name, session.widget_to_text(param, st.session_state.get(param_key(member, spec.kind, param.name)))))
    return out


def current_members(catalog: Catalog) -> list[MemberConfig] | None:
    """The two configurations on screen, or None if a chosen strategy is not in the catalog."""
    out = []
    for member in MEMBERS:
        kind = st.session_state.get(K_KIND[member], "")
        spec = catalog.strategy(kind)
        if spec is None:
            return None
        params = member_params(member, spec)
        out.append(MemberConfig(member, kind, tuple(params), compare.make_label(member, spec, kind, params),
                                compare.make_short(member, spec, kind)))
    return out


def current_request(runner: Runner, source: DatasetSource | None, catalog: Catalog) -> CompareRequest | None:
    members = current_members(catalog)
    if source is None or members is None:
        return None
    return CompareRequest(source.kind, source.key, source.sha256, tuple(members), runner.sha256)


# ---- seeding and presets (callbacks; they only edit widget values) -----------------------------------------

def _set_defaults(member: str, spec: StrategySpec) -> None:
    for param in spec.params:
        if param.name != compare.SYMBOLS_PARAM:
            st.session_state[param_key(member, spec.kind, param.name)] = session.default_widget_value(param)


def _apply_values(member: str, spec: StrategySpec, values: dict[str, str]) -> None:
    for name, text in values.items():
        param = spec.param(name)
        if param is not None and name != compare.SYMBOLS_PARAM:
            st.session_state[param_key(member, spec.kind, name)] = int(text) if param.type == "uint" else text


def _preset_values(spec: StrategySpec) -> dict[str, str]:
    if st.session_state.get(datasets_ui.COMPARE_PICKER.mode) != datasets_ui.MODE_BUILTIN:
        return {}
    key = persist.recall(datasets_ui.COMPARE_PICKER.builtin, datasets.BUILTINS[0].key)
    try:
        return dict(datasets.builtin(key).presets.get(spec.kind, {}))
    except KeyError:
        return {}


def _seed_member(catalog: Catalog, member: str) -> None:
    spec = catalog.strategy(st.session_state.get(K_KIND[member], ""))
    if spec is not None:
        _set_defaults(member, spec)
        _apply_values(member, spec, _preset_values(spec))


def apply_dataset(catalog: Catalog) -> None:
    """A different dataset: the shared allowlist becomes its symbols and both members load their demo presets."""
    st.session_state[K_SYMBOLS] = ", ".join(datasets_ui.selected_symbols(datasets_ui.COMPARE_PICKER))
    for member in MEMBERS:
        _seed_member(catalog, member)


def apply_member(catalog: Catalog, member: str) -> None:
    """A different strategy for one member: that member loads its defaults and the dataset's demo preset."""
    _seed_member(catalog, member)


def restore_defaults(catalog: Catalog) -> None:
    """Both members' own strategy defaults (from the C++ configuration) and the dataset's symbols for the allowlist."""
    st.session_state[K_SYMBOLS] = ", ".join(datasets_ui.selected_symbols(datasets_ui.COMPARE_PICKER))
    for member in MEMBERS:
        spec = catalog.strategy(st.session_state.get(K_KIND[member], ""))
        if spec is not None:
            _set_defaults(member, spec)


def quick_start(catalog: Catalog, which: str) -> None:
    """Load a ready-made pair: SMA versus mean reversion, or two variants of the SMA crossover (demo parameters)."""
    available = [s.kind for s in catalog.available]
    wanted = {QUICK_SMA_VS_MR: ("sma_crossover", "mean_reversion"), QUICK_TWO_SMA: ("sma_crossover", "sma_crossover")}[which]
    if not all(kind in available for kind in wanted):
        return
    st.session_state[K_SYMBOLS] = ", ".join(datasets_ui.selected_symbols(datasets_ui.COMPARE_PICKER))
    for member, kind in zip(MEMBERS, wanted):
        st.session_state[K_KIND[member]] = kind
        _seed_member(catalog, member)
    if which == QUICK_TWO_SMA:
        spec = catalog.strategy("sma_crossover")
        assert spec is not None
        before = {n: st.session_state.get(param_key("A", spec.kind, n)) for n in ("short_window", "long_window")}
        _apply_values("B", spec, VARIANT[spec.kind])
        after = {n: st.session_state.get(param_key("B", spec.kind, n)) for n in ("short_window", "long_window")}
        if after == before:
            _apply_values("B", spec, VARIANT_FALLBACK[spec.kind])


def ensure_initialized(catalog: Catalog) -> None:
    if st.session_state.get(K_INIT):
        return
    st.session_state[datasets_ui.COMPARE_PICKER.mode] = datasets_ui.MODE_BUILTIN
    st.session_state[datasets_ui.COMPARE_PICKER.builtin] = datasets.BUILTINS[0].key
    available = [s.kind for s in catalog.available]
    st.session_state[K_KIND["A"]] = available[0] if available else ""
    st.session_state[K_KIND["B"]] = available[1] if len(available) > 1 else (available[0] if available else "")
    st.session_state[K_LAUNCHES] = 0
    st.session_state[K_CURSOR] = 0
    st.session_state[K_SLIDER] = 0
    apply_dataset(catalog)
    st.session_state[K_INIT] = True


# ---- playback (callbacks) ---------------------------------------------------------------------------

def symbol_filter(model: Any) -> str | None:
    value = st.session_state.get(K_SYMBOL)
    return value if value in model.symbols else None


def set_cursor(position: int) -> None:
    st.session_state[K_CURSOR] = position
    st.session_state[K_SLIDER] = position


def on_slider() -> None:
    st.session_state[K_CURSOR] = st.session_state.get(K_SLIDER, 0)


def navigate(action: str) -> None:
    run: CompletedComparison | None = st.session_state.get(K_DISPLAYED)
    if run is None:
        return
    model = run.model
    cursor, symbol = st.session_state.get(K_CURSOR, 0), symbol_filter(model)
    target = {"reset": 0, "previous": model.previous_bar(cursor, symbol), "next": model.next_bar(cursor, symbol),
              "next_signal": model.next_signal(cursor, symbol)}[action]
    if target is not None:
        set_cursor(target)


# ---- the one place the two engine runs start --------------------------------------------------------------

def launch(runner: Runner, source: DatasetSource, members: Sequence[MemberConfig],
           progress=None) -> CompletedComparison | FailedComparison:
    """Run the comparison for the settings on screen. Called only from the Run comparison button's branch."""
    number = st.session_state.get(K_LAUNCHES, 0) + 1
    st.session_state[K_LAUNCHES] = number
    request = CompareRequest(source.kind, source.key, source.sha256, tuple(members), runner.sha256)
    try:
        completed = compare.execute(runner, source, members, number, state.now(), progress)
    except ComparisonFailure as failure:
        failed = FailedComparison(request, failure.errors, failure.succeeded, number, state.now())
        st.session_state[K_FAILURE] = failed
        return failed
    st.session_state[K_DISPLAYED] = completed
    st.session_state[K_FAILURE] = None
    set_cursor(0)
    if st.session_state.get(K_SYMBOL) not in (completed.model.all_label, *completed.model.symbols):
        st.session_state[K_SYMBOL] = completed.model.all_label
    return completed
