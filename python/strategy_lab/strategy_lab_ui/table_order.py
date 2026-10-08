"""Exact numeric row order for the sortable tables.

Prices, quantities and signal ids are shown as EXACT TEXT (see `schema.exact_text`). The table widget (`st.dataframe`) cannot
sort or show them exactly itself: it sorts a text column as text (`100` before `20` before `3`), and a numeric column is turned
into a floating-point number in the browser, which collapses distinct values above 2^53 (the largest price, 9223372036854.775807,
and the one below it both display as 9223372036854.773, and integers above 2^53 are flagged and mis-ordered). There is no option
to turn header sorting off.

So the exact order is explicit: a "Sort rows by" control orders the rows in Python, on the exact `int` / `Decimal` values (Python
compares an int with a Decimal exactly, and nothing becomes a float). The columns stay text, right-aligned. Clicking the header of
such a column still sorts as text: the control's help says so, and the control is the way to get numeric order.
"""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from typing import Any, Mapping, Sequence

import pandas as pd
import streamlit as st

from . import persist

RECORDED = "Recorded order"

HELP = ("Orders the rows by the EXACT value of a price, quantity or id: integers and decimals are compared exactly, never as "
        "floating-point numbers, and rows without a value come last. Clicking the header of one of these columns sorts as TEXT "
        "instead ('100' before '20'), because the table widget would otherwise round numbers above 2^53.")

# Columns of the sortable tables that hold an exact number as text: right-aligned like numbers.
EXACT_COLUMNS = ("Signal", "Signal id (raw)", "Quantity", "Close")

Exact = int | Decimal


@dataclass(frozen=True)
class RowOrder:
    """`column` is a key of the per-row exact values (RECORDED keeps the recorded order)."""

    column: str = RECORDED
    descending: bool = False


def choices(columns: Sequence[str]) -> dict[str, RowOrder]:
    """The labelled orders offered for a table whose exact columns are `columns`; the first is the recorded order."""
    out = {RECORDED: RowOrder()}
    for column in columns:
        out[f"{column}, low to high (exact)"] = RowOrder(column, False)
        out[f"{column}, high to low (exact)"] = RowOrder(column, True)
    return out


def exact_order(rows: list[Any], keys: Sequence[Mapping[str, Exact | None]], order: RowOrder) -> list[Any]:
    """`rows` in the exact numeric order of `order.column`, taken from the parallel `keys` (one mapping of exact values per row).
    Ties keep their recorded order (the sort is stable, also when descending) and rows with no value are last either way."""
    if order.column == RECORDED:
        return list(rows)
    present = [i for i, key in enumerate(keys) if key[order.column] is not None]
    absent = [i for i, key in enumerate(keys) if key[order.column] is None]
    present.sort(key=lambda i: keys[i][order.column], reverse=order.descending)
    return [rows[i] for i in present + absent]


def control(key: str, columns: Sequence[str]) -> RowOrder:
    """Draw the "Sort rows by" selector (its choice survives leaving the view) and return the chosen order."""
    options = choices(columns)
    left, _ = st.columns([1, 2])
    with left:
        label = persist.selectbox("Sort rows by", list(options), key=key, help=HELP)
    return options[label]


def column_config(table: pd.DataFrame) -> dict[str, Any]:
    """Right-align the exact-number text columns that `table` has."""
    return {name: st.column_config.TextColumn(alignment="right") for name in EXACT_COLUMNS if name in table.columns}
