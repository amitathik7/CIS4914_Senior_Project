"""The dataset chooser used by Explore and Compare: a built-in synthetic fixture, or an uploaded CSV.

Each view has its own keys, so choosing a dataset in one view never changes another view's settings.

A file-uploader widget cannot be restored by Streamlit after the view that showed it was left, so the uploaded name and bytes
are also kept in plain session state (`kept`, at most 8 MiB per view). When the uploader is empty but a file is kept, that file
is used and the panel says so, with a button to forget it; replacing the file in the uploader replaces it here too.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

import streamlit as st

from . import datasets, nav, persist
from .datasets import DatasetSource
from .errors import LabUiError

MODE_BUILTIN = "Built-in demo"
MODE_UPLOAD = "Upload CSV"
_HAD = "_widget_had_file"          # suffix of the key that remembers whether the uploader held a file on the previous draw


@dataclass(frozen=True)
class PickerKeys:
    mode: str
    builtin: str
    upload: str
    kept: str            # session key holding (file name, bytes) of the last uploaded file


EXPLORE_PICKER = PickerKeys("ui_dataset_mode", "ui_builtin_key", "ui_upload", "ui_upload_kept")
COMPARE_PICKER = PickerKeys("cmp_dataset_mode", "cmp_builtin_key", "cmp_upload", "cmp_upload_kept")


def current_upload(keys: PickerKeys) -> tuple[str, bytes] | None:
    """The file in use: the uploader's, else the one kept from earlier in this session."""
    widget = st.session_state.get(keys.upload)
    if widget is not None:
        st.session_state[keys.kept] = (widget.name, widget.getvalue())
    return st.session_state.get(keys.kept)


def _forget(keys: PickerKeys) -> None:
    st.session_state[keys.kept] = None


def selected_symbols(keys: PickerKeys) -> tuple[str, ...]:
    """Symbols of the dataset now selected (a suggestion for the allowlist)."""
    if st.session_state.get(keys.mode) == MODE_UPLOAD:
        upload = current_upload(keys)
        return datasets.detect_symbols(upload[1]) if upload is not None else ()
    try:
        return datasets.load_builtin(persist.recall(keys.builtin, datasets.BUILTINS[0].key)).symbols
    except (KeyError, LabUiError):
        return ()


def render(keys: PickerKeys, on_change: Callable[[], None]) -> tuple[DatasetSource | None, LabUiError | None]:
    persist.radio("Dataset source", [MODE_BUILTIN, MODE_UPLOAD], key=keys.mode, horizontal=True, on_change=on_change)
    try:
        if st.session_state[keys.mode] == MODE_BUILTIN:
            names = [d.key for d in datasets.BUILTINS]
            persist.selectbox("Built-in dataset", names, key=keys.builtin,
                              format_func=lambda k: datasets.builtin(k).label, on_change=on_change)
            item = datasets.builtin(st.session_state[keys.builtin])
            source = datasets.load_builtin(item.key)
            st.caption(f"{item.description} (Synthetic data.)")
            st.session_state[keys.kept + _HAD] = False
            return source, None
        upload = st.file_uploader("CSV file (strategy_lab_bars_csv/1)", type=["csv"], key=keys.upload,
                                  max_upload_size=datasets.MAX_UPLOAD_BYTES // 1048576,
                                  help="Columns: symbol, exchange_time (UTC, ...Z), type (bar or trade), price; "
                                       "optional open, high, low, volume. The replay tool validates every row "
                                       "and reports the line and column of any problem; nothing is repaired.",
                                  on_change=on_change)
        # The uploader's own remove button empties the widget while the user is still looking at it: that is a decision to drop
        # the file. Arriving from another view also finds it empty (Streamlit forgot it): that is not, so the kept file stays.
        if upload is None and st.session_state.get(keys.kept + _HAD) and nav.same_view_as_last_run():
            st.session_state[keys.kept] = None
        st.session_state[keys.kept + _HAD] = upload is not None
        chosen = current_upload(keys)
        if chosen is None:
            st.caption("Choose a file to enable Run. Uploaded data has unknown provenance.")
            return None, None
        try:
            source = datasets.from_upload(*chosen)
        except LabUiError as error:
            st.error(f"{error.title}: {error.message}")
            if upload is None:
                st.button("Forget this file", key=f"{keys.upload}_forget", on_click=_forget, args=(keys,))
            return None, error
        st.caption(f"{source.size:,} bytes, SHA-256 {source.sha256[:12]}... Provenance unknown.")
        if upload is None:
            st.caption(f"Using the file uploaded earlier in this session: **{source.display_name}**.")
            st.button("Forget this file", key=f"{keys.upload}_forget", on_click=_forget, args=(keys,))
        return source, None
    except LabUiError as error:
        st.error(f"{error.title}: {error.message}")
        return None, error
