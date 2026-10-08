"""Tables for the Signals and Diagnostics tabs. Rows come only from the visible prefix and the display filter.

Identifiers, prices and quantities are shown as exact text so they stay exact in the browser (a float would round
anything above 2^53). The table widget would sort such a column as text, so the exact numeric order is applied here, in
Python, on the exact values (`table_order`): `order` picks it. An indicator that does not exist is an empty cell AND is
named, with its reason, in the "Unavailable" column; it is never shown as zero. Indicators are DERIVED statistics
(averages, z-scores) and are floats.
"""

from __future__ import annotations

import pandas as pd

from .catalog import StrategySpec
from .replay import PrefixCounts, ReplayModel
from .schema import exact_text
from .table_order import RowOrder, exact_order

# The exact-number columns each table can be ordered by (see table_order).
SIGNAL_ORDER_COLUMNS = ("Signal", "Quantity")
DIAGNOSTIC_ORDER_COLUMNS = ("Close",)

ACTION_TEXT = {"buy": "Buy request", "sell": "Sell request", "none": "No request"}
SIDE_TEXT = {"buy": "Buy request", "sell": "Sell request"}


def signals_table(model: ReplayModel, cursor: int, symbol: str | None, order: RowOrder = RowOrder()) -> pd.DataFrame:
    rows, keys = [], []
    for signal in model.signals_through(cursor, symbol):
        keys.append({"Signal": signal.signal_id, "Quantity": signal.requested_quantity})
        rows.append({
            "Signal": str(signal.signal_id),
            "Event": signal.event_index + 1,
            "Symbol": signal.symbol,
            "Request": SIDE_TEXT.get(signal.side, signal.side),
            "Quantity": exact_text(signal.requested_quantity),
            "Created (UTC)": signal.created_at,
            "Trigger": signal.metadata.get("trigger", ""),
            "Details": "; ".join(f"{k}={v}" for k, v in signal.metadata.items() if k != "trigger"),
            "Reference": signal.signal_ref,
        })
    columns = ["Signal", "Event", "Symbol", "Request", "Quantity", "Created (UTC)", "Trigger", "Details", "Reference"]
    return pd.DataFrame(exact_order(rows, keys, order), columns=columns)


def diagnostics_table(model: ReplayModel, cursor: int, symbol: str | None, indicators: tuple[str, ...],
                      order: RowOrder = RowOrder()) -> pd.DataFrame:
    rows, keys = [], []
    for event in model.events_through(cursor, symbol):
        keys.append({"Close": event.price})
        result = model.result_of(event)
        row: dict[str, object] = {
            "Event": event.index + 1,
            "Time (UTC)": event.exchange_time,
            "Symbol": event.symbol,
            "Type": event.type,
            "Close": exact_text(event.price),
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
    return pd.DataFrame(exact_order(rows, keys, order), columns=columns)


def reasons_table(spec: StrategySpec | None, counts: PrefixCounts) -> pd.DataFrame:
    rows = []
    for code, count in sorted(counts.reasons.items(), key=lambda item: (-item[1], item[0])):
        known = next((r for r in spec.reasons if r.code == code), None) if spec else None
        rows.append({"Reason": code, "Verdict": known.verdict if known else "", "Events": count,
                     "Meaning": known.text if known else "(not in the catalog)"})
    return pd.DataFrame(rows, columns=["Reason", "Verdict", "Events", "Meaning"])
