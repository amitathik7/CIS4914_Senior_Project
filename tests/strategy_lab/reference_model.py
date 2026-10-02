"""An INDEPENDENT reference for the two reference strategies, in exact rational arithmetic.

TEST-ONLY. This is an oracle for the tests in this directory, written from the strategy documents
(docs/strategies/*.md) and NOT from the C++ code: it uses ``fractions.Fraction`` end to end, so none
of the C++ floating-point machinery (compensated sums, power-of-two scaling, tolerances) is shared
with it. It is never used by the Strategy Lab tool or UI, which keep every decision in C++.

Where the documented rules involve a floating-point tolerance (equal averages within 1e-12, a
constant window within 1e-12 of the mean) this model uses the exact meaning (equal; zero variance).
The fixtures it is run on avoid the band between the two, so the answers must agree.
"""

from __future__ import annotations

import csv
import sys
from dataclasses import dataclass, field
from fractions import Fraction
from math import sqrt
from pathlib import Path

DBL_MAX = Fraction(sys.float_info.max)


@dataclass
class Row:
    line: int
    symbol: str
    time: str
    type: str
    price: Fraction


def load_rows(path: str | Path) -> list[Row]:
    rows: list[Row] = []
    with open(path, newline="", encoding="utf-8") as handle:
        for number, record in enumerate(csv.DictReader(handle), start=2):
            rows.append(Row(number, record["symbol"], record["exchange_time"], record["type"], Fraction(record["price"])))
    return rows


@dataclass
class Decision:
    """What the model says about ONE event for ONE strategy."""

    verdict: str
    reason: str
    action: str = "none"
    window_fill: int | None = None
    numbers: dict[str, float] = field(default_factory=dict)   # only the ones that exist


def _ignored(rows_state: dict | None, reason: str, size: int) -> Decision:
    fill = None if rows_state is None else len(rows_state["closes"])
    return Decision("ignored", reason, window_fill=fill)


def sma_crossover(rows: list[Row], short: int, long: int, symbols: list[str]) -> list[Decision]:
    """docs/strategies/moving_average_crossover.md sections 1, 3 and 4."""
    state = {symbol: {"closes": [], "relation": None} for symbol in symbols}
    ceiling = DBL_MAX / (2 * (long + 1))
    out: list[Decision] = []
    for row in rows:
        if row.type != "bar":
            out.append(_ignored(None, "not_a_bar", long))
            continue
        if row.symbol not in state:
            out.append(_ignored(None, "symbol_not_allowlisted", long))
            continue
        mine = state[row.symbol]
        if row.price > ceiling:
            out.append(_ignored(mine, "price_above_max_close", long))
            continue
        mine["closes"].append(row.price)
        del mine["closes"][:-long]
        fill = len(mine["closes"])
        if fill < long:
            out.append(Decision("warming_up", "warming_up", window_fill=fill))
            continue
        short_avg = sum(mine["closes"][-short:]) / short
        long_avg = sum(mine["closes"]) / long
        numbers = {"short_sma": float(short_avg), "long_sma": float(long_avg), "short_minus_long": float(short_avg - long_avg)}
        gap = short_avg - long_avg
        if gap == 0:
            out.append(Decision("evaluated", "averages_equal", window_fill=fill, numbers=numbers))
            continue
        now = "above" if gap > 0 else "below"
        before = mine["relation"]
        mine["relation"] = now
        if before is None:
            out.append(Decision("evaluated", "baseline_established", window_fill=fill, numbers=numbers))
        elif before == now:
            out.append(Decision("evaluated", "same_side", window_fill=fill, numbers=numbers))
        elif now == "above":
            out.append(Decision("evaluated", "crossover_buy", "buy", fill, numbers))
        else:
            out.append(Decision("evaluated", "crossover_sell", "sell", fill, numbers))
    return out


def mean_reversion(rows: list[Row], lookback: int, entry: str, rearm: str, symbols: list[str]) -> list[Decision]:
    """docs/strategies/mean_reversion.md sections 1 and 4. Thresholds are compared by squaring, so no square
    root enters a decision: z <= -entry  <=>  (close - mean) < 0 and (close - mean)^2 >= entry^2 * variance."""
    entry_f, rearm_f = Fraction(entry), Fraction(rearm)
    state = {symbol: {"closes": [], "latch": "neutral"} for symbol in symbols}
    out: list[Decision] = []
    for row in rows:
        if row.type != "bar":
            out.append(_ignored(None, "not_a_bar", lookback))
            continue
        if row.symbol not in state:
            out.append(_ignored(None, "symbol_not_allowlisted", lookback))
            continue
        mine = state[row.symbol]
        mine["closes"].append(row.price)
        del mine["closes"][:-lookback]
        fill = len(mine["closes"])
        if fill < lookback:
            out.append(Decision("warming_up", "warming_up", window_fill=fill))
            continue
        mean = sum(mine["closes"]) / lookback
        variance = sum((c - mean) ** 2 for c in mine["closes"]) / lookback   # population: divide by N
        deviation = row.price - mean
        numbers = {"mean": float(mean), "standard_deviation": sqrt(float(variance))}
        if variance == 0:
            mine["latch"] = "neutral"
            out.append(Decision("evaluated", "constant_window", window_fill=fill, numbers=numbers))
            continue
        numbers["z_score"] = float(deviation) / sqrt(float(variance))
        squared = deviation * deviation
        buy_due = deviation < 0 and squared >= entry_f * entry_f * variance and mine["latch"] != "lower_extreme"
        sell_due = deviation > 0 and squared >= entry_f * entry_f * variance and mine["latch"] != "upper_extreme"
        if buy_due:
            mine["latch"] = "lower_extreme"
            out.append(Decision("evaluated", "entry_buy", "buy", fill, numbers))
        elif sell_due:
            mine["latch"] = "upper_extreme"
            out.append(Decision("evaluated", "entry_sell", "sell", fill, numbers))
        elif squared <= rearm_f * rearm_f * variance:
            mine["latch"] = "neutral"
            out.append(Decision("evaluated", "inside_rearm_band", window_fill=fill, numbers=numbers))
        elif squared >= entry_f * entry_f * variance:
            out.append(Decision("evaluated", "excursion_already_requested", window_fill=fill, numbers=numbers))
        else:
            out.append(Decision("evaluated", "between_bands", window_fill=fill, numbers=numbers))
    return out
