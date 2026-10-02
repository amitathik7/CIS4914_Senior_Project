"""The checked-in validation scenarios (`tests/fixtures/strategy_lab/scenarios/*.json`): loading and sanity-checking only.

A scenario names a dataset (pinned by the SHA-256 of its LF-normalized bytes, so a CRLF checkout still matches), one or more
strategy configurations, and the EXPECTED outcome. Expected values were derived by hand from the strategy documents; each file
says how (`provenance`). Nothing here, and nothing in `validate.py`, ever writes an expected value from a run of the tool.

Numbers are kept exact: an expected value is a `Fraction` parsed from text ("5/2", "-1.7320508075688773"). `exact` must be a
value a double represents exactly; anything else needs `approx` and a NAMED tolerance declared in the same file with a written
justification. Unknown members are refused, so a misspelt key cannot quietly switch an assertion off.
"""

from __future__ import annotations

import copy
import hashlib
import re
from dataclasses import dataclass, field, replace
from fractions import Fraction
from pathlib import Path
from typing import Any, Mapping

from . import jsonio as j
from .datasets import FIXTURE_DIR
from .errors import LabUiError

SCHEMA = "strategy_lab.scenario/1"
FIXTURE_ROOT = FIXTURE_DIR.parent                     # tests/fixtures/strategy_lab
SCENARIO_DIR = FIXTURE_ROOT / "scenarios"
DEMO_ID = "demo_injected_mismatch"
DEMO_BASE = "sma_crossover_timing"

_ID = re.compile(r"^[a-z][a-z0-9_]{1,63}$")
_HEX = re.compile(r"^[0-9a-f]{64}$")


@dataclass(frozen=True)
class Tolerance:
    name: str
    abs_tol: Fraction
    why: str


@dataclass(frozen=True)
class NumberExpect:
    value: Fraction
    tolerance: Tolerance | None = None            # None: the double must equal `value` exactly


@dataclass(frozen=True)
class StrategyConf:
    kind: str
    params: tuple[tuple[str, str], ...]

    @property
    def strategy_id(self) -> str:
        return dict(self.params).get("strategy_id", self.kind)


@dataclass(frozen=True)
class ExpectedSignal:
    strategy: str
    event_index: int
    symbol: str
    side: str
    signal_id: int | None = None


@dataclass(frozen=True)
class EventExpect:
    strategy: str
    first: int
    last: int                                     # inclusive; equal to `first` for a single event
    symbol: str | None = None
    exchange_time: str | None = None
    type: str | None = None
    verdict: str | None = None
    reason: str | None = None
    action: str | None = None
    window_fill: int | None = None
    window_size: int | None = None
    indicators: tuple[tuple[str, NumberExpect], ...] = ()
    unavailable: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True)
class ExpectedProblem:
    line: int | None = None
    column: str | None = None
    where: str | None = None


@dataclass(frozen=True)
class ExpectedRejection:
    exit_status: int
    error_code: str
    message_contains: tuple[str, ...] = ()
    problems: tuple[ExpectedProblem, ...] = ()
    total_problems: int | None = None


@dataclass(frozen=True)
class Provenance:
    derived_by: str
    sources: tuple[str, ...]
    derivation: tuple[str, ...]
    cross_checks: tuple[str, ...]


@dataclass(frozen=True, eq=False)
class Scenario:
    id: str
    title: str
    purpose: str
    covers: tuple[str, ...]
    dataset_path: str
    dataset_sha256_lf: str
    strategies: tuple[StrategyConf, ...]
    provenance: Provenance
    tolerances: Mapping[str, Tolerance]
    outcome: str                                  # "result" | "rejected"
    warnings: tuple[str, ...] | None
    signals: tuple[ExpectedSignal, ...] | None
    events: tuple[EventExpect, ...]
    rejection: ExpectedRejection | None
    source_file: str
    raw: Mapping[str, Any] = field(repr=False, default_factory=dict)        # the file as written, for the report
    demonstration: bool = False


@dataclass(frozen=True, eq=False)
class ScenarioSet:
    scenarios: tuple[Scenario, ...]
    root: Path
    identity: str                                 # SHA-256 over the file names and LF-normalized bytes
    files: tuple[str, ...]

    def by_id(self, scenario_id: str) -> Scenario | None:
        return next((s for s in self.scenarios if s.id == scenario_id), None)


# ---- parsing -------------------------------------------------------------------------------------------------

def _text(value: Any, path: str) -> str:
    text = j.string(value, path)
    if not text.strip():
        raise LabUiError("invalid_structure", f"{path}: must not be empty.")
    return text


