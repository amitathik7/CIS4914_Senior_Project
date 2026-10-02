"""Replay over a COMPLETED result. Nothing here runs the engine.

The cursor is a number of events revealed, 0..N, in the order the tool recorded them (file order,
equal timestamps and interleaved symbols exactly as replayed). At cursor c, events 0..c-1 are the
"visible prefix" and event c-1 is the "selected event"; c = 0 shows nothing. Everything displayed
as the replay state (chart, markers, tables, counts) is derived from the prefix only.

Events and signals are joined by their integer `event_index` / `signal_id`, never by timestamp,
so equal timestamps across symbols stay distinct and large ids stay exact.

`symbol` arguments are a display filter: None means every symbol. A filter hides rows; it never
changes the cursor or any decision.
"""

from __future__ import annotations

from bisect import bisect_left
from collections import Counter
from dataclasses import dataclass
from typing import Mapping

from .schema import Event, ReplayDocument, Signal, StrategyEventResult


@dataclass(frozen=True)
class PrefixCounts:
    """Counts over the visible prefix (and the symbol filter): derived by counting recorded output only."""

    events: int
    bars: int
    buys: int
    sells: int
    verdicts: Mapping[str, int]      # ignored / warming_up / evaluated / unavailable
    reasons: Mapping[str, int]

    @property
    def signals(self) -> int:
        return self.buys + self.sells


@dataclass(frozen=True)
class SymbolSeries:
    """One symbol's revealed rows, as parallel lists ready for plotting."""

    symbol: str
    event_indexes: list[int]
    times: list[str]                 # axis position only (millisecond resolution); identity is the event's own text
    prices: list[float | None]
    verdicts: list[str | None]
    reasons: list[str | None]
    indicators: dict[str, list[float | None]]
    types: list[str]


def plotly_time(exchange_time: str) -> str:
    """'2026-01-05T14:30:00.123456789Z' -> '2026-01-05 14:30:00.123'. For placing a point on an axis only."""
    text = exchange_time.rstrip("Z").replace("T", " ")
    if "." in text:
        head, fraction = text.split(".", 1)
        return f"{head}.{(fraction + '000')[:3]}"
    return text


def _plain_float(value: int | float | None) -> float | None:
    """For drawing only (never for identity or joins). Huge integers that overflow float are not drawn."""
    if value is None:
        return None
    try:
        return float(value)
    except OverflowError:
        return None


