"""Tables and sentences for the Compare view. Rows come only from the visible prefix and the display filter, or from the
selected event. Everything is read from the two runs' recorded output; nothing ranks the configurations.
"""

from __future__ import annotations

import pandas as pd

from .compare import CompareModel, CompletedComparison, MemberRun, Readiness
from .schema import Event, StrategyEventResult
from .tables import ACTION_TEXT, SIDE_TEXT

READINESS_TEXT = {"ready": "Ready", "warming_up": "Warming up", "not_tracked": "No bar has reached the strategy",
                  "no_rows": "No rows revealed yet"}


def member_rows(run: MemberRun) -> pd.DataFrame:
    """Exact parameters as the tool resolved and ran them (defaults filled in), plus the derived facts it reported."""
    config = run.document.strategies[0]
    rows = [{"Item": name, "Value": plain(value)} for name, value in config.parameters.items()]
    rows += [{"Item": f"derived: {name}", "Value": plain(value)} for name, value in config.derived.items()]
    return pd.DataFrame(rows, columns=["Item", "Value"])


def plain(value: object) -> str:
    if isinstance(value, list):
        return ", ".join(str(v) for v in value)
    return str(value)


def counts_table(model: CompareModel, names: dict[str, str], cursor: int, symbol: str | None) -> pd.DataFrame:
    """Metrics as rows and one column per configuration, so it stays narrow enough for the side panel."""
    counts = model.counts(cursor, symbol)
    rows = []
    for item, pick in (
            ("Events revealed", lambda c: c.events), ("Buy requests", lambda c: c.buys),
            ("Sell requests", lambda c: c.sells), ("Evaluated bars", lambda c: c.verdicts.get("evaluated", 0)),
            ("Warming-up bars", lambda c: c.verdicts.get("warming_up", 0)),
            ("Ignored events", lambda c: c.verdicts.get("ignored", 0))):
        rows.append({"Item": item, **{names[m]: pick(counts[m]) for m in model.members}})
    return pd.DataFrame(rows)


def _readiness_text(r: Readiness) -> str:
    text = READINESS_TEXT[r.state]
    if r.fill is not None:
        text += f": {r.fill} of {r.size} accepted bars"
    if r.as_of_event is not None and r.state != "no_rows":
        text += f" (as of event {r.as_of_event})"
    return text


def readiness_table(model: CompareModel, names: dict[str, str], cursor: int, symbol: str | None) -> pd.DataFrame:
    """One row per symbol, one column per configuration."""
    per_member = {m: model.readiness(m, cursor, symbol) for m in model.members}
    rows = []
    for position, first in enumerate(per_member[model.members[0]]):
        rows.append({"Symbol": first.symbol, **{names[m]: _readiness_text(per_member[m][position]) for m in model.members}})
    return pd.DataFrame(rows)


def _result_text(result: StrategyEventResult) -> tuple[str, str, str, str]:
    if not result.diagnostics_available:
        return ("no diagnostics", result.unavailable_reason or "", "", "")
    left = result.window_remaining or 0
    window = ("not tracked for this event" if result.window_fill is None else
              f"{result.window_fill} of {result.window_size}" + (" (full)" if left == 0 else f" ({left} more needed)"))
    return (result.verdict or "", result.reason or "", ACTION_TEXT.get(result.action or "", result.action or ""), window)


def _indicator(result: StrategyEventResult, name: str) -> str:
    if name in result.indicators:
        value = result.indicators[name]
        return repr(value) if isinstance(value, float) else str(value)
    if name in result.unavailable:
        return f"unavailable: {result.unavailable[name]}"
    return "not reported by this strategy"


def decision_table(model: CompareModel, labels: dict[str, str], event: Event) -> pd.DataFrame:
    """The recorded decision of both members on one event, side by side. 'Differs' is only filled in where both reported."""
    first, second = model.members
    a, b = model.result_of(first, event), model.result_of(second, event)
    rows = []
    for name, x, y in zip(("Verdict", "Reason", "Outcome", "Window"), _result_text(a), _result_text(b)):
        rows.append({"Item": name, labels[first]: x, labels[second]: y, "Differs": "yes" if x != y else "no"})
    for name in dict.fromkeys([*a.indicators, *a.unavailable, *b.indicators, *b.unavailable]):
        x, y = _indicator(a, name), _indicator(b, name)
        both = name in {*a.indicators, *a.unavailable} and name in {*b.indicators, *b.unavailable}
        rows.append({"Item": name, labels[first]: x, labels[second]: y, "Differs": ("yes" if x != y else "no") if both else ""})
    return pd.DataFrame(rows)


