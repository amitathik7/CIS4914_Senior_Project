"""Strict JSON reading and small structural validators.

The runner promises exactly one JSON document and a newline on stdout (docs/STRATEGY_LAB.md
section 6). This reader holds it to that: valid UTF-8, no duplicate keys, none of the
non-standard literals NaN/Infinity, nothing after the document but the newline.

Integers stay Python integers (arbitrary precision) so identifiers and counters above 2**53
survive exactly; nothing here routes an identifier through a float.
"""

from __future__ import annotations

import json
import re
from typing import Any, Callable, Mapping

from .errors import LabUiError

_MAX_EXCERPT = 2000


def excerpt(data: bytes | str, limit: int = _MAX_EXCERPT) -> str:
    text = data.decode("utf-8", errors="replace") if isinstance(data, bytes) else data
    return text if len(text) <= limit else text[:limit] + f"... [{len(text) - limit} more characters]"


def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate object key {key!r}")
        out[key] = value
    return out


def _refuse_constant(name: str) -> Any:
    raise ValueError(f"non-standard JSON literal {name}")


def strict_loads(stdout: bytes) -> Any:
    """Parse the runner's stdout. Raises LabUiError(kind="malformed_output")."""
    if not stdout:
        raise LabUiError("malformed_output", "The replay tool wrote nothing on stdout.")
    try:
        text = stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise LabUiError("malformed_output", "The replay tool's output is not valid UTF-8.",
                         detail=str(error), stdout_excerpt=excerpt(stdout)) from error
    decoder = json.JSONDecoder(object_pairs_hook=_no_duplicates, parse_constant=_refuse_constant)
    try:
        value, end = decoder.raw_decode(text)
    except ValueError as error:
        raise LabUiError("malformed_output", "The replay tool's output is not a valid JSON document.",
                         detail=str(error), stdout_excerpt=excerpt(text)) from error
    if text[end:] != "\n":
        raise LabUiError("malformed_output",
                         "Expected exactly one JSON document followed by one newline on stdout.",
                         detail=f"trailing data after the document: {excerpt(text[end:], 200)!r}",
                         stdout_excerpt=excerpt(text))
    return value


def strict_loads_file(data: bytes, *, what: str) -> Any:
    """A hand-written JSON file (UTF-8, optional BOM): the same strictness as the runner's output (no duplicate keys, no
    NaN/Infinity), but any trailing whitespace is fine. Raises LabUiError(kind="scenario_invalid")."""
    try:
        text = data.decode("utf-8-sig")
        value, end = json.JSONDecoder(object_pairs_hook=_no_duplicates, parse_constant=_refuse_constant).raw_decode(text)
    except (UnicodeDecodeError, ValueError) as error:
        raise LabUiError("scenario_invalid", f"{what} is not a valid JSON document: {error}") from error
    if text[end:].strip():
        raise LabUiError("scenario_invalid", f"{what} has text after the JSON document.")
    return value


# ---- structural validators -----------------------------------------------------------

def bad(path: str, expected: str, got: Any) -> LabUiError:
    return LabUiError("invalid_structure", f"{path}: expected {expected}, got {type(got).__name__}.",
                      detail=f"{path} = {excerpt(repr(got), 200)}")


def obj(value: Any, path: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise bad(path, "an object", value)
    return value


def only_keys(container: Mapping[str, Any], allowed: tuple[str, ...], path: str) -> None:
    """Refuse an unknown member, so a misspelt key cannot silently switch an assertion off."""
    extra = sorted(set(container) - set(allowed))
    if extra:
        raise LabUiError("invalid_structure", f"{path}: unknown member(s) {', '.join(extra)}; allowed: {', '.join(allowed)}.")


def arr(value: Any, path: str) -> list[Any]:
    if not isinstance(value, list):
        raise bad(path, "an array", value)
    return value


def need(container: Mapping[str, Any], key: str, path: str) -> Any:
    if key not in container:
        raise LabUiError("invalid_structure", f"{path}: required member '{key}' is missing.")
    return container[key]


def string(value: Any, path: str) -> str:
    if not isinstance(value, str):
        raise bad(path, "a string", value)
    return value


def integer(value: Any, path: str, *, minimum: int | None = None) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise bad(path, "an integer", value)
    if minimum is not None and value < minimum:
        raise LabUiError("invalid_structure", f"{path}: expected an integer of at least {minimum}, got {value}.")
    return value


def number(value: Any, path: str) -> int | float:
    """A JSON number, kept as parsed (int stays int). Booleans are not numbers."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise bad(path, "a number", value)
    return value


def boolean(value: Any, path: str) -> bool:
    if not isinstance(value, bool):
        raise bad(path, "a boolean", value)
    return value


def req(container: Mapping[str, Any], key: str, path: str, check: Callable[[Any, str], Any]) -> Any:
    return check(need(container, key, path), f"{path}.{key}")


def opt(container: Mapping[str, Any], key: str, path: str, check: Callable[[Any, str], Any]) -> Any:
    return check(container[key], f"{path}.{key}") if key in container else None


def string_map(value: Any, path: str) -> dict[str, str]:
    return {k: string(v, f"{path}.{k}") for k, v in obj(value, path).items()}


_VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")


def schema_version(value: Any, path: str, supported_major: int) -> str:
    text = string(value, path)
    match = _VERSION.match(text)
    if match is None:
        raise LabUiError("unsupported_schema", f"{path}: '{text}' is not a MAJOR.MINOR version.")
    if int(match.group(1)) != supported_major:
        raise LabUiError(
            "unsupported_schema",
            f"The output uses schema version {text}; this UI understands major version {supported_major}. "
            "Rebuild strategy_lab_replay and the UI from the same checkout.",
            detail=f"{path} = {text!r}")
    return text
