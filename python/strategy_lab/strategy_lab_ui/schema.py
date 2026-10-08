"""Parse and validate a `strategy_lab.replay` document (schema version 1.x).

Nothing is displayed until the whole structure has been checked, including the links the replay
depends on: event indexes are 0..N-1 in order, every signal points at the event that produced it
(`event_index`, `signal_id`, `strategy_id`, symbol), and every `signal_ids` entry on an event names
a real signal. Joins use those integers, never timestamps and never floats.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal
from typing import Any, Mapping

from . import jsonio as j
from .catalog import REPLAY_SCHEMA, check_envelope
from .errors import LabUiError

Number = int | float          # a DERIVED statistic (an average, a z-score)
Price = int | Decimal         # an exact price: the decimal text of an int64 count of 1e-6, never a float


def exact_text(value: int | Decimal | None) -> str:
    """A price or a whole-share quantity as its exact decimal text, for every place a person reads it (tables,
    captions, hovers, exports). Never through a float, never scientific notation; None is the empty string. The
    only float made from a price is the plotting coordinate in `replay.plot_float`."""
    if value is None:
        return ""
    return format(value, "f") if isinstance(value, Decimal) else str(value)


@dataclass(frozen=True, slots=True)
class StrategyEventResult:
    strategy_id: str
    diagnostics_available: bool
    verdict: str | None
    reason: str | None
    action: str | None
    window_fill: int | None
    window_remaining: int | None
    window_size: int | None
    indicators: Mapping[str, Number]
    unavailable: Mapping[str, str]
    states: Mapping[str, str]
    signal_ids: tuple[int, ...]
    unavailable_reason: str | None = None


@dataclass(frozen=True, slots=True)
class Event:
    index: int
    source_line: int | None
    symbol: str
    exchange_time: str                 # RFC 3339 UTC, nine fractional digits: identity, never converted
    type: str
    price: Price | None
    price_status: str | None           # always None now: an integer price is never non-finite (kept for older documents)
    open: Price | None
    high: Price | None
    low: Price | None
    volume: int | None                 # a whole number of shares
    bus_sequence: int
    results: tuple[StrategyEventResult, ...]


@dataclass(frozen=True, slots=True)
class Signal:
    signal_id: int
    signal_ref: str
    strategy_id: str
    event_index: int
    symbol: str
    side: str
    order_type: str | None
    requested_quantity: int | None     # a whole number of shares (an exact int64)
    limit_price: Price | None
    created_at: str
    bus_sequence: int | None
    metadata: Mapping[str, str]


@dataclass(frozen=True, slots=True)
class PublicationFailure:
    signal: Signal
    kind: str
    message: str


@dataclass(frozen=True, slots=True)
class StrategyConfig:
    kind: str
    strategy_id: str
    window_size: int
    parameters: Mapping[str, Any]
    derived: Mapping[str, Any]


@dataclass(frozen=True, slots=True)
class SymbolSummary:
    symbol: str
    bar_rows: int
    trade_rows: int
    first_exchange_time: str
    last_exchange_time: str


@dataclass(frozen=True, slots=True)
class DatasetInfo:
    name: str
    format: str
    sha256: str
    bytes: int
    rows: int
    first_exchange_time: str
    last_exchange_time: str
    symbols: tuple[SymbolSummary, ...]


@dataclass(frozen=True, slots=True)
class RunWarning:
    code: str
    message: str
    strategy_id: str | None


@dataclass(frozen=True, eq=False)
class ReplayDocument:
    schema_version: str
    provenance: Mapping[str, Any]
    run_id: str
    notice: str
    context: Mapping[str, str]
    dataset: DatasetInfo
    max_rows: int
    bus_fault: str
    strategies: tuple[StrategyConfig, ...]
    warnings: tuple[RunWarning, ...]
    events: tuple[Event, ...]
    signals: tuple[Signal, ...]
    publication_failures: tuple[PublicationFailure, ...]
    engine: Mapping[str, Any]
    summary: Mapping[str, Any]
    signals_by_event: Mapping[int, tuple[Signal, ...]] = field(default_factory=dict)

    def strategy_slot(self, strategy_id: str) -> int:
        for slot, config in enumerate(self.strategies):
            if config.strategy_id == strategy_id:
                return slot
        raise KeyError(strategy_id)


# ---- leaf parsers ----------------------------------------------------------------------

_STATUS = ("nan", "inf", "-inf")


def _optional_number(item: Mapping[str, Any], key: str, path: str,
                     check: Any = j.exact_number) -> tuple[Any, str | None]:
    """A number that may be omitted; if not finite it is omitted and `<key>_status` says why. `check` is
    j.exact_number for a price, j.integer for a quantity or volume, j.number for a derived statistic."""
    value = j.opt(item, key, path, check)
    status = j.opt(item, f"{key}_status", path, j.string)
    if status is not None and status not in _STATUS:
        raise LabUiError("invalid_structure", f"{path}.{key}_status: unknown value '{status}'.")
    return value, status


def _event_result(raw: Any, path: str) -> StrategyEventResult:
    item = j.obj(raw, path)
    available = j.req(item, "diagnostics_available", path, j.boolean)
    ids = tuple(j.integer(v, f"{path}.signal_ids[{i}]", minimum=0)
                for i, v in enumerate(j.req(item, "signal_ids", path, j.arr)))
    strategy_id = j.req(item, "strategy_id", path, j.string)
    if not available:
        return StrategyEventResult(strategy_id, False, None, None, None, None, None, None, {}, {}, {}, ids,
                                   j.opt(item, "diagnostics_unavailable_reason", path, j.string))
    window = j.req(item, "window", path, j.obj)
    wpath = f"{path}.window"
    return StrategyEventResult(
        strategy_id=strategy_id,
        diagnostics_available=True,
        verdict=j.req(item, "verdict", path, j.string),
        reason=j.req(item, "reason", path, j.string),
        action=j.req(item, "action", path, j.string),
        window_fill=j.opt(window, "fill", wpath, lambda v, p: j.integer(v, p, minimum=0)),
        window_remaining=j.opt(window, "remaining", wpath, lambda v, p: j.integer(v, p, minimum=0)),
        window_size=j.req(window, "size", wpath, lambda v, p: j.integer(v, p, minimum=0)),
        indicators={k: j.number(v, f"{path}.indicators.{k}")
                    for k, v in j.req(item, "indicators", path, j.obj).items()},
        unavailable=j.string_map(j.req(item, "unavailable", path, j.obj), f"{path}.unavailable"),
        states=j.string_map(j.req(item, "states", path, j.obj), f"{path}.states"),
        signal_ids=ids,
    )


def _event(raw: Any, path: str, strategy_count: int) -> Event:
    item = j.obj(raw, path)
    results = tuple(_event_result(r, f"{path}.results[{i}]")
                    for i, r in enumerate(j.req(item, "results", path, j.arr)))
    if len(results) != strategy_count:
        raise LabUiError("invalid_structure",
                         f"{path}.results: expected {strategy_count} entries (one per strategy), got {len(results)}.")
    price, price_status = _optional_number(item, "price", path)
    return Event(
        index=j.req(item, "index", path, lambda v, p: j.integer(v, p, minimum=0)),
        source_line=j.opt(item, "source_line", path, lambda v, p: j.integer(v, p, minimum=1)),
        symbol=j.req(item, "symbol", path, j.string),
        exchange_time=j.req(item, "exchange_time", path, j.string),
        type=j.req(item, "type", path, j.string),
        price=price, price_status=price_status,
        open=_optional_number(item, "open", path)[0],
        high=_optional_number(item, "high", path)[0],
        low=_optional_number(item, "low", path)[0],
        volume=_optional_number(item, "volume", path, j.integer)[0],
        bus_sequence=j.req(item, "bus_sequence", path, lambda v, p: j.integer(v, p, minimum=0)),
        results=results,
    )


def _signal(raw: Any, path: str, run_id: str, *, with_sequence: bool) -> Signal:
    item = j.obj(raw, path)
    signal_id = j.req(item, "signal_id", path, lambda v, p: j.integer(v, p, minimum=0))
    ref = j.req(item, "signal_ref", path, j.string)
    if ref != f"{run_id}:{signal_id}":
        raise LabUiError("invalid_structure",
                         f"{path}.signal_ref: '{ref}' is not '{run_id}:{signal_id}' (the run-scoped reference).")
    return Signal(
        signal_id=signal_id, signal_ref=ref,
        strategy_id=j.req(item, "strategy_id", path, j.string),
        event_index=j.req(item, "event_index", path, lambda v, p: j.integer(v, p, minimum=0)),
        symbol=j.req(item, "symbol", path, j.string),
        side=j.req(item, "side", path, j.string),
        order_type=j.opt(item, "order_type", path, j.string),
        requested_quantity=_optional_number(item, "requested_quantity", path, j.integer)[0],
        limit_price=_optional_number(item, "limit_price", path)[0],
        created_at=j.req(item, "created_at", path, j.string),
        bus_sequence=(j.req(item, "bus_sequence", path, lambda v, p: j.integer(v, p, minimum=0))
                      if with_sequence else None),
        metadata=j.string_map(j.req(item, "metadata", path, j.obj), f"{path}.metadata"),
    )


def _dataset(body: Mapping[str, Any]) -> DatasetInfo:
    path = "$.result.input.dataset"
    item = j.req(j.req(body, "input", "$.result", j.obj), "dataset", "$.result.input", j.obj)
    symbols = []
    for i, raw in enumerate(j.req(item, "symbols", path, j.arr)):
        sp = f"{path}.symbols[{i}]"
        s = j.obj(raw, sp)
        symbols.append(SymbolSummary(
            j.req(s, "symbol", sp, j.string),
            j.req(s, "bar_rows", sp, lambda v, p: j.integer(v, p, minimum=0)),
            j.req(s, "trade_rows", sp, lambda v, p: j.integer(v, p, minimum=0)),
            j.req(s, "first_exchange_time", sp, j.string),
            j.req(s, "last_exchange_time", sp, j.string)))
    return DatasetInfo(
        name=j.req(item, "name", path, j.string), format=j.req(item, "format", path, j.string),
        sha256=j.req(item, "sha256", path, j.string),
        bytes=j.req(item, "bytes", path, lambda v, p: j.integer(v, p, minimum=0)),
        rows=j.req(item, "rows", path, lambda v, p: j.integer(v, p, minimum=0)),
        first_exchange_time=j.req(item, "first_exchange_time", path, j.string),
        last_exchange_time=j.req(item, "last_exchange_time", path, j.string),
        symbols=tuple(symbols))


def _parameters(raw: Mapping[str, Any], path: str) -> dict[str, Any]:
    """The parameters a strategy ran with. A whole-share quantity must be an exact integer (a Decimal or float there is a
    contract violation and is refused, never converted); the other parameters are ratios, windows and text, where a decimal
    number is a float."""
    if "requested_quantity" in raw:
        j.integer(raw["requested_quantity"], f"{path}.requested_quantity", minimum=1)
    return j.plain(dict(raw))


def _configuration(body: Mapping[str, Any]) -> tuple[int, str, tuple[StrategyConfig, ...]]:
    path = "$.result.configuration"
    item = j.req(body, "configuration", "$.result", j.obj)
    configs = []
    for i, raw in enumerate(j.req(item, "strategies", path, j.arr)):
        sp = f"{path}.strategies[{i}]"
        s = j.obj(raw, sp)
        configs.append(StrategyConfig(
            kind=j.req(s, "kind", sp, j.string), strategy_id=j.req(s, "strategy_id", sp, j.string),
            window_size=j.req(s, "window_size", sp, lambda v, p: j.integer(v, p, minimum=0)),
            parameters=_parameters(j.req(s, "parameters", sp, j.obj), f"{sp}.parameters"),
            derived=j.plain(dict(j.req(s, "derived", sp, j.obj)))))
    if not configs:
        raise LabUiError("invalid_structure", f"{path}.strategies: no strategy was run.")
    ids = [c.strategy_id for c in configs]
    if len(set(ids)) != len(ids):
        raise LabUiError("invalid_structure", f"{path}.strategies: a strategy_id appears twice.")
    return (j.req(item, "max_rows", path, lambda v, p: j.integer(v, p, minimum=1)),
            j.req(item, "bus_fault", path, j.string), tuple(configs))


# ---- the document ------------------------------------------------------------------------

def parse_replay(document: Any) -> ReplayDocument:
    root = check_envelope(document, REPLAY_SCHEMA)
    provenance = dict(j.req(root, "provenance", "$", j.obj))
    body = j.req(root, "result", "$", j.obj)

    run = j.req(body, "run", "$.result", j.obj)
    run_id = j.req(run, "run_id", "$.result.run", j.string)
    mode = j.req(run, "mode", "$.result.run", j.string)
    if mode != "signal_replay":
        raise LabUiError("unsupported_schema", f"$.result.run.mode: '{mode}' is not 'signal_replay'.")
    context = j.string_map(j.req(run, "context", "$.result.run", j.obj), "$.result.run.context")

    dataset = _dataset(body)
    max_rows, bus_fault, strategies = _configuration(body)

    warnings = []
    for i, raw in enumerate(j.req(body, "warnings", "$.result", j.arr)):
        wp = f"$.result.warnings[{i}]"
        w = j.obj(raw, wp)
        warnings.append(RunWarning(j.req(w, "code", wp, j.string), j.req(w, "message", wp, j.string),
                                   j.opt(w, "strategy_id", wp, j.string)))

    events = tuple(_event(e, f"$.result.events[{i}]", len(strategies))
                   for i, e in enumerate(j.req(body, "events", "$.result", j.arr)))
    signals = tuple(_signal(s, f"$.result.signals[{i}]", run_id, with_sequence=True)
                    for i, s in enumerate(j.req(body, "signals", "$.result", j.arr)))
    failures = []
    for i, raw in enumerate(j.req(body, "publication_failures", "$.result", j.arr)):
        fp = f"$.result.publication_failures[{i}]"
        f = j.obj(raw, fp)
        failures.append(PublicationFailure(_signal(f, fp, run_id, with_sequence=False),
                                           j.req(f, "kind", fp, j.string), j.req(f, "message", fp, j.string)))

    summary = dict(j.req(body, "summary", "$.result", j.obj))
    engine = dict(j.req(body, "engine", "$.result", j.obj))
    by_event = _check_links(events, signals, strategies, summary, dataset)

    return ReplayDocument(
        schema_version=root["schema_version"], provenance=provenance, run_id=run_id,
        notice=j.req(run, "notice", "$.result.run", j.string), context=context, dataset=dataset,
        max_rows=max_rows, bus_fault=bus_fault, strategies=strategies, warnings=tuple(warnings),
        events=events, signals=signals, publication_failures=tuple(failures), engine=engine,
        summary=summary, signals_by_event=by_event)


def _check_links(events: tuple[Event, ...], signals: tuple[Signal, ...],
                 strategies: tuple[StrategyConfig, ...], summary: Mapping[str, Any],
                 dataset: DatasetInfo) -> dict[int, tuple[Signal, ...]]:
    """Every join the replay uses, verified once. Raises invalid_structure on any disagreement."""
    slots = {c.strategy_id: slot for slot, c in enumerate(strategies)}
    for position, event in enumerate(events):
        if event.index != position:
            raise LabUiError("invalid_structure",
                             f"$.result.events[{position}].index: expected {position}, got {event.index}.")
        for slot, result in enumerate(event.results):
            if result.strategy_id != strategies[slot].strategy_id:
                raise LabUiError("invalid_structure",
                                 f"$.result.events[{position}].results[{slot}]: strategy '{result.strategy_id}' "
                                 f"does not match configuration '{strategies[slot].strategy_id}'.")
    if len(events) != dataset.rows:
        raise LabUiError("invalid_structure",
                         f"The result has {len(events)} events but the dataset reports {dataset.rows} rows.")

    seen_ids: set[tuple[str, int]] = set()
    by_event: dict[int, list[Signal]] = {}
    for position, signal in enumerate(signals):
        where = f"$.result.signals[{position}]"
        if signal.strategy_id not in slots:
            raise LabUiError("invalid_structure", f"{where}.strategy_id: unknown strategy '{signal.strategy_id}'.")
        if signal.event_index >= len(events):
            raise LabUiError("invalid_structure", f"{where}.event_index: {signal.event_index} is past the last event.")
        key = (signal.strategy_id, signal.signal_id)
        if key in seen_ids:
            raise LabUiError("invalid_structure", f"{where}: signal {signal.signal_id} appears twice.")
        seen_ids.add(key)
        event = events[signal.event_index]
        if event.symbol != signal.symbol:
            raise LabUiError("invalid_structure",
                             f"{where}.symbol: '{signal.symbol}' differs from the symbol of event {signal.event_index} "
                             f"('{event.symbol}').")
        if signal.signal_id not in event.results[slots[signal.strategy_id]].signal_ids:
            raise LabUiError("invalid_structure",
                             f"{where}: event {signal.event_index} does not list signal {signal.signal_id}.")
        by_event.setdefault(signal.event_index, []).append(signal)

    listed = 0
    for event in events:
        for result in event.results:
            for signal_id in result.signal_ids:
                listed += 1
                if (result.strategy_id, signal_id) not in seen_ids:
                    raise LabUiError("invalid_structure",
                                     f"Event {event.index} lists signal {signal_id}, which is not in the signals array.")
    if listed != len(signals):
        raise LabUiError("invalid_structure",
                         f"Events list {listed} signals but the signals array holds {len(signals)}.")
    for field_name, expected in (("events", len(events)), ("signals", len(signals))):
        reported = summary.get(field_name)
        if isinstance(reported, int) and not isinstance(reported, bool) and reported != expected:
            raise LabUiError("invalid_structure",
                             f"$.result.summary.{field_name} is {reported} but the document holds {expected}.")
    return {index: tuple(group) for index, group in by_event.items()}
