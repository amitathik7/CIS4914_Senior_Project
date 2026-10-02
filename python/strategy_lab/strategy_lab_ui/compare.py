"""Compare: two configurations of the real strategies, replayed independently over ONE shared dataset.

* Each member (A, B) is its own run of the existing bridge: a fresh engine, fresh strategy state, the same dataset bytes
  (the same SHA-256), the same lab bus, clock and replay order. Nothing is shared between the two engine runs, so their
  signal ids both count 1, 2, 3...: every join here is scoped by member, and the raw ids are kept untouched.
* A comparison exists only when BOTH runs succeeded and describe the same event sequence. If either fails the other's result
  is not presented as half a comparison (see `ComparisonFailure`).
* Python computes no decision. Counts are counts of recorded signal requests; readiness is read from the recorded window.
* Signal-only: no fills, returns or portfolio exist, and nothing here ranks one configuration above the other.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Callable, Mapping, Sequence

from . import bridge
from .bridge import RunOutcome, Runner
from .catalog import StrategySpec
from .datasets import DatasetSource
from .errors import LabUiError
from .replay import PrefixCounts, ReplayModel
from .schema import Event, ReplayDocument, Signal, StrategyEventResult

MEMBERS = ("A", "B")
_LABEL_SKIP = ("symbols", "strategy_id", "requested_quantity")
SYMBOLS_PARAM = "symbols"


def make_label(member: str, spec: StrategySpec | None, kind: str, params: Sequence[tuple[str, str]]) -> str:
    """'A: Moving-average crossover (short_window 2, long_window 3)': the exact settings, never a nickname."""
    shown = ", ".join(f"{name} {text}" for name, text in params if name not in _LABEL_SKIP)
    return f"{member}: {spec.title if spec else kind}" + (f" ({shown})" if shown else "")


def make_short(member: str, spec: StrategySpec | None, kind: str) -> str:
    """'A: Moving-average crossover': the name used where space is tight (the parameters are in the full label)."""
    return f"{member}: {spec.title if spec else kind}"


@dataclass(frozen=True)
class MemberConfig:
    member: str
    kind: str
    params: tuple[tuple[str, str], ...]          # exactly the texts passed as --param, catalog order
    label: str
    short: str = ""

    def param(self, name: str) -> str | None:
        return dict(self.params).get(name)


@dataclass(frozen=True)
class CompareRequest:
    """Everything that decides what a comparison shows: the immutable snapshot a result is labelled with."""

    dataset_kind: str
    dataset_key: str
    dataset_sha256: str
    members: tuple[MemberConfig, ...]
    executable_sha256: str

    def differences(self, other: "CompareRequest") -> list[str]:
        """What changed between two requests, in words for the banner (per member, never mixed up)."""
        reasons: list[str] = []
        if (self.dataset_kind, self.dataset_key) != (other.dataset_kind, other.dataset_key):
            reasons.append(f"dataset ({other.dataset_key} -> {self.dataset_key})")
        elif self.dataset_sha256 != other.dataset_sha256:
            reasons.append("dataset contents")
        mine_symbols = {m.param(SYMBOLS_PARAM) for m in self.members}
        theirs_symbols = {m.param(SYMBOLS_PARAM) for m in other.members}
        if mine_symbols != theirs_symbols:
            reasons.append("shared symbols allowlist")
        for now, before in zip(self.members, other.members):
            if now.kind != before.kind:
                reasons.append(f"{now.member}: strategy ({before.kind} -> {now.kind})")
                continue
            a, b = dict(now.params), dict(before.params)
            changed = [n for n in dict.fromkeys([*b, *a]) if n != SYMBOLS_PARAM and a.get(n) != b.get(n)]
            if changed:
                reasons.append(f"{now.member}: parameters (" + ", ".join(changed) + ")")
        if self.executable_sha256 != other.executable_sha256:
            reasons.append("replay executable was rebuilt or replaced")
        return reasons


# ---- the two runs and the model over both -------------------------------------------------------------

@dataclass(frozen=True, eq=False)
class MemberRun:
    config: MemberConfig
    outcome: RunOutcome
    model: ReplayModel

    @property
    def document(self) -> ReplayDocument:
        return self.outcome.document

    @property
    def raw_json(self) -> bytes:
        return self.outcome.process.stdout


@dataclass(frozen=True)
class CompareSignal:
    """A signal request together with the member whose run produced it. `signal_id` is only unique inside one run."""

    member: str
    signal: Signal

    @property
    def member_ref(self) -> str:
        return f"{self.member}/{self.signal.signal_ref}"


@dataclass(frozen=True)
class Readiness:
    symbol: str
    state: str                    # ready | warming_up | not_tracked | no_rows
    fill: int | None = None
    size: int | None = None
    as_of_event: int | None = None   # 1-based event number whose recorded window is quoted


_ALIGNED_EVENT_FIELDS = ("index", "source_line", "symbol", "exchange_time", "type", "price", "price_status",
                         "open", "high", "low", "volume")


def check_aligned(first: ReplayDocument, second: ReplayDocument) -> None:
    """Both runs must describe the same dataset, event order and replay policy, else they are not comparable."""
    problems: list[str] = []
    if first.dataset.sha256 != second.dataset.sha256:
        problems.append("the datasets differ (SHA-256)")
    for key in ("clock", "replay_order", "bus_fault"):
        if first.context.get(key) != second.context.get(key):
            problems.append(f"the replay {key.replace('_', ' ')} differs")
    if len(first.events) != len(second.events):
        problems.append(f"the event counts differ ({len(first.events)} and {len(second.events)})")
    else:
        for a, b in zip(first.events, second.events):
            if any(getattr(a, f) != getattr(b, f) for f in _ALIGNED_EVENT_FIELDS):
                problems.append(f"event {a.index + 1} is not the same row in both runs")
                break
    if problems:
        raise LabUiError("comparison_mismatch", "The two runs cannot be compared: " + "; ".join(problems) + ".")


class CompareModel:
    """The shared cursor, filter and joins over both members. Quacks like a `ReplayModel` for the playback controls."""

    def __init__(self, runs: Sequence[MemberRun]) -> None:
        if len(runs) != 2:
            raise ValueError("a comparison has exactly two members")
        check_aligned(runs[0].document, runs[1].document)
        self.runs = tuple(runs)
        self.members = tuple(r.config.member for r in runs)
        self.models: Mapping[str, ReplayModel] = {r.config.member: r.model for r in runs}
        base = runs[0].model
        self.events = base.events
        self.total = base.total
        self.symbols = base.symbols
        self.all_label = base.all_label

    # the cursor (identical for both members: the same events in the same order)
    def clamp(self, cursor: int) -> int:
        return self.models[self.members[0]].clamp(cursor)

    def next_bar(self, cursor: int, symbol: str | None = None) -> int | None:
        return self.models[self.members[0]].next_bar(cursor, symbol)

    def previous_bar(self, cursor: int, symbol: str | None = None) -> int | None:
        return self.models[self.members[0]].previous_bar(cursor, symbol)

    def next_signal(self, cursor: int, symbol: str | None = None) -> int | None:
        """The next event after the cursor where EITHER member produced a signal request (for the display filter)."""
        found = [p for p in (m.next_signal(cursor, symbol) for m in self.models.values()) if p is not None]
        return min(found) if found else None

    def selected_event(self, cursor: int) -> Event | None:
        return self.models[self.members[0]].selected_event(cursor)

    def events_through(self, cursor: int, symbol: str | None = None) -> list[Event]:
        return self.models[self.members[0]].events_through(cursor, symbol)

    # what the prefix shows, per member
    def result_of(self, member: str, event: Event) -> StrategyEventResult:
        """The member's OWN recorded result for this event. `event` identifies the row (the shared sequence); the result
        is read from that member's document, never from the event object of the other member's run."""
        model = self.models[member]
        return model.result_of(model.events[event.index])

    def signals_through(self, cursor: int, symbol: str | None = None) -> list[CompareSignal]:
        found = [CompareSignal(member, s) for member in self.members
                 for s in self.models[member].signals_through(cursor, symbol)]
        order = {m: i for i, m in enumerate(self.members)}
        return sorted(found, key=lambda c: (c.signal.event_index, order[c.member], c.signal.signal_id))

    def signals_on(self, event_index: int) -> list[CompareSignal]:
        return [CompareSignal(member, s) for member in self.members for s in self.models[member].signals_on(event_index)]

    def counts(self, cursor: int, symbol: str | None = None) -> dict[str, PrefixCounts]:
        return {member: self.models[member].counts(cursor, symbol) for member in self.members}

    def readiness(self, member: str, cursor: int, symbol: str | None = None) -> list[Readiness]:
        """Per symbol: is this member's recorded window full, as of the latest revealed row that reports one?

        Read from `window` only. It is NOT inferred from signals: a ready strategy may emit nothing."""
        model = self.models[member]
        shown = list(model.symbols) if symbol is None else [symbol]
        revealed = model.events_through(cursor)
        out: list[Readiness] = []
        for sym in shown:
            rows = [e for e in revealed if e.symbol == sym]
            if not rows:
                out.append(Readiness(sym, "no_rows"))
                continue
            for event in reversed(rows):
                result = model.result_of(event)
                if result.diagnostics_available and result.window_fill is not None:
                    full = (result.window_remaining or 0) == 0
                    out.append(Readiness(sym, "ready" if full else "warming_up", result.window_fill,
                                         result.window_size, event.index + 1))
                    break
            else:
                out.append(Readiness(sym, "not_tracked", as_of_event=rows[-1].index + 1))
        return out


@dataclass(frozen=True, eq=False)
class CompletedComparison:
    request: CompareRequest
    source: DatasetSource                          # identity only: the bytes are not retained
    runner: Runner
    runs: tuple[MemberRun, ...]
    model: CompareModel
    number: int                                    # which comparison launch of this session produced it
    finished_at: str

    def run_of(self, member: str) -> MemberRun:
        return next(r for r in self.runs if r.config.member == member)


@dataclass(frozen=True, eq=False)
class FailedComparison:
    request: CompareRequest | None
    errors: Mapping[str, LabUiError]               # member -> why it failed ("" key: a problem with the pair)
    succeeded: tuple[str, ...]                     # members whose own run finished (their result is NOT shown)
    number: int
    at: str


class ComparisonFailure(Exception):
    """At least one member failed, or the two runs do not describe the same events: there is no comparison."""

    def __init__(self, errors: Mapping[str, LabUiError], succeeded: Sequence[str]) -> None:
        super().__init__("; ".join(f"{m}: {e}" for m, e in errors.items()))
        self.errors = dict(errors)
        self.succeeded = tuple(succeeded)


Progress = Callable[[str, str], None]               # (member, "running" | "done" | "failed")


def execute(runner: Runner, source: DatasetSource, members: Sequence[MemberConfig], number: int, now: str,
            progress: Progress | None = None) -> CompletedComparison:
    """Run each member independently, in order, then pair them. Raises ComparisonFailure; never returns half a result.

    Both members are always attempted, so one message can name every configuration that failed."""
    request = CompareRequest(source.kind, source.key, source.sha256, tuple(members), runner.sha256)
    runs: list[MemberRun] = []
    errors: dict[str, LabUiError] = {}
    for config in members:
        if progress:
            progress(config.member, "running")
        try:
            outcome = bridge.run_replay(runner, dataset=source.data, dataset_sha256=source.sha256, kind=config.kind,
                                        params=config.params)
        except LabUiError as error:
            errors[config.member] = error
            if progress:
                progress(config.member, "failed")
            continue
        runs.append(MemberRun(config, outcome, ReplayModel(outcome.document)))
        if progress:
            progress(config.member, "done")
    if errors:
        raise ComparisonFailure(errors, [r.config.member for r in runs])
    try:
        model = CompareModel(runs)
    except LabUiError as error:
        raise ComparisonFailure({"": error}, [r.config.member for r in runs]) from error
    return CompletedComparison(request, replace(source, data=b""), runner, tuple(runs), model, number, now)