def signals_at_event(model: CompareModel, labels: dict[str, str], event_index: int) -> pd.DataFrame:
    rows = []
    for item in model.signals_on(event_index):
        s = item.signal
        rows.append({"Configuration": labels[item.member], "Request": SIDE_TEXT.get(s.side, s.side),
                     "Signal id (raw)": str(s.signal_id), "Scoped reference": item.member_ref,
                     "Quantity": "" if s.requested_quantity is None else str(s.requested_quantity),
                     "Created (UTC)": s.created_at, "Trigger": s.metadata.get("trigger", "")})
    return pd.DataFrame(rows, columns=["Configuration", "Request", "Signal id (raw)", "Scoped reference", "Quantity",
                                       "Created (UTC)", "Trigger"])


def agreement_sentence(model: CompareModel, event_index: int) -> str:
    """What the two runs recorded on this event, in words. A description, never a verdict on either configuration."""
    by_member = {m: [s.signal.side for s in model.signals_on(event_index) if s.member == m] for m in model.members}
    first, second = model.members
    a, b = by_member[first], by_member[second]
    if not a and not b:
        return "Neither configuration produced a signal request on this event."
    if a and b:
        if set(a) == set(b):
            return f"Both configurations requested {SIDE_TEXT.get(a[0], a[0])} on this event (simultaneous, same side)."
        return (f"Opposing requests on this event: {first} {', '.join(SIDE_TEXT.get(s, s) for s in a)}; "
                f"{second} {', '.join(SIDE_TEXT.get(s, s) for s in b)}.")
    who, sides = (first, a) if a else (second, b)
    other = second if a else first
    return f"Only {who} produced a signal request here ({', '.join(SIDE_TEXT.get(s, s) for s in sides)}); {other} produced none."


def signals_table(model: CompareModel, labels: dict[str, str], cursor: int, symbol: str | None) -> pd.DataFrame:
    rows = []
    for item in model.signals_through(cursor, symbol):
        s = item.signal
        rows.append({"Configuration": labels[item.member], "Signal id (raw)": str(s.signal_id),
                     "Scoped reference": item.member_ref, "Event": s.event_index + 1,
                     "Symbol": s.symbol, "Request": SIDE_TEXT.get(s.side, s.side),
                     "Quantity": None if s.requested_quantity is None else float(s.requested_quantity),
                     "Created (UTC)": s.created_at, "Trigger": s.metadata.get("trigger", "")})
    return pd.DataFrame(rows, columns=["Configuration", "Signal id (raw)", "Scoped reference", "Event", "Symbol", "Request",
                                       "Quantity", "Created (UTC)", "Trigger"])


def diagnostics_table(model: CompareModel, cursor: int, symbol: str | None) -> pd.DataFrame:
    first, second = model.members
    rows = []
    for event in model.events_through(cursor, symbol):
        a, b = _result_text(model.result_of(first, event)), _result_text(model.result_of(second, event))
        rows.append({"Event": event.index + 1, "Time (UTC)": event.exchange_time, "Symbol": event.symbol,
                     "Type": event.type, "Close": None if event.price is None else float(event.price),
                     f"{first} verdict": a[0], f"{first} reason": a[1], f"{first} outcome": a[2], f"{first} window": a[3],
                     f"{second} verdict": b[0], f"{second} reason": b[1], f"{second} outcome": b[2],
                     f"{second} window": b[3], "Decisions differ": "yes" if a[:3] != b[:3] else "no"})
    return pd.DataFrame(rows)


def run_summary_rows(comparison: CompletedComparison) -> pd.DataFrame:
    rows = []
    for run in comparison.runs:
        doc = run.document
        rows.append({"Configuration": run.config.label, "strategy_id": doc.strategies[0].strategy_id,
                     "run_id": doc.run_id, "result_sha256": str(doc.provenance.get("result_sha256", ""))[:16] + "...",
                     "Signal requests (full run)": len(doc.signals), "Warnings": ", ".join(w.code for w in doc.warnings) or "none"})
    return pd.DataFrame(rows)
