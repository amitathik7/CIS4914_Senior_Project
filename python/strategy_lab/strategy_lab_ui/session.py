"""Streamlit session state: which key holds what, and every callback that changes it.

Keys, by the four kinds of state in state.py:
  editable settings   ui_*           (widget values)
  run snapshot/result lab_displayed  (CompletedRun)   lab_failure (FailedRun)   lab_launches (int)
  playback + filters  lab_cursor, lab_slider, lab_symbol

The engine is started in exactly one place, `launch()`, which only the Run button's branch calls.
Callbacks here move the cursor and edit widget values; none can start a replay.
"""

from __future__ import annotations

from typing import Any

import streamlit as st

from . import datasets, datasets_ui, persist, state
from .bridge import Runner, describe
from .catalog import Catalog, ParamSpec, StrategySpec
from .datasets import DatasetSource
from .errors import LabUiError

K_INIT = "ui_initialized"
K_MODE = "ui_dataset_mode"
K_BUILTIN = "ui_builtin_key"
K_UPLOAD = "ui_upload"
K_STRATEGY = "ui_strategy_kind"
K_SYMBOLS = "ui_symbols"
K_DISPLAYED = "lab_displayed"
K_FAILURE = "lab_failure"
K_LAUNCHES = "lab_launches"
K_CURSOR = "lab_cursor"
K_SLIDER = "lab_slider"
K_SYMBOL = "lab_symbol"

MODE_BUILTIN = datasets_ui.MODE_BUILTIN
MODE_UPLOAD = datasets_ui.MODE_UPLOAD
ADVANCED_PARAMS = ("strategy_id", "requested_quantity")

_CATALOGS: dict[tuple, Catalog] = {}
_catalog_loads = 0


def param_key(kind: str, name: str) -> str:
    return f"ui_param.{kind}.{name}"


# ---- the catalog (discovered separately from any run; cached per executable identity) -------

def catalog_for(runner: Runner) -> Catalog:
    global _catalog_loads
    if runner.identity not in _CATALOGS:
        _CATALOGS[runner.identity] = describe(runner)
        _catalog_loads += 1
    return _CATALOGS[runner.identity]


def catalog_loads() -> int:
    """How many times this process has asked the executable to `describe` itself (once per executable build)."""
    return _catalog_loads


# ---- widget values <-> parameter texts ----------------------------------------------------------

def default_widget_value(param: ParamSpec) -> Any:
    """The catalog default in the form the widget holds: ints for uint, text for everything else."""
    if not param.has_default:
        return ""
    if param.type == "uint":
        return int(param.default)
    return _as_text(param.default)