class ReplayModel:
    def __init__(self, document: ReplayDocument, strategy_id: str | None = None) -> None:
        self.document = document
        self.slot = document.strategy_slot(strategy_id) if strategy_id else 0
        self.strategy_id = document.strategies[self.slot].strategy_id
        self.events: tuple[Event, ...] = document.events
        self.total = len(self.events)

        self._indexes: dict[str, list[int]] = {}
        for event in self.events:
            self._indexes.setdefault(event.symbol, []).append(event.index)
        listed = [s.symbol for s in document.dataset.symbols]
        self.symbols: tuple[str, ...] = tuple(listed + [s for s in self._indexes if s not in listed])
        # The display-filter entry meaning "every symbol". A symbol may be any printable text, so the label is
        # lengthened until it cannot be mistaken for one.
        label = "All symbols"
        while label in self.symbols:
            label += "*"
        self.all_label = label

        self.signals: tuple[Signal, ...] = tuple(sorted(
            (s for s in document.signals if s.strategy_id == self.strategy_id),
            key=lambda s: (s.event_index, s.signal_id)))
        self._signal_positions: dict[str | None, list[int]] = {None: sorted({s.event_index + 1 for s in self.signals})}
        for symbol in self.symbols:
            self._signal_positions[symbol] = sorted({s.event_index + 1 for s in self.signals if s.symbol == symbol})
        self._time_cache: dict[int, str] = {}

    # ---- the cursor -----------------------------------------------------------------------

    def clamp(self, cursor: int) -> int:
        return max(0, min(self.total, int(cursor)))

    def _matching(self, symbol: str | None) -> list[int] | range:
        return range(self.total) if symbol is None else self._indexes.get(symbol, [])

    def next_bar(self, cursor: int, symbol: str | None = None) -> int | None:
        """The position that reveals the next event of `symbol` (any symbol when None), or None at the end."""
        indexes = self._matching(symbol)
        k = bisect_left(indexes, cursor)
        return indexes[k] + 1 if k < len(indexes) else None

    def previous_bar(self, cursor: int, symbol: str | None = None) -> int | None:
        """The position showing the previous event of `symbol`; 0 once none is left; None at 0."""
        if cursor <= 0:
            return None
        indexes = self._matching(symbol)
        k = bisect_left(indexes, cursor - 1)
        return indexes[k - 1] + 1 if k > 0 else 0

    def next_signal(self, cursor: int, symbol: str | None = None) -> int | None:
        """The position of the next event, after the cursor, that produced a signal for the display filter.

        One stop per event: several signals on the same event (two strategies, say) are all listed with
        that event, so none is skipped."""
        positions = self._signal_positions.get(symbol, [])
        k = bisect_left(positions, cursor + 1)
        return positions[k] if k < len(positions) else None

    # ---- what the prefix shows ----------------------------------------------------------------

    def selected_event(self, cursor: int) -> Event | None:
        return self.events[cursor - 1] if 1 <= cursor <= self.total else None

    def result_of(self, event: Event) -> StrategyEventResult:
        return event.results[self.slot]

    def signals_on(self, event_index: int) -> tuple[Signal, ...]:
        return tuple(s for s in self.document.signals_by_event.get(event_index, ()) if s.strategy_id == self.strategy_id)

    def events_through(self, cursor: int, symbol: str | None = None) -> list[Event]:
        prefix = self.events[:self.clamp(cursor)]
        return list(prefix) if symbol is None else [e for e in prefix if e.symbol == symbol]

    def signals_through(self, cursor: int, symbol: str | None = None) -> list[Signal]:
        return [s for s in self.signals if s.event_index < cursor and (symbol is None or s.symbol == symbol)]

    def counts(self, cursor: int, symbol: str | None = None) -> PrefixCounts:
        verdicts: Counter[str] = Counter({"ignored": 0, "warming_up": 0, "evaluated": 0, "unavailable": 0})
        reasons: Counter[str] = Counter()
        bars = 0
        shown = self.events_through(cursor, symbol)
        for event in shown:
            bars += event.type == "bar"
            result = self.result_of(event)
            if not result.diagnostics_available:
                verdicts["unavailable"] += 1
                continue
            verdicts[result.verdict or "unavailable"] += 1
            reasons[result.reason or "unknown"] += 1
        signals = self.signals_through(cursor, symbol)
        return PrefixCounts(len(shown), bars, sum(s.side == "buy" for s in signals),
                            sum(s.side == "sell" for s in signals), dict(verdicts), dict(reasons))

    def time_text(self, event: Event) -> str:
        if event.index not in self._time_cache:
            self._time_cache[event.index] = plotly_time(event.exchange_time)
        return self._time_cache[event.index]

    def series(self, symbol: str, cursor: int, indicator_names: tuple[str, ...]) -> SymbolSeries:
        """The revealed rows of one symbol, with each named indicator (None where it does not exist)."""
        indexes = self._indexes.get(symbol, [])
        shown = indexes[:bisect_left(indexes, cursor)]
        out = SymbolSeries(symbol, [], [], [], [], [], {name: [] for name in indicator_names}, [])
        for index in shown:
            event = self.events[index]
            result = self.result_of(event)
            out.event_indexes.append(index)
            out.times.append(self.time_text(event))
            out.prices.append(_plain_float(event.price))
            out.verdicts.append(result.verdict)
            out.reasons.append(result.reason)
            out.types.append(event.type)
            for name in indicator_names:
                out.indicators[name].append(_plain_float(result.indicators.get(name)))
        return out
