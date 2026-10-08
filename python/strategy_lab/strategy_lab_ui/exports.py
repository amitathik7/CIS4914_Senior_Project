"""Downloads. Every export says whether it covers the FULL RUN or the VISIBLE REPLAY PREFIX.

* Run JSON: the runner's stdout, byte for byte (the whole run; its `provenance.result_sha256`
  still verifies against it). Never re-serialized, so no number or id passes through a float.
* Signals CSV: one row per signal request. The `export_scope` column repeats the scope on every row so the
  statement survives renaming the file. Spreadsheet programs may treat a cell starting with = + - @ as a
  formula; values are written exactly as recorded and are not altered to avoid that.
"""

from __future__ import annotations

import csv
import io
import re
from decimal import Decimal
from typing import Sequence

from .replay import ReplayModel
from .schema import Signal

BASE_COLUMNS = ("export_scope", "signal_id", "signal_ref", "strategy_id", "event_index", "event_number", "symbol",
                "side", "order_type", "requested_quantity", "created_at", "bus_sequence")


def scope_full(model: ReplayModel) -> str:
    return f"full run: all {model.total} events, all symbols"


def scope_prefix(model: ReplayModel, cursor: int, symbol: str | None) -> str:
    shown = "all symbols" if symbol is None else f"symbol {symbol}"
    first = "none revealed" if cursor == 0 else f"events 1-{cursor}"
    return f"visible replay prefix: {first} of {model.total}, {shown}"


def _text(value: int | float | Decimal | None) -> str:
    if value is None:
        return ""
    if isinstance(value, Decimal):
        return format(value, "f")         # the exact digits, never scientific notation
    return str(value) if isinstance(value, int) else repr(value)


def signals_csv(signals: Sequence[Signal], scope: str) -> bytes:
    keys = sorted({key for signal in signals for key in signal.metadata})
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow([*BASE_COLUMNS, *(f"metadata.{key}" for key in keys)])
    for signal in signals:
        writer.writerow([
            scope, signal.signal_id, signal.signal_ref, signal.strategy_id, signal.event_index,
            signal.event_index + 1, signal.symbol, signal.side, signal.order_type or "",
            _text(signal.requested_quantity), signal.created_at, _text(signal.bus_sequence),
            *(signal.metadata.get(key, "") for key in keys)])
    return buffer.getvalue().encode("utf-8")


def _slug(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text)[:32] or "x"


def json_filename(run_id: str) -> str:
    return f"strategy_lab_{_slug(run_id)}_full_run.json"


def csv_filename_full(run_id: str) -> str:
    return f"strategy_lab_{_slug(run_id)}_signals_full_run.csv"


def csv_filename_prefix(run_id: str, cursor: int, total: int, symbol: str | None) -> str:
    return f"strategy_lab_{_slug(run_id)}_signals_prefix_{cursor}of{total}_{_slug(symbol) if symbol else 'all'}.csv"
