"""The four kinds of state, kept apart (no Streamlit here, so all of it is unit-tested directly).

1. Editable settings: whatever the widgets hold now. Reduced to a `RunRequest` only when needed.
2. The immutable snapshot of a run: its `RunRequest` (dataset identity, strategy, every parameter text
   sent, executable identity), inside a `CompletedRun`.
3. The parsed completed result: `CompletedRun.outcome.document` and the `ReplayModel` built on it.
4. Playback position and display filters: plain integers/strings the UI keeps in session state; they
   never reach this module's run functions.

A result is only ever described as "current" when its snapshot equals the request built from the
settings on screen right now. Anything else is labelled stale, with the reasons.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from datetime import datetime
from typing import Sequence

from .bridge import RunOutcome, Runner, run_replay
from .datasets import DatasetSource
from .errors import LabUiError
from .replay import ReplayModel


@dataclass(frozen=True)
class RunRequest:
    dataset_kind: str
    dataset_key: str
    dataset_sha256: str
    strategy_kind: str
    params: tuple[tuple[str, str], ...]          # exactly the texts passed as --param, in order
    executable_sha256: str

    def differences(self, other: "RunRequest") -> list[str]:
        """What changed between two requests, in words for the banner."""
        reasons: list[str] = []
        if (self.dataset_kind, self.dataset_key) != (other.dataset_kind, other.dataset_key):
            reasons.append(f"dataset ({other.dataset_key} -> {self.dataset_key})")
        elif self.dataset_sha256 != other.dataset_sha256:
            reasons.append("dataset contents")
        if self.strategy_kind != other.strategy_kind:
            reasons.append(f"strategy ({other.strategy_kind} -> {self.strategy_kind})")
        mine, theirs = dict(self.params), dict(other.params)
        changed = [name for name in dict.fromkeys([*theirs, *mine]) if mine.get(name) != theirs.get(name)]
        if changed and self.strategy_kind == other.strategy_kind:
            reasons.append("parameters (" + ", ".join(changed) + ")")
        if self.executable_sha256 != other.executable_sha256:
            reasons.append("replay executable was rebuilt or replaced")
        return reasons


def make_request(source: DatasetSource, runner: Runner, kind: str, params: Sequence[tuple[str, str]]) -> RunRequest:
    return RunRequest(source.kind, source.key, source.sha256, kind, tuple(params), runner.sha256)


@dataclass(frozen=True, eq=False)
class CompletedRun:
    request: RunRequest
    source: DatasetSource                         # identity only: the bytes are not retained
    runner: Runner
    outcome: RunOutcome
    model: ReplayModel
    number: int                                   # which launch of this session produced it
    finished_at: str

    @property
    def document(self):
        return self.outcome.document

    @property
    def raw_json(self) -> bytes:
        return self.outcome.process.stdout


@dataclass(frozen=True, eq=False)
class FailedRun:
    request: RunRequest | None
    error: LabUiError
    number: int
    at: str


def _now() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


now = _now                              # shared with the Compare and Validate views


def execute(runner: Runner, source: DatasetSource, kind: str, params: Sequence[tuple[str, str]],
            number: int) -> CompletedRun:
    """Launch ONE replay and return the immutable record of it. Raises LabUiError on any failure."""
    request = make_request(source, runner, kind, params)
    outcome = run_replay(runner, dataset=source.data, dataset_sha256=source.sha256, kind=kind, params=params)
    return CompletedRun(request, replace(source, data=b""), runner, outcome, ReplayModel(outcome.document),
                        number, _now())


def failed(request: RunRequest | None, error: LabUiError, number: int) -> FailedRun:
    return FailedRun(request, error, number, _now())


@dataclass(frozen=True)
class Status:
    kind: str                       # empty | current | stale | failed
    reasons: tuple[str, ...] = ()   # why a displayed result is stale


def run_status(displayed: CompletedRun | None, failure: FailedRun | None, current: RunRequest | None) -> Status:
    stale: tuple[str, ...] = ()
    if displayed is not None:
        if current is None:
            stale = ("the settings on screen cannot be run as they are",)
        else:
            stale = tuple(current.differences(displayed.request))
    if failure is not None:
        return Status("failed", stale)
    if displayed is None:
        return Status("empty")
    return Status("stale", stale) if stale else Status("current")