def _strings(value: Any, path: str) -> tuple[str, ...]:
    return tuple(_text(v, f"{path}[{i}]") for i, v in enumerate(j.arr(value, path)))


def _fraction(value: Any, path: str) -> Fraction:
    try:
        return Fraction(j.string(value, path))
    except (ValueError, ZeroDivisionError) as error:
        raise LabUiError("invalid_structure", f"{path}: '{value}' is not a number or fraction.") from error


def _tolerances(raw: Any, path: str) -> dict[str, Tolerance]:
    out: dict[str, Tolerance] = {}
    for name, body in j.obj(raw, path).items():
        where = f"{path}.{name}"
        item = j.obj(body, where)
        j.only_keys(item, ("abs_tol", "why"), where)
        tol = _fraction(j.need(item, "abs_tol", where), f"{where}.abs_tol")
        if tol <= 0:
            raise LabUiError("invalid_structure", f"{where}.abs_tol: must be greater than zero.")
        out[name] = Tolerance(name, tol, _text(j.need(item, "why", where), f"{where}.why"))
    return out


def _number(raw: Any, path: str, tolerances: Mapping[str, Tolerance]) -> NumberExpect:
    item = j.obj(raw, path)
    if "exact" in item:
        j.only_keys(item, ("exact",), path)
        value = _fraction(item["exact"], f"{path}.exact")
        if Fraction(float(value)) != value:
            raise LabUiError("invalid_structure", f"{path}.exact: {value} is not exactly representable as a double; "
                                                  "give an 'approx' value with a named tolerance instead.")
        return NumberExpect(value)
    j.only_keys(item, ("approx", "tolerance"), path)
    name = j.string(j.need(item, "tolerance", path), f"{path}.tolerance")
    if name not in tolerances:
        raise LabUiError("invalid_structure", f"{path}.tolerance: '{name}' is not declared in this file's tolerances.")
    return NumberExpect(_fraction(j.need(item, "approx", path), f"{path}.approx"), tolerances[name])


_EVENT_KEYS = ("strategy", "event_index", "event_range", "symbol", "exchange_time", "type", "verdict", "reason", "action",
               "window_fill", "window_size", "indicators", "unavailable")


def _event(raw: Any, path: str, ids: tuple[str, ...], tolerances: Mapping[str, Tolerance]) -> EventExpect:
    item = j.obj(raw, path)
    j.only_keys(item, _EVENT_KEYS, path)
    strategy = j.req(item, "strategy", path, j.string)
    if strategy not in ids:
        raise LabUiError("invalid_structure", f"{path}.strategy: '{strategy}' is not one of this scenario's strategy ids {list(ids)}.")
    if ("event_index" in item) == ("event_range" in item):
        raise LabUiError("invalid_structure", f"{path}: give exactly one of event_index and event_range.")
    if "event_index" in item:
        first = last = j.req(item, "event_index", path, lambda v, p: j.integer(v, p, minimum=0))
    else:
        pair = j.req(item, "event_range", path, j.arr)
        if len(pair) != 2:
            raise LabUiError("invalid_structure", f"{path}.event_range: expected [first, last].")
        first, last = (j.integer(v, f"{path}.event_range[{i}]", minimum=0) for i, v in enumerate(pair))
        if last < first:
            raise LabUiError("invalid_structure", f"{path}.event_range: last is before first.")
    text = lambda key: j.opt(item, key, path, j.string)                                       # noqa: E731
    number = lambda key: j.opt(item, key, path, lambda v, p: j.integer(v, p, minimum=0))      # noqa: E731
    indicators = tuple((name, _number(body, f"{path}.indicators.{name}", tolerances))
                       for name, body in j.obj(item.get("indicators", {}), f"{path}.indicators").items())
    unavailable = tuple(j.string_map(item.get("unavailable", {}), f"{path}.unavailable").items())
    return EventExpect(strategy, first, last, text("symbol"), text("exchange_time"), text("type"), text("verdict"),
                       text("reason"), text("action"), number("window_fill"), number("window_size"), indicators, unavailable)