def _as_text(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return repr(value)
    return str(value)


def _widget_to_text(param: ParamSpec, value: Any) -> str:
    if value is None:
        return ""
    if param.type == "uint":
        return str(int(value))
    text = str(value)
    # Numbers: surrounding blanks are not part of the value. Free text (strategy_id) is passed as typed.
    return text.strip() if param.type == "double" else text


widget_to_text = _widget_to_text          # shared with the Compare view's parameter widgets


def current_params(spec: StrategySpec) -> list[tuple[str, str]]:
    """Every parameter of the strategy as the text that will be passed to the tool, catalog order."""
    out = []
    for param in spec.params:
        if param.name == "symbols":
            out.append(("symbols", datasets.symbols_param(st.session_state.get(K_SYMBOLS, ""))))
        else:
            out.append((param.name, _widget_to_text(param, st.session_state.get(param_key(spec.kind, param.name)))))
    return out


# ---- seeding and presets (callbacks; they only edit widget values) ---------------------------------

def _set_defaults(spec: StrategySpec) -> None:
    for param in spec.params:
        if param.name != "symbols":
            st.session_state[param_key(spec.kind, param.name)] = default_widget_value(param)


def current_source_symbols() -> tuple[str, ...]:
    """Symbols of the dataset now selected (a suggestion for the allowlist)."""
    return datasets_ui.selected_symbols(datasets_ui.EXPLORE_PICKER)


def apply_demo_settings(catalog: Catalog) -> None:
    """Strategy defaults, then the fixture's demo preset on top (built-in datasets only), then its symbols."""
    kind = st.session_state[K_STRATEGY]
    spec = catalog.strategy(kind)
    if spec is None:
        return
    _set_defaults(spec)
    st.session_state[K_SYMBOLS] = ", ".join(current_source_symbols())
    if st.session_state.get(K_MODE) == MODE_BUILTIN:
        preset = datasets.builtin(persist.recall(K_BUILTIN, datasets.BUILTINS[0].key)).presets.get(kind, {})
        for name, text in preset.items():
            param = spec.param(name)
            if name == "symbols":
                st.session_state[K_SYMBOLS] = ", ".join(text.split(","))
            elif param is not None:
                st.session_state[param_key(kind, name)] = int(text) if param.type == "uint" else text


def restore_defaults(catalog: Catalog) -> None:
    """The strategy's own defaults (from the C++ config), and the dataset's symbols for the allowlist."""
    spec = catalog.strategy(st.session_state[K_STRATEGY])
    if spec is not None:
        _set_defaults(spec)
        st.session_state[K_SYMBOLS] = ", ".join(current_source_symbols())


def ensure_initialized(catalog: Catalog) -> None:
    if st.session_state.get(K_INIT):
        return
    st.session_state[K_MODE] = MODE_BUILTIN
    st.session_state[K_BUILTIN] = datasets.BUILTINS[0].key
    available = catalog.available
    st.session_state[K_STRATEGY] = available[0].kind if available else ""
    st.session_state[K_LAUNCHES] = 0
    st.session_state[K_CURSOR] = 0
    st.session_state[K_SLIDER] = 0
    apply_demo_settings(catalog)
    st.session_state[K_INIT] = True


# ---- playback (callbacks) ---------------------------------------------------------------------------

def symbol_filter(model) -> str | None:
    """The display filter as a symbol, or None for every symbol (also when the stored choice is stale)."""
    value = st.session_state.get(K_SYMBOL)
    return value if value in model.symbols else None


def set_cursor(position: int) -> None:
    st.session_state[K_CURSOR] = position
    st.session_state[K_SLIDER] = position


def on_slider() -> None:
    st.session_state[K_CURSOR] = st.session_state.get(K_SLIDER, 0)


def navigate(action: str) -> None:
    run = st.session_state.get(K_DISPLAYED)
    if run is None:
        return
    model = run.model
    cursor, symbol = st.session_state.get(K_CURSOR, 0), symbol_filter(model)
    target: int | None = {"reset": 0, "previous": model.previous_bar(cursor, symbol),
                          "next": model.next_bar(cursor, symbol),
                          "next_signal": model.next_signal(cursor, symbol)}[action]
    if target is not None:
        set_cursor(target)


# ---- the one place the engine starts ------------------------------------------------------------------

def launch(runner: Runner, source: DatasetSource, spec: StrategySpec) -> None:
    """Run the replay for the settings on screen. Called only from the Run button's branch."""
    number = st.session_state.get(K_LAUNCHES, 0) + 1
    st.session_state[K_LAUNCHES] = number
    params = current_params(spec)
    request = state.make_request(source, runner, spec.kind, params)
    try:
        completed = state.execute(runner, source, spec.kind, params, number)
    except LabUiError as error:
        st.session_state[K_FAILURE] = state.failed(request, error, number)
        return
    st.session_state[K_DISPLAYED] = completed
    st.session_state[K_FAILURE] = None
    set_cursor(0)
    if st.session_state.get(K_SYMBOL) not in (completed.model.all_label, *completed.model.symbols):
        st.session_state[K_SYMBOL] = completed.model.all_label


def reject_before_launch(error: LabUiError) -> None:
    """A problem found before any process is started (nothing was launched, so the counter stays)."""
    st.session_state[K_FAILURE] = state.failed(None, error, st.session_state.get(K_LAUNCHES, 0))


def settings_snapshot(runner: Runner, source: DatasetSource | None, spec: StrategySpec | None) -> state.RunRequest | None:
    if source is None or spec is None:
        return None
    return state.make_request(source, runner, spec.kind, current_params(spec))

