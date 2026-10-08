"""Shared helpers for the Strategy Lab UI tests (stdlib unittest; run from python/strategy_lab).

`make_document` builds a schema-1.0 replay document as a plain dict, so schema, replay, chart and export
logic can be tested on exact, hand-written inputs (equal timestamps across symbols, ids beyond 2**53, two strategies
signalling on one event) without the executable. `real_runner` finds the built executable or skips.
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from typing import Any, Sequence

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from strategy_lab_ui import bridge  # noqa: E402  (after the path tweak)
from strategy_lab_ui.errors import LabUiError  # noqa: E402

FIXTURES = ROOT.parents[1] / "tests" / "fixtures" / "strategy_lab" / "datasets"
T0 = "2026-01-05T14:{m:02d}:00.000000000Z"
RUN_ID = "0123456789abcdef"


def real_runner() -> bridge.Runner:
    try:
        return bridge.find_runner()
    except LabUiError as error:
        raise unittest.SkipTest(f"strategy_lab_replay is not built ({error.message})") from error


def ev(symbol: str, minute: int, price: float | int | None = 1, *, type: str = "bar", verdict: str = "evaluated",
       reason: str = "same_side", signals: Sequence[tuple[int, int, str]] = (), indicators: dict[str, float] | None = None,
       time: str | None = None, unavailable: dict[str, str] | None = None,
       window: dict[str, int] | None = None) -> dict[str, Any]:
    """One event spec. `signals` are (strategy slot, signal id, side). `window` is the recorded window (default: full, 3 of 3)."""
    return {"symbol": symbol, "time": time or T0.format(m=minute), "price": price, "type": type, "verdict": verdict,
            "reason": reason, "signals": list(signals), "indicators": indicators or {}, "unavailable": unavailable or {},
            "window": window}


def make_document(specs: Sequence[dict[str, Any]], *, strategies: Sequence[str] = ("s1",), kind: str = "sma_crossover",
                  parameters: dict[str, Any] | None = None, run_id: str = RUN_ID, dataset_sha: str = "ab" * 32,
                  schema_version: str = "1.0", symbols: Sequence[str] | None = None) -> dict[str, Any]:
    events, signals = [], []
    for index, spec in enumerate(specs):
        results = []
        for slot, strategy_id in enumerate(strategies):
            mine = [(sid, side) for s, sid, side in spec["signals"] if s == slot]
            action = mine[0][1] if mine else "none"
            result: dict[str, Any] = {
                "strategy_id": strategy_id, "diagnostics_available": True, "verdict": spec["verdict"],
                "reason": spec["reason"], "action": action, "window": spec.get("window") or {"fill": 3, "remaining": 0, "size": 3},
                "indicators": dict(spec["indicators"]), "unavailable": dict(spec["unavailable"]),
                "states": {"relation_before": "none"}, "signal_ids": [sid for sid, _ in mine]}
            results.append(result)
            for sid, side in mine:
                signals.append({"signal_id": sid, "signal_ref": f"{run_id}:{sid}", "strategy_id": strategy_id,
                                "event_index": index, "symbol": spec["symbol"], "side": side, "order_type": "market",
                                "requested_quantity": 1, "created_at": spec["time"], "bus_sequence": 2 * index + 2,
                                "metadata": {"trigger": f"test_{side}"}})
        event: dict[str, Any] = {"index": index, "source_line": index + 2, "symbol": spec["symbol"],
                                 "exchange_time": spec["time"], "type": spec["type"], "bus_sequence": 2 * index + 1,
                                 "results": results}
        if spec["price"] is not None:
            event["price"] = spec["price"]
        events.append(event)
    seen = list(dict.fromkeys(spec["symbol"] for spec in specs)) if symbols is None else list(symbols)
    return {
        "schema": "strategy_lab.replay", "schema_version": schema_version,
        "provenance": {"tool": "strategy_lab_replay", "project_version": "0.0.1", "compiler": "test", "build": "debug",
                       "result_sha256": "00" * 32},
        "result": {
            "run": {"run_id": run_id, "mode": "signal_replay", "notice": "test notice", "signal_id_scope": "x",
                    "context": {"engine": "e", "event_bus": "b", "clock": "c", "replay_order": "file order",
                                "bus_fault": "none"}},
            "input": {"dataset": {"name": "dataset.csv", "format": "strategy_lab_bars_csv/1", "sha256": dataset_sha,
                                  "bytes": 100, "rows": len(specs), "first_exchange_time": specs[0]["time"],
                                  "last_exchange_time": specs[-1]["time"],
                                  "symbols": [{"symbol": s, "bar_rows": 1, "trade_rows": 0,
                                               "first_exchange_time": specs[0]["time"],
                                               "last_exchange_time": specs[-1]["time"]} for s in seen]}},
            "configuration": {"max_rows": 20000, "bus_fault": "none", "strategies": [
                {"kind": kind, "strategy_id": sid, "window_size": 3,
                 "parameters": parameters or {"short_window": 1, "long_window": 3}, "derived": {}}
                for sid in strategies]},
            "warnings": [], "events": events, "signals": signals, "publication_failures": [],
            "engine": {"stats": {"events_routed": len(specs)}, "strategies": [], "bus": {}},
            "summary": {"events": len(specs), "bars": len(specs), "signals": len(signals), "strategies": []}}}


def dumps(document: dict[str, Any]) -> bytes:
    """Exactly what the runner writes: compact JSON and one newline."""
    return (json.dumps(document, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def parse(document: dict[str, Any]):
    from strategy_lab_ui.jsonio import strict_loads
    from strategy_lab_ui.schema import parse_replay
    return parse_replay(strict_loads(dumps(document)))


def outcome_of(document: dict[str, Any]) -> "bridge.RunOutcome":
    """What bridge.run_replay would return for this hand-built document (exact bytes kept, as the runner wrote them)."""
    raw = dumps(document)
    process = bridge.ProcessResult(("fake",), "", 0, raw, "", False, 0.0)
    return bridge.RunOutcome(process, parse(document))


APP = str(ROOT / "app.py")
VALID_CSV = (b"symbol,exchange_time,type,price\nXYZ,2026-03-02T14:30:00Z,bar,50\nXYZ,2026-03-02T14:31:00Z,bar,50\n"
             b"XYZ,2026-03-02T14:32:00Z,bar,50\nXYZ,2026-03-02T14:33:00Z,bar,40\nXYZ,2026-03-02T14:34:00Z,bar,60\n")


def page_text(at) -> str:
    """Every piece of visible text a script run produced (markdown, captions, banners, code and the cells of every table)."""
    parts: list[str] = []
    for kind in ("markdown", "caption", "warning", "error", "info", "success", "text", "code"):
        parts += [str(e.value) for e in getattr(at, kind)]
    for table in at.get("table"):
        parts.append(table.value.to_string() if hasattr(table, "value") else "")
    return "\n".join(parts)


def chart_spec(at) -> dict:
    """The figure the app sent to the browser (the Plotly JSON): what is actually drawn. Each view draws one chart."""
    return json.loads(at.get("plotly_chart")[0].proto.spec)


def open_view(at, view: str) -> None:
    at.radio(key="ui_view").set_value(view).run()
    assert not at.exception, [e.value for e in at.exception]


class ProcessCounter:
    """Counts every process the bridge starts. `describe` is the catalog (once per executable build); `run` is a replay.

    Use as a context manager around AppTest runs: it is how the tests prove an interaction did or did not start the engine,
    whichever view started it."""

    def __init__(self) -> None:
        from unittest import mock
        self.calls: list[str] = []
        original = bridge.run_process

        def counting(runner, args, **kwargs):
            self.calls.append(args[0])
            return original(runner, args, **kwargs)

        self._patch = mock.patch.object(bridge, "run_process", counting)

    def __enter__(self) -> "ProcessCounter":
        self._patch.start()
        return self

    def __exit__(self, *exc) -> None:
        self._patch.stop()

    @property
    def runs(self) -> int:
        return self.calls.count("run")
