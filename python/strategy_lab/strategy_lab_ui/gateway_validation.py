"""The two documents the console's Advanced > Validation area needs, built from the Lab's own scenario and report modules.

Nothing here runs a process: `info_document` only reads the scenario files, and `include_demonstration` only checks a request body.
The scenario checks themselves are `validate.run_all`, called by the gateway on an explicit POST."""

from __future__ import annotations

from typing import Any

from . import scenarios, validate
from .errors import LabUiError


def info_document() -> dict[str, Any]:
    """The scenario files a validation would run: identity, and one entry per scenario."""
    scenario_set = scenarios.load_scenarios()
    directory = str(scenarios.SCENARIO_DIR.relative_to(scenarios.SCENARIO_DIR.parents[3])).replace("\\", "/")
    return {
        "scenario_set": {"directory": directory, "identity_sha256": scenario_set.identity, "files": list(scenario_set.files)},
        "scenarios": [{"id": s.id, "title": s.title, "purpose": s.purpose, "covers": list(s.covers), "outcome": s.outcome,
                       "dataset_path": s.dataset_path, "strategies": [{"kind": c.kind, "strategy_id": c.strategy_id} for c in s.strategies]}
                      for s in scenario_set.scenarios],
        "demonstration_available": scenario_set.by_id(scenarios.DEMO_BASE) is not None,
        "scope_note": validate.SCOPE_NOTE,
    }


def include_demonstration(body: Any) -> bool:
    """`{}` or `{"include_demonstration": true | false}`. Anything else is a bad request (nothing else may steer a validation)."""
    if body is None:
        return False
    if not isinstance(body, dict) or set(body) - {"include_demonstration"}:
        raise LabUiError("bad_request", "The body must be {} or {\"include_demonstration\": true | false}.")
    include = body.get("include_demonstration", False)
    if not isinstance(include, bool):
        error = LabUiError("bad_request", "include_demonstration must be true or false.")
        error.detail = "field: include_demonstration"
        raise error
    return include
