"""One exception type for everything that can stop a run from reaching the screen.

`kind` is a stable machine-readable slug; `message` is a sentence for the user;
`detail` is technical text for the "Technical details" expander.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Problem:
    """One located problem reported by the runner (a CSV line/column or a configuration item)."""

    message: str
    line: int | None = None
    column: str | None = None
    where: str | None = None


# kind -> short title for the error banner.
TITLES: dict[str, str] = {
    "executable_missing": "The replay executable was not found",
    "executable_invalid": "The replay executable could not be used",
    "spawn_failed": "The replay executable could not be started",
    "timeout": "The replay took too long and was stopped",
    "output_too_large": "The replay produced more output than the UI accepts",
    "input_missing": "The built-in dataset file is missing",
    "input_empty": "The dataset is empty",
    "input_too_large": "The dataset is too large",
    "input_not_text": "The dataset is not a text file",
    "input_unencodable": "A setting cannot be passed to the replay tool",
    "runner_rejected": "The replay tool rejected the configuration",
    "dataset_invalid": "The dataset was rejected",
    "runner_internal_error": "The replay tool reported an internal error",
    "unexpected_exit": "The replay tool exited unexpectedly",
    "malformed_output": "The replay tool's output could not be read",
    "unsupported_schema": "The replay tool's output format is not supported",
    "invalid_structure": "The replay tool's output is inconsistent",
    "result_mismatch": "The result does not match the request",
    "comparison_mismatch": "The two runs do not describe the same event sequence",
    "scenario_invalid": "A validation scenario file is invalid",
    "fixture_changed": "A scenario's dataset file is not the one the scenario was written for",
}


class LabUiError(Exception):
    def __init__(
        self,
        kind: str,
        message: str,
        *,
        detail: str = "",
        exit_code: int | None = None,
        stderr: str = "",
        stdout_excerpt: str = "",
        problems: tuple[Problem, ...] = (),
        total_problems: int = 0,
        runner_code: str = "",
    ) -> None:
        super().__init__(message)
        self.kind = kind
        self.message = message
        self.detail = detail
        self.exit_code = exit_code
        self.stderr = stderr
        self.stdout_excerpt = stdout_excerpt
        self.problems = problems
        self.total_problems = max(total_problems, len(problems))
        self.runner_code = runner_code          # the tool's own error code (error document), when it wrote one

    @property
    def title(self) -> str:
        return TITLES.get(self.kind, "The run failed")

    def __str__(self) -> str:
        return f"[{self.kind}] {self.message}"
