"""Tables for the Signals and Diagnostics tabs. Rows come only from the visible prefix and the display filter.

Identifiers are shown as text so they stay exact in the browser. An indicator that does not exist is an empty
cell AND is named, with its reason, in the "Unavailable" column; it is never shown as zero.
"""

from __future__ import annotations

import pandas as pd

from .catalog import StrategySpec
from .replay import PrefixCounts, ReplayModel

ACTION_TEXT = {"buy": "Buy request", "sell": "Sell request", "none": "No request"}
SIDE_TEXT = {"buy": "Buy request", "sell": "Sell request"}


def signals_table(model: ReplayModel, cursor: int, symbol: str | None) -> pd.DataFrame:
    rows = []
    for signal in model.signals_through(cursor, symbol):
        rows.append({
            "Signal": str(signal.signal_id),
            "Event": signal.event_index + 1,
            "Symbol": signal.symbol,
            "Request": SIDE_TEXT.get(signal.side, signal.side),
            "Quantity": None if signal.requested_quantity is None else float(signal.requested_quantity),
            "Created (UTC)": signal.created_at,
            "Trigger": signal.metadata.get("trigger", ""),
            "Details": "; ".join(f"{k}={v}" for k, v in signal.metadata.items() if k != "trigger"),
            "Reference": signal.signal_ref,
        })
    columns = ["Signal", "Event", "Symbol", "Request", "Quantity", "Created (UTC)", "Trigger", "Details", "Reference"]
    return pd.DataFrame(rows, columns=columns)


def diagnostics_table(model: ReplayModel, cursor: int, symbol: str | None, indicators: tuple[str, ...]) -> pd.DataFrame:
    rows = []
    for event in model.events_through(cursor, symbol):
        result = model.result_of(event)
        row: dict[str, object] = {
            "Event": event.index + 1,
            "Time (UTC)": event.exchange_time,
            "Symbol": event.symbol,
            "Type": event.type,
            "Close": None if event.price is None else float(event.price),
        }
        if result.diagnostics_available:
            fill = "?" if result.window_fill is None else result.window_fill
            row.update({"Verdict": result.verdict, "Reason": result.reason, "Action": ACTION_TEXT.get(result.action or "", result.action),
                        "Window": f"{fill}/{result.window_size}"})
            for name in indicators:
                value = result.indicators.get(name)
                row[name] = None if value is None else float(value)
            row["Unavailable"] = "; ".join(f"{k}: {v}" for k, v in result.unavailable.items())
        else:
            row.update({"Verdict": "no diagnostics", "Reason": result.unavailable_reason or "", "Action": "", "Window": ""})
            for name in indicators:
                row[name] = None
            row["Unavailable"] = "all: the strategy reported nothing for this event"
        row["Signals"] = ", ".join(str(i) for i in result.signal_ids)
        rows.append(row)
    columns = ["Event", "Time (UTC)", "Symbol", "Type", "Close", "Verdict", "Reason", "Action", "Window",
               *indicators, "Unavailable", "Signals"]
    return pd.DataFrame(rows, columns=columns)


def reasons_table(spec: StrategySpec | None, counts: PrefixCounts) -> pd.DataFrame:
    rows = []
    for code, count in sorted(counts.reasons.items(), key=lambda item: (-item[1], item[0])):
        known = next((r for r in spec.reasons if r.code == code), None) if spec else None
        rows.append({"Reason": code, "Verdict": known.verdict if known else "", "Events": count,
                     "Meaning": known.text if known else "(not in the catalog)"})
    return pd.DataFrame(rows, columns=["Reason", "Verdict", "Events", "Meaning"])