def _signal(raw: Any, path: str, ids: tuple[str, ...]) -> ExpectedSignal:
    item = j.obj(raw, path)
    j.only_keys(item, ("strategy", "event_index", "symbol", "side", "signal_id"), path)
    strategy = j.req(item, "strategy", path, j.string)
    if strategy not in ids:
        raise LabUiError("invalid_structure", f"{path}.strategy: '{strategy}' is not one of this scenario's strategy ids {list(ids)}.")
    side = j.req(item, "side", path, j.string)
    if side not in ("buy", "sell"):
        raise LabUiError("invalid_structure", f"{path}.side: '{side}' is not buy or sell.")
    return ExpectedSignal(strategy, j.req(item, "event_index", path, lambda v, p: j.integer(v, p, minimum=0)),
                          j.req(item, "symbol", path, j.string), side,
                          j.opt(item, "signal_id", path, lambda v, p: j.integer(v, p, minimum=1)))


def _rejection(raw: Any, path: str) -> ExpectedRejection:
    item = j.obj(raw, path)
    j.only_keys(item, ("outcome", "exit_status", "error_code", "message_contains", "problems", "total_problems"), path)
    status = j.req(item, "exit_status", path, j.integer)
    if status not in (2, 3):
        raise LabUiError("invalid_structure", f"{path}.exit_status: a rejection is exit status 2 or 3, not {status}.")
    problems = []
    for i, entry in enumerate(j.arr(item.get("problems", []), f"{path}.problems")):
        where = f"{path}.problems[{i}]"
        body = j.obj(entry, where)
        j.only_keys(body, ("line", "column", "where"), where)
        problems.append(ExpectedProblem(j.opt(body, "line", where, lambda v, p: j.integer(v, p, minimum=1)),
                                        j.opt(body, "column", where, j.string), j.opt(body, "where", where, j.string)))
    return ExpectedRejection(status, _text(j.need(item, "error_code", path), f"{path}.error_code"),
                             _strings(item.get("message_contains", []), f"{path}.message_contains"), tuple(problems),
                             j.opt(item, "total_problems", path, lambda v, p: j.integer(v, p, minimum=0)))


def parse_scenario(raw: Any, source_file: str) -> Scenario:
    root = j.obj(raw, "$")
    j.only_keys(root, ("schema", "id", "title", "purpose", "covers", "dataset", "strategies", "provenance", "tolerances", "expect"), "$")
    if j.req(root, "schema", "$", j.string) != SCHEMA:
        raise LabUiError("scenario_invalid", f"{source_file}: schema must be '{SCHEMA}'.")
    sid = j.req(root, "id", "$", j.string)
    if not _ID.match(sid):
        raise LabUiError("invalid_structure", f"$.id: '{sid}' is not a lower-case identifier.")
    dataset = j.req(root, "dataset", "$", j.obj)
    j.only_keys(dataset, ("path", "sha256_lf"), "$.dataset")
    path = j.req(dataset, "path", "$.dataset", j.string)
    if path.startswith("/") or "\\" in path or ".." in path.split("/") or path.split("/")[0] not in ("datasets", "invalid"):
        raise LabUiError("invalid_structure", f"$.dataset.path: '{path}' must be a relative path under datasets/ or invalid/.")
    digest = j.req(dataset, "sha256_lf", "$.dataset", j.string)
    if not _HEX.match(digest):
        raise LabUiError("invalid_structure", "$.dataset.sha256_lf: expected 64 lower-case hex digits.")

    strategies = []
    for i, entry in enumerate(j.req(root, "strategies", "$", j.arr)):
        where = f"$.strategies[{i}]"
        body = j.obj(entry, where)
        j.only_keys(body, ("kind", "params"), where)
        strategies.append(StrategyConf(j.req(body, "kind", where, j.string),
                                       tuple(j.string_map(j.req(body, "params", where, j.obj), f"{where}.params").items())))
    if not strategies:
        raise LabUiError("invalid_structure", "$.strategies: at least one strategy is required.")
    ids = tuple(s.strategy_id for s in strategies)
    if len(set(ids)) != len(ids):
        raise LabUiError("invalid_structure", "$.strategies: strategy ids must be unique within a scenario.")

    prov = j.req(root, "provenance", "$", j.obj)
    j.only_keys(prov, ("derived_by", "sources", "derivation", "cross_checks"), "$.provenance")
    provenance = Provenance(_text(j.need(prov, "derived_by", "$.provenance"), "$.provenance.derived_by"),
                            _strings(j.need(prov, "sources", "$.provenance"), "$.provenance.sources"),
                            _strings(j.need(prov, "derivation", "$.provenance"), "$.provenance.derivation"),
                            _strings(j.need(prov, "cross_checks", "$.provenance"), "$.provenance.cross_checks"))
    if not provenance.derivation:
        raise LabUiError("invalid_structure", "$.provenance.derivation: an expectation must say how it was derived.")

    tolerances = _tolerances(root.get("tolerances", {}), "$.tolerances")
    expect = j.req(root, "expect", "$", j.obj)
    outcome = j.req(expect, "outcome", "$.expect", j.string)
    if outcome == "result":
        j.only_keys(expect, ("outcome", "warnings", "signals", "events"), "$.expect")
        warnings = _strings(j.need(expect, "warnings", "$.expect"), "$.expect.warnings")
        signals = tuple(_signal(s, f"$.expect.signals[{i}]", ids)
                        for i, s in enumerate(j.req(expect, "signals", "$.expect", j.arr)))
        events = tuple(_event(e, f"$.expect.events[{i}]", ids, tolerances)
                       for i, e in enumerate(j.arr(expect.get("events", []), "$.expect.events")))
        rejection = None
    elif outcome == "rejected":
        warnings, signals, events, rejection = None, None, (), _rejection(expect, "$.expect")
    else:
        raise LabUiError("invalid_structure", f"$.expect.outcome: '{outcome}' is not result or rejected.")
    return Scenario(sid, _text(j.need(root, "title", "$"), "$.title"), _text(j.need(root, "purpose", "$"), "$.purpose"),
                    _strings(j.need(root, "covers", "$"), "$.covers"), path, digest, tuple(strategies), provenance,
                    tolerances, outcome, warnings, signals, events, rejection, source_file, raw=root)


