"""Decision inspector: the recorded decision for the selected event, in the tool's own words.

Verdict, reason, action, window, indicators and states are shown exactly as recorded; the plain-language
reason text is the catalog's. A value that does not exist is shown as unavailable with its reason, never as 0.
"""

from __future__ import annotations

import pandas as pd
import streamlit as st

from .catalog import StrategySpec
from .replay import ReplayModel
from .schema import Event, StrategyEventResult
from .tables import ACTION_TEXT, SIDE_TEXT

VERDICT_BADGE = {"evaluated": ("Evaluated", "blue"), "warming_up": ("Warming up", "gray"), "ignored": ("Ignored", "orange")}


def _window_text(result: StrategyEventResult) -> str:
    if result.window_fill is None:
        return f"not tracked for this event (window size {result.window_size})"
    left = result.window_remaining or 0
    return (f"{result.window_fill} of {result.window_size} accepted bars"
            + (" (full)" if left == 0 else f" ({left} more needed)"))


def _indicator_frame(result: StrategyEventResult, names: tuple[str, ...]) -> pd.DataFrame:
    rows = []
    for name in dict.fromkeys([*names, *result.indicators, *result.unavailable]):
        if name in result.indicators:
            rows.append({"Indicator": name, "Value": repr(result.indicators[name]) if isinstance(result.indicators[name], float)
                         else str(result.indicators[name]), "Why unavailable": ""})
        else:
            rows.append({"Indicator": name, "Value": "unavailable", "Why unavailable": result.unavailable.get(name, "not reported")})
    return pd.DataFrame(rows, columns=["Indicator", "Value", "Why unavailable"])


def render_inspector(model: ReplayModel, spec: StrategySpec | None, cursor: int, symbol: str | None) -> None:
    st.markdown("#### Decision inspector")
    event: Event | None = model.selected_event(cursor)
    if event is None:
        st.info("No event selected: at position 0 nothing has been revealed. Step forward to inspect the first "
                "recorded decision.")
        return
    result = model.result_of(event)
    st.markdown(f"**Event {event.index + 1} of {model.total}** - {event.symbol} - {event.type} - "
                f"`{event.exchange_time}`")
    price = "no price" if event.price is None else f"close {event.price}"
    st.caption(f"{price}; source line {event.source_line if event.source_line is not None else 'unknown'}; "
               f"bus sequence {event.bus_sequence}.")
    if symbol is not None and event.symbol != symbol:
        st.info(f"This event belongs to {event.symbol}, not the displayed symbol ({symbol}). The chart and tables "
                f"show {symbol} only; the cursor follows the recorded order of all symbols.")

    if not result.diagnostics_available:
        st.warning("The strategy reported nothing for this event. "
                   + (result.unavailable_reason or ""))
    else:
        label, color = VERDICT_BADGE.get(result.verdict or "", (result.verdict or "unknown", "gray"))
        st.badge(label, color=color)
        meaning = spec.reason_text(result.reason or "") if spec else None
        st.markdown(f"**Reason:** `{result.reason}`" + (f" - {meaning}" if meaning else ""))
        st.markdown(f"**Outcome:** {ACTION_TEXT.get(result.action or '', result.action)}"
                    + ("" if result.action != "none" else " (a silent bar is not a hold signal)"))
        st.markdown(f"**Window:** {_window_text(result)}")
        st.table(_indicator_frame(result, spec.indicators if spec else ()), hide_index=True)
        if result.states:
            st.caption("State: " + "; ".join(f"{k} = {v}" for k, v in result.states.items()))

    signals = model.signals_on(event.index)
    if signals:
        st.markdown(f"**{len(signals)} signal request{'s' if len(signals) != 1 else ''} on this event**")
        for signal in signals:
            st.markdown(f"- **{SIDE_TEXT.get(signal.side, signal.side)} #{signal.signal_id}** ({signal.signal_ref}), "
                        f"{signal.order_type or 'no order type'}, quantity {signal.requested_quantity}, "
                        f"created `{signal.created_at}`")
            if signal.metadata:
                st.caption("; ".join(f"{k} = {v}" for k, v in signal.metadata.items()))
