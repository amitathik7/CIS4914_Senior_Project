"""Keep widget values when a view stops rendering them.

Streamlit forgets the value of any widget that was not drawn in a script run. The lab shows only one view (Explore,
Compare or Validate) at a time, so without help every setting of a view would vanish when the user looks at another one.
Each wrapper below restores the last value of its key *before* the widget is created and remembers it afterwards.

Only widgets that hold a value belong here. Buttons, download buttons and file uploaders cannot be set through session
state (uploads are retained as bytes instead, see `datasets_ui`).
"""

from __future__ import annotations

from typing import Any, Callable

import streamlit as st

_STORE = "_kept_widget_values"


def _sticky(widget: Callable[..., Any]) -> Callable[..., Any]:
    def call(*args: Any, key: str, **kwargs: Any) -> Any:
        store: dict[str, Any] = st.session_state.setdefault(_STORE, {})
        if key not in st.session_state and key in store:
            st.session_state[key] = store[key]
        value = widget(*args, key=key, **kwargs)
        store[key] = st.session_state[key]
        return value

    call.__name__ = getattr(widget, "__name__", "sticky_widget")
    return call


radio = _sticky(st.radio)
selectbox = _sticky(st.selectbox)
text_input = _sticky(st.text_input)
number_input = _sticky(st.number_input)
slider = _sticky(st.slider)
checkbox = _sticky(st.checkbox)


def recall(key: str, default: Any = None) -> Any:
    """The current value of a widget key, else the last value it had. For callbacks: a widget that is not drawn (the built-in
    dataset list while Upload is chosen) has lost its session value, but the callback may still need to know what it was."""
    if key in st.session_state:
        return st.session_state[key]
    return st.session_state.get(_STORE, {}).get(key, default)