def load_scenarios(directory: Path | None = None, root: Path | None = None) -> ScenarioSet:
    """Read and check every *.json in the scenario directory (sorted by name). Raises LabUiError(scenario_invalid)."""
    directory = directory or SCENARIO_DIR
    root = root or FIXTURE_ROOT
    files = sorted(directory.glob("*.json")) if directory.is_dir() else []
    if not files:
        raise LabUiError("scenario_invalid", f"No scenario files (*.json) were found in {directory}.")
    scenarios: list[Scenario] = []
    digest = hashlib.sha256()
    for path in files:
        data = path.read_bytes()
        digest.update(path.name.encode("utf-8") + b"\0" + data.replace(b"\r\n", b"\n") + b"\0")
        try:
            scenarios.append(parse_scenario(j.strict_loads_file(data, what=path.name), path.name))
        except LabUiError as error:
            if error.kind == "scenario_invalid" and error.message.startswith(path.name):
                raise
            raise LabUiError("scenario_invalid", f"{path.name}: {error.message}", detail=error.detail) from error
    ids = [s.id for s in scenarios]
    for duplicate in {i for i in ids if ids.count(i) > 1}:
        raise LabUiError("scenario_invalid", f"The scenario id '{duplicate}' appears in more than one file.")
    return ScenarioSet(tuple(scenarios), root, digest.hexdigest(), tuple(p.name for p in files))


# ---- the opt-in demonstration of a mismatch -------------------------------------------------------------------

def injected_mismatch(base: Scenario) -> Scenario:
    """A copy of `base` with two deliberately WRONG expectations (the first signal one event late, one average off by 1/2).

    It exists so a mismatch can be shown on purpose. It is built in memory, never stored as a file, never part of a normal
    run, and must FAIL: a pass would mean the comparison itself is broken (see validate.py)."""
    if not base.signals or base.outcome != "result":
        raise ValueError("a mismatch can only be injected into a scenario that expects signals")
    first = replace(base.signals[0], event_index=base.signals[0].event_index + 1)
    events = list(base.events)
    raw = copy.deepcopy(dict(base.raw))                      # the report shows the wrong expectation as it was used
    raw["expect"]["signals"][0]["event_index"] += 1
    for position, expectation in enumerate(events):
        wanted = dict(expectation.indicators).get("short_sma")
        if wanted is not None and wanted.tolerance is None:
            shifted = tuple((n, NumberExpect(v.value + Fraction(1, 2), v.tolerance)) if n == "short_sma" else (n, v)
                            for n, v in expectation.indicators)
            events[position] = replace(expectation, indicators=shifted)
            raw["expect"]["events"][position]["indicators"]["short_sma"] = {"exact": str(wanted.value + Fraction(1, 2))}
            break
    return replace(base, id=DEMO_ID, title=f"DEMONSTRATION: {base.title} - with two deliberately wrong expectations",
                   purpose="Shows what a mismatch looks like: the first Buy is expected one event too late and one "
                           "moving average half a unit too high. This check is EXPECTED to fail.",
                   covers=("demonstration",), signals=(first, *base.signals[1:]), events=tuple(events), raw=raw,
                   source_file=f"(built in memory from {base.source_file})", demonstration=True)
