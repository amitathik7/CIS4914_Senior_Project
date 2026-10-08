"""Compare downloads. Every export says whether it covers the FULL RUN or the VISIBLE REPLAY PREFIX.

* Both runner results are exported as the tool wrote them, byte for byte (so each `provenance.result_sha256` still verifies).
* Every signal row carries its member, the member's run id, the RAW signal id and the run-scoped `signal_ref`, plus a combined
  `member_signal_key` ("A/<run_id>:<id>"): ids repeat across the two independent engine runs, so an id alone never joins.
* Counts are counts of recorded requests. No column ranks the configurations.
"""

from __future__ import annotations

import csv
import io
import json
import zipfile
from typing import Any, Sequence

from . import exports
from .compare import CompareModel, CompareSignal, CompletedComparison

SIGNAL_COLUMNS = ("export_scope", "member", "member_label", "strategy_kind", "strategy_id", "run_id", "signal_id",
                  "signal_ref", "member_signal_key", "event_index", "event_number", "symbol", "side", "order_type",
                  "requested_quantity", "created_at", "bus_sequence")
SUMMARY_COLUMNS = ("export_scope", "member", "member_label", "strategy_kind", "strategy_id", "run_id",
                   "events_counted", "buy_requests", "sell_requests", "signal_requests", "evaluated", "warming_up",
                   "ignored")
NOTE = ("Signal requests only: no fills, returns or portfolio. Counts are not a measure of strategy quality.")


def scope_full(model: CompareModel) -> str:
    return f"full run: all {model.total} events, all symbols, both configurations"


def scope_prefix(model: CompareModel, cursor: int, symbol: str | None) -> str:
    shown = "all symbols" if symbol is None else f"symbol {symbol}"
    first = "none revealed" if cursor == 0 else f"events 1-{cursor}"
    return f"visible replay prefix: {first} of {model.total}, {shown}, both configurations"


def _slug(text: str) -> str:
    return exports._slug(text)


def _info(comparison: CompletedComparison, member: str) -> tuple[Any, Any]:
    run = comparison.run_of(member)
    return run.config, run.document.strategies[0]


def signals_csv(comparison: CompletedComparison, signals: Sequence[CompareSignal], scope: str) -> bytes:
    keys = sorted({k for item in signals for k in item.signal.metadata})
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow([*SIGNAL_COLUMNS, *(f"metadata.{k}" for k in keys)])
    for item in signals:
        config, strategy = _info(comparison, item.member)
        s = item.signal
        writer.writerow([scope, item.member, config.label, config.kind, s.strategy_id, comparison.run_of(item.member).document.run_id,
                         s.signal_id, s.signal_ref, item.member_ref, s.event_index, s.event_index + 1, s.symbol, s.side,
                         s.order_type or "", exports._text(s.requested_quantity), s.created_at,
                         exports._text(s.bus_sequence), *(s.metadata.get(k, "") for k in keys)])
    return buffer.getvalue().encode("utf-8")


def summary_csv(comparison: CompletedComparison, cursor: int, symbol: str | None, scope: str) -> bytes:
    counts = comparison.model.counts(cursor, symbol)
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(SUMMARY_COLUMNS)
    for member in comparison.model.members:
        config, strategy = _info(comparison, member)
        c = counts[member]
        writer.writerow([scope, member, config.label, config.kind, strategy.strategy_id,
                         comparison.run_of(member).document.run_id, c.events, c.buys, c.sells, c.signals,
                         c.verdicts.get("evaluated", 0), c.verdicts.get("warming_up", 0), c.verdicts.get("ignored", 0)])
    return buffer.getvalue().encode("utf-8")


def provenance_json(comparison: CompletedComparison, scope: str, member_files: dict[str, str]) -> bytes:
    """The comparison's configuration and provenance. `member_files` maps a member to its raw run file's name."""
    first = comparison.runs[0].document
    members = []
    for run in comparison.runs:
        doc = run.document
        members.append({
            "member": run.config.member, "label": run.config.label, "strategy_kind": run.config.kind,
            "parameters_as_passed": [{"name": n, "value": v} for n, v in run.config.params],
            "parameters_as_run": dict(doc.strategies[0].parameters), "derived": dict(doc.strategies[0].derived),
            "strategy_id": doc.strategies[0].strategy_id, "run_id": doc.run_id,
            "result_sha256": doc.provenance.get("result_sha256"), "schema_version": doc.schema_version,
            "signal_requests_full_run": len(doc.signals), "warnings": [w.code for w in doc.warnings],
            "raw_result_file": member_files.get(run.config.member)})
    document = {
        "export": "strategy_lab.comparison_provenance", "export_version": "1.0", "scope": scope, "note": NOTE,
        "comparison_number_in_session": comparison.number, "finished_at": comparison.finished_at,
        "dataset": {"name": comparison.source.display_name, "kind": comparison.source.kind,
                    "synthetic_fixture": comparison.source.synthetic, "provenance": comparison.source.provenance,
                    "sha256": first.dataset.sha256, "bytes": first.dataset.bytes, "rows": first.dataset.rows,
                    "format": first.dataset.format},
        "shared_by_both_members": {"dataset_sha256": first.dataset.sha256, "event_count": len(first.events),
                                   "symbols_allowlist": comparison.request.members[0].param("symbols"),
                                   "replay_order": first.context.get("replay_order"), "clock": first.context.get("clock"),
                                   "bus_fault": first.bus_fault,
                                   "independent_engine_runs": "each member ran in its own fresh engine; signal ids repeat across them"},
        "members": members,
        "executable": {"file_name": comparison.runner.path.replace("\\", "/").rsplit("/", 1)[-1],
                       "sha256": comparison.runner.sha256, "size_bytes": comparison.runner.size,
                       "tool": first.provenance.get("tool"), "version": first.provenance.get("project_version"),
                       "compiler": first.provenance.get("compiler"), "build": first.provenance.get("build")}}
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def package_zip(comparison: CompletedComparison) -> bytes:
    """FULL RUN package: provenance, both original runner results, the labelled summary and every signal request."""
    model = comparison.model
    scope = scope_full(model)
    files = {run.config.member: f"{run.config.member}_{_slug(run.document.run_id)}_run.json" for run in comparison.runs}
    entries: dict[str, bytes] = {
        "comparison_provenance.json": provenance_json(comparison, scope, files),
        "comparison_summary_full_run.csv": summary_csv(comparison, model.total, None, scope),
        "comparison_signals_full_run.csv": signals_csv(comparison, model.signals_through(model.total), scope),
    }
    for run in comparison.runs:
        entries[files[run.config.member]] = run.raw_json            # byte for byte, never re-serialized
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))     # fixed: the same input gives the same bytes
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, entries[name])
    return buffer.getvalue()


def _ids(comparison: CompletedComparison) -> str:
    return "_".join(_slug(r.document.run_id)[:8] for r in comparison.runs)


def package_filename(comparison: CompletedComparison) -> str:
    return f"strategy_lab_compare_{_ids(comparison)}_full_run.zip"


def signals_filename_prefix(comparison: CompletedComparison, cursor: int, symbol: str | None) -> str:
    return (f"strategy_lab_compare_{_ids(comparison)}_signals_prefix_{cursor}of{comparison.model.total}_"
            f"{_slug(symbol) if symbol else 'all'}.csv")


def summary_filename_prefix(comparison: CompletedComparison, cursor: int, symbol: str | None) -> str:
    return (f"strategy_lab_compare_{_ids(comparison)}_summary_prefix_{cursor}of{comparison.model.total}_"
            f"{_slug(symbol) if symbol else 'all'}.csv")

