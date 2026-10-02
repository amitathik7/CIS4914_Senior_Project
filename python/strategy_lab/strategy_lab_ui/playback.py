"""The replay controls shared by Explore and Compare: Reset, Previous, Next bar, Next signal, the display-symbol filter and
the timeline slider.

They only move a cursor and set a filter over a COMPLETED result (a callback edits session state; none can start the
engine). `model` is anything with `total`, `symbols`, `all_label`, `next_bar`, `previous_bar` and `next_signal`
(a `ReplayModel`, or the Compare model that answers for both members at once).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Protocol

import streamlit as st

from . import persist


class Navigable(Protocol):
    total: int
    symbols: tuple[str, ...]
    all_label: str

    def next_bar(self, cursor: int, symbol: str | None = None) -> int | None: ...
    def previous_bar(self, cursor: int, symbol: str | None = None) -> int | None: ...
    def next_signal(self, cursor: int, symbol: str | None = None) -> int | None: ...


@dataclass(frozen=True)
class PlaybackKeys:
    prefix: str          # button keys are f"{prefix}_reset", _prev, _next, _signal
    symbol: str          # session key of the display-symbol selectbox
    slider: str          # session key of the timeline slider


EXPLORE_KEYS = PlaybackKeys("nav", "lab_symbol", "lab_slider")
COMPARE_KEYS = PlaybackKeys("cmp_nav", "cmp_symbol", "cmp_slider")


def render_controls(model: Navigable, cursor: int, symbol: str | None, keys: PlaybackKeys,
                    navigate: Callable[[str], None], on_slider: Callable[[], None], *, both: bool = False) -> None:
    """`both` only changes the help texts: in Compare a signal means a request from either configuration."""
    who = "either configuration" if both else "the displayed symbol(s)"
    columns = st.columns([1, 1.3, 1.3, 1.5, 2.6], vertical_alignment="bottom")
    columns[0].button("Reset", key=f"{keys.prefix}_reset", on_click=navigate, args=("reset",), disabled=cursor == 0,
                      width="stretch", help="Back to position 0: nothing revealed.")
    columns[1].button("Previous", key=f"{keys.prefix}_prev", on_click=navigate, args=("previous",),
                      disabled=cursor == 0, width="stretch", help="One event back; later information disappears.")
    columns[2].button("Next bar", key=f"{keys.prefix}_next", on_click=navigate, args=("next",), width="stretch",
                      disabled=model.next_bar(cursor, symbol) is None,
                      help="Reveal the next recorded event (of the displayed symbol, if one is chosen).")
    columns[3].button("Next signal", key=f"{keys.prefix}_signal", on_click=navigate, args=("next_signal",),
                      width="stretch", disabled=model.next_signal(cursor, symbol) is None,
                      help=f"Jump to the next event that produced a signal request from {who}"
                           + (" for the displayed symbol(s)." if both else "."))
    with columns[4]:
        persist.selectbox("Display symbol (filters this result; does not rerun)", [model.all_label, *model.symbols],
                          key=keys.symbol,
                          help="Hides other symbols' rows. The configured allowlist, in the left panel, is what "
                               "decides what is computed.")
    # The slider mirrors the cursor, which lives in plain session state (it survives a visit to another view).
    st.session_state[keys.slider] = cursor
    persist.slider("Replay position (events revealed, in recorded order, all symbols)", 0, model.total,
                   key=keys.slider, on_change=on_slider)
