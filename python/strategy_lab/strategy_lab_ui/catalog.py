"""The catalog (`describe`) and error documents of strategy_lab_replay.

The catalog is the authority for strategy names, parameters, defaults and the plain-language
text of every decision reason. The UI builds its form from it and keeps no copy.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Mapping

from . import SUPPORTED_SCHEMA_MAJOR
from . import jsonio as j
from .errors import LabUiError, Problem

CATALOG_SCHEMA = "strategy_lab.catalog"
REPLAY_SCHEMA = "strategy_lab.replay"
ERROR_SCHEMA = "strategy_lab.error"

_PARAM_TYPES = ("string", "uint", "double", "string_list")


@dataclass(frozen=True)
class ParamSpec:
    name: str
    type: str                      # string | uint | double | string_list
    required: bool
    constraint: str
    default: Any = None            # as parsed from JSON (int stays int); None when there is no default
    has_default: bool = False


@dataclass(frozen=True)
class ReasonSpec:
    code: str
    verdict: str
    text: str


@dataclass(frozen=True)
class StrategySpec:
    kind: str
    title: str
    summary: str
    docs: str
    available: bool
    unavailable_reason: str
    params: tuple[ParamSpec, ...]
    reasons: tuple[ReasonSpec, ...]
    indicators: tuple[str, ...]
    states: tuple[str, ...]
    signal_metadata_keys: tuple[str, ...]

    def param(self, name: str) -> ParamSpec | None:
        return next((p for p in self.params if p.name == name), None)

    def reason_text(self, code: str) -> str | None:
        return next((r.text for r in self.reasons if r.code == code), None)


@dataclass(frozen=True)
class Catalog:
    tool: str
    project_version: str
    schema_version: str
    default_max_rows: int
    hard_max_rows: int
    dataset_format: str
    strategies: tuple[StrategySpec, ...]
    bus_faults: tuple[str, ...] = field(default_factory=tuple)

    def strategy(self, kind: str) -> StrategySpec | None:
        return next((s for s in self.strategies if s.kind == kind), None)

    @property
    def available(self) -> tuple[StrategySpec, ...]:
        return tuple(s for s in self.strategies if s.available)


@dataclass(frozen=True)
class ErrorDocument:
    code: str
    exit_status: int
    message: str
    total_problems: int
    problems: tuple[Problem, ...]


def check_envelope(document: Any, expected_schema: str) -> Mapping[str, Any]:
    """Top-level object, expected schema name, supported major version."""
    root = j.obj(document, "$")
    name = j.req(root, "schema", "$", j.string)
    if name != expected_schema:
        raise LabUiError("malformed_output", f"Expected a '{expected_schema}' document but got '{name}'.")
    j.req(root, "schema_version", "$", lambda v, p: j.schema_version(v, p, SUPPORTED_SCHEMA_MAJOR))
    return root


def _param(value: Any, path: str) -> ParamSpec:
    item = j.obj(value, path)
    kind = j.req(item, "type", path, j.string)
    if kind not in _PARAM_TYPES:
        raise LabUiError("unsupported_schema", f"{path}.type: unknown parameter type '{kind}'.")
    has_default = "default" in item
    return ParamSpec(
        name=j.req(item, "name", path, j.string),
        type=kind,
        required=j.req(item, "required", path, j.boolean),
        constraint=j.req(item, "constraint", path, j.string),
        default=item["default"] if has_default else None,
        has_default=has_default,
    )


def _strategy(value: Any, path: str) -> StrategySpec:
    item = j.obj(value, path)
    available = j.req(item, "available", path, j.boolean)

    def strings(key: str) -> tuple[str, ...]:
        return tuple(j.string(v, f"{path}.{key}[{i}]") for i, v in enumerate(j.req(item, key, path, j.arr)))

    reasons: list[ReasonSpec] = []
    for i, raw in enumerate(j.req(item, "decision_reasons", path, j.arr)):
        rpath = f"{path}.decision_reasons[{i}]"
        entry = j.obj(raw, rpath)
        reasons.append(ReasonSpec(j.req(entry, "code", rpath, j.string),
                                  j.req(entry, "verdict", rpath, j.string),
                                  j.req(entry, "text", rpath, j.string)))
    return StrategySpec(
        kind=j.req(item, "kind", path, j.string),
        title=j.req(item, "title", path, j.string),
        summary=j.req(item, "summary", path, j.string),
        docs=j.req(item, "docs", path, j.string),
        available=available,
        unavailable_reason=j.opt(item, "unavailable_reason", path, j.string) or "",
        params=tuple(_param(p, f"{path}.parameters[{i}]")
                     for i, p in enumerate(j.req(item, "parameters", path, j.arr))),
        reasons=tuple(reasons),
        indicators=strings("indicators"),
        states=strings("states"),
        signal_metadata_keys=strings("signal_metadata_keys"),
    )


def parse_catalog(document: Any) -> Catalog:
    root = check_envelope(document, CATALOG_SCHEMA)
    body = j.req(root, "catalog", "$", j.obj)
    limits = j.req(body, "limits", "$.catalog", j.obj)
    fmt = j.req(body, "dataset_format", "$.catalog", j.obj)
    strategies = tuple(_strategy(s, f"$.catalog.strategies[{i}]")
                       for i, s in enumerate(j.req(body, "strategies", "$.catalog", j.arr)))
    kinds = [s.kind for s in strategies]
    if len(set(kinds)) != len(kinds):
        raise LabUiError("invalid_structure", "$.catalog.strategies: a strategy kind is listed twice.")
    faults: list[str] = []
    for i, raw in enumerate(j.req(body, "bus_faults", "$.catalog", j.arr)):
        faults.append(j.req(j.obj(raw, f"$.catalog.bus_faults[{i}]"), "name", f"$.catalog.bus_faults[{i}]", j.string))
    return Catalog(
        tool=j.req(body, "tool", "$.catalog", j.string),
        project_version=j.req(body, "project_version", "$.catalog", j.string),
        schema_version=root["schema_version"],
        default_max_rows=j.req(limits, "default_max_rows", "$.catalog.limits", j.integer),
        hard_max_rows=j.req(limits, "hard_max_rows", "$.catalog.limits", j.integer),
        dataset_format=j.req(fmt, "id", "$.catalog.dataset_format", j.string),
        strategies=strategies,
        bus_faults=tuple(faults),
    )


def parse_error_document(document: Any) -> ErrorDocument:
    root = check_envelope(document, ERROR_SCHEMA)
    body = j.req(root, "error", "$", j.obj)
    problems: list[Problem] = []
    for i, raw in enumerate(j.req(body, "problems", "$.error", j.arr)):
        path = f"$.error.problems[{i}]"
        item = j.obj(raw, path)
        problems.append(Problem(
            message=j.req(item, "message", path, j.string),
            line=j.opt(item, "line", path, j.integer),
            column=j.opt(item, "column", path, j.string),
            where=j.opt(item, "where", path, j.string),
        ))
    return ErrorDocument(
        code=j.req(body, "code", "$.error", j.string),
        exit_status=j.req(body, "exit_status", "$.error", j.integer),
        message=j.req(body, "message", "$.error", j.string),
        total_problems=j.req(body, "total_problems", "$.error", lambda v, p: j.integer(v, p, minimum=0)),
        problems=tuple(problems),
    )
