"""The only place that starts strategy_lab_replay.

* The executable is located and identified (path, size, mtime, SHA-256) before use. No path
  ever comes from the browser.
* The process is started from an argument list with shell=False, in a private temporary
  directory that is always removed. Every argument travels in an `@args.txt` response file
  (UTF-8, one argument per line): on Windows the C runtime hands the tool its command line in the
  ANSI code page, so only a pure-ASCII relative `@args.txt` can be passed directly. The dataset is
  copied to `dataset.csv` next to it and referenced relatively, so no user-controlled path
  reaches the command line.
* Time and output are bounded; a run that exceeds either is killed and reported.
* Exit statuses follow docs/STRATEGY_LAB.md section 6: 0 result, 2 usage/configuration,
  3 dataset, 4 internal. stdout is exactly one JSON document; stderr is human text and is only
  ever shown as technical detail.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

from .catalog import Catalog, parse_catalog, parse_error_document
from .errors import LabUiError
from .jsonio import excerpt, strict_loads
from .schema import ReplayDocument, parse_replay

REPO_ROOT = Path(__file__).resolve().parents[3]
EXE_ENV = "STRATEGY_LAB_EXE"
EXE_NAME = "strategy_lab_replay.exe" if os.name == "nt" else "strategy_lab_replay"

DEFAULT_TIMEOUT_S = 60.0
CATALOG_TIMEOUT_S = 20.0
MAX_STDOUT_BYTES = 128 * 1024 * 1024
MAX_STDERR_BYTES = 1024 * 1024
UI_MAX_ROWS = 20_000                       # passed as --max-rows: the tool itself refuses larger files
ARGS_FILE = "args.txt"
DATASET_FILE = "dataset.csv"

BUILD_HELP = (
    'cmake -S . -B out/strategy_lab -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF '
    "-DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON\n"
    "cmake --build out/strategy_lab --config Release --target strategy_lab_replay"
)


# ---- locating and identifying the executable ------------------------------------------------

@dataclass(frozen=True)
class Runner:
    """What is started, and an identity that changes whenever the program does."""

    command: tuple[str, ...]          # argv prefix; the tool itself, or an interpreter plus script in tests
    path: str
    size: int
    mtime_ns: int
    sha256: str

    @property
    def identity(self) -> tuple[str, int, int, str]:
        return (self.path, self.size, self.mtime_ns, self.sha256)


_HASHES: dict[tuple[str, int, int], str] = {}


def _hash_file(path: Path, size: int, mtime_ns: int) -> str:
    key = (str(path), size, mtime_ns)
    if key not in _HASHES:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for block in iter(lambda: handle.read(1 << 20), b""):
                digest.update(block)
        _HASHES[key] = digest.hexdigest()
    return _HASHES[key]


def identify(path: Path, command: Sequence[str] | None = None) -> Runner:
    """Resolve and fingerprint a file. Raises executable_missing / executable_invalid."""
    try:
        resolved = path.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise LabUiError("executable_missing", f"No file at {path}.", detail=str(error)) from error
    if not resolved.is_file():
        raise LabUiError("executable_invalid", f"{resolved} is not a file.")
    if command is None and os.name == "nt" and resolved.suffix.lower() != ".exe":
        raise LabUiError("executable_invalid", f"{resolved} is not an .exe file.")
    try:
        stat = resolved.stat()
        digest = _hash_file(resolved, stat.st_size, stat.st_mtime_ns)
    except OSError as error:
        raise LabUiError("executable_invalid", f"{resolved} cannot be read.", detail=str(error)) from error
    return Runner(tuple(command) if command else (str(resolved),), str(resolved), stat.st_size,
                  stat.st_mtime_ns, digest)


def default_candidates(repo_root: Path = REPO_ROOT) -> list[Path]:
    base = repo_root / "out" / "strategy_lab" / "bin"
    return [base / config / EXE_NAME for config in ("Release", "RelWithDebInfo", "MinSizeRel", "Debug")] + [base / EXE_NAME]


def find_runner(explicit: str | None = None, environ: Mapping[str, str] | None = None,
                repo_root: Path = REPO_ROOT) -> Runner:
    """`explicit`, else $STRATEGY_LAB_EXE, else the documented build output (Release first)."""
    environ = os.environ if environ is None else environ
    chosen = explicit or environ.get(EXE_ENV) or ""
    if chosen:
        return identify(Path(chosen))
    candidates = default_candidates(repo_root)
    for candidate in candidates:
        if candidate.is_file():
            return identify(candidate)
    raise LabUiError(
        "executable_missing",
        "strategy_lab_replay has not been built. Build it once (see the commands in the technical details), "
        f"or set {EXE_ENV} to an existing executable.",
        detail="Looked for:\n" + "\n".join(f"  {c}" for c in candidates) + "\n\nBuild with:\n" + BUILD_HELP)


# ---- running a process ----------------------------------------------------------------------

@dataclass(frozen=True)
class ProcessResult:
    argv: tuple[str, ...]
    args_file: str                    # the exact response-file text the tool was given
    exit_code: int
    stdout: bytes
    stderr: str
    stderr_truncated: bool
    duration_s: float


class _Pipe:
    def __init__(self, stream, limit: int, on_overflow=None) -> None:
        self.stream, self.limit, self.on_overflow = stream, limit, on_overflow
        self.buffer = bytearray()
        self.overflow = False
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _pump(self) -> None:
        try:
            while True:
                chunk = self.stream.read(65536)
                if not chunk:
                    return
                room = self.limit - len(self.buffer)
                if room > 0:
                    self.buffer += chunk[:room]
                if len(chunk) > room and not self.overflow:
                    self.overflow = True
                    if self.on_overflow is not None:
                        self.on_overflow()
        except (OSError, ValueError):
            return


def encode_args(args: Sequence[str]) -> bytes:
    """The response file: one argument per line. Refuses what that format cannot carry."""
    lines = []
    for arg in args:
        if arg == "" or "\r" in arg or "\n" in arg or arg[0] in "#@":
            raise LabUiError(
                "input_unencodable",
                "A setting is empty, contains a line break, or starts with '#' or '@', which the replay tool's "
                f"argument file cannot carry: {arg[:40]!r}.")
        lines.append(arg)
    try:
        return ("\n".join(lines) + "\n").encode("utf-8")
    except UnicodeEncodeError as error:
        raise LabUiError("input_unencodable", "A setting contains characters that cannot be encoded as UTF-8.",
                         detail=str(error)) from error


def run_process(runner: Runner, args: Sequence[str], *, timeout: float = DEFAULT_TIMEOUT_S,
                max_stdout: int = MAX_STDOUT_BYTES, files: Mapping[str, bytes] | None = None) -> ProcessResult:
    args_bytes = encode_args(args)
    with tempfile.TemporaryDirectory(prefix="strategy_lab_ui_", ignore_cleanup_errors=True) as work:
        workdir = Path(work)
        for name, data in (files or {}).items():
            (workdir / name).write_bytes(data)
        (workdir / ARGS_FILE).write_bytes(args_bytes)
        argv = (*runner.command, f"@{ARGS_FILE}")
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        started = time.perf_counter()
        try:
            proc = subprocess.Popen(argv, cwd=work, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, shell=False, bufsize=0, creationflags=flags)
        except OSError as error:
            raise LabUiError("spawn_failed", f"Could not start {runner.path}: {error}", detail=repr(error)) from error

        out = _Pipe(proc.stdout, max_stdout, on_overflow=lambda: _kill(proc))
        err = _Pipe(proc.stderr, MAX_STDERR_BYTES)
        deadline = time.monotonic() + timeout
        timed_out = False
        try:
            while True:
                try:
                    proc.wait(timeout=max(0.0, min(0.05, deadline - time.monotonic())))
                    break
                except subprocess.TimeoutExpired:
                    if time.monotonic() >= deadline:
                        timed_out = True
                        _kill(proc)
                        break
            proc.wait(timeout=15)
        finally:
            _kill(proc)
            out.thread.join(5)
            err.thread.join(5)
            for stream in (proc.stdout, proc.stderr):
                try:
                    stream.close()
                except OSError:
                    pass
        duration = time.perf_counter() - started

        stderr = bytes(err.buffer).decode("utf-8", errors="replace")
        if timed_out:
            raise LabUiError("timeout", f"The replay tool did not finish within {timeout:g} seconds and was stopped.",
                             stderr=stderr, detail=f"command: {' '.join(argv)}\n{args_bytes.decode('utf-8', 'replace')}")
        if out.overflow:
            raise LabUiError("output_too_large",
                             f"The replay tool wrote more than {max_stdout // (1024 * 1024)} MiB and was stopped. "
                             "Use a smaller dataset.", stderr=stderr)
        return ProcessResult(argv, args_bytes.decode("utf-8"), proc.returncode, bytes(out.buffer), stderr,
                             err.overflow, duration)


def _kill(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        try:
            proc.kill()
        except OSError:
            pass


# ---- interpreting the result ------------------------------------------------------------------

_FAILURE_KINDS = {2: "runner_rejected", 3: "dataset_invalid", 4: "runner_internal_error"}


def _raise_for_status(result: ProcessResult) -> None:
    """Exit status 0 passes. 2/3/4 become the tool's own error document; anything else is unexpected."""
    code = result.exit_code
    if code == 0:
        return
    if code not in _FAILURE_KINDS:
        raise LabUiError("unexpected_exit", f"The replay tool exited with status {code}, which it never uses.",
                         exit_code=code, stderr=result.stderr, stdout_excerpt=excerpt(result.stdout),
                         detail=f"exit status {code}")
    try:
        document = parse_error_document(strict_loads(result.stdout))
    except LabUiError as error:
        raise LabUiError("malformed_output",
                         f"The replay tool exited with status {code} but did not write a readable error document.",
                         exit_code=code, stderr=result.stderr, stdout_excerpt=excerpt(result.stdout),
                         detail=f"{error.message} {error.detail}".strip()) from error
    if document.exit_status != code:
        raise LabUiError("malformed_output",
                         f"The error document says exit status {document.exit_status} but the process returned {code}.",
                         exit_code=code, stderr=result.stderr, stdout_excerpt=excerpt(result.stdout))
    raise LabUiError(_FAILURE_KINDS[code], document.message, exit_code=code, stderr=result.stderr,
                     problems=document.problems, total_problems=document.total_problems,
                     detail=f"error code {document.code}", runner_code=document.code)


def describe(runner: Runner, timeout: float = CATALOG_TIMEOUT_S) -> Catalog:
    """Ask the tool what it can run. Also proves the file really is the lab's replay tool."""
    result = run_process(runner, ["describe"], timeout=timeout)
    try:
        _raise_for_status(result)
        return parse_catalog(strict_loads(result.stdout))
    except LabUiError as error:
        error.stderr = error.stderr or result.stderr
        if error.kind in ("malformed_output", "unsupported_schema", "invalid_structure", "unexpected_exit"):
            raise LabUiError("executable_invalid", f"{runner.path} did not answer 'describe' as strategy_lab_replay: "
                             f"{error.message}", detail=error.detail, stderr=result.stderr,
                             stdout_excerpt=excerpt(result.stdout), exit_code=result.exit_code) from error
        raise


@dataclass(frozen=True)
class RunOutcome:
    process: ProcessResult
    document: ReplayDocument


StrategyArgs = tuple[str, Sequence[tuple[str, str]]]      # (kind, [(parameter, text), ...])


def run_replay(runner: Runner, *, dataset: bytes, dataset_sha256: str, kind: str,
               params: Sequence[tuple[str, str]], max_rows: int = UI_MAX_ROWS,
               timeout: float = DEFAULT_TIMEOUT_S) -> RunOutcome:
    """Launch ONE single-strategy replay (Explore, and each member of a Compare). Only an explicit Run calls it."""
    return run_replay_multi(runner, dataset=dataset, dataset_sha256=dataset_sha256, strategies=[(kind, params)],
                            max_rows=max_rows, timeout=timeout)


def run_replay_multi(runner: Runner, *, dataset: bytes, dataset_sha256: str, strategies: Sequence[StrategyArgs],
                     max_rows: int = UI_MAX_ROWS, timeout: float = DEFAULT_TIMEOUT_S) -> RunOutcome:
    """Launch ONE replay of one or more strategies in one engine (command-line order is registration order).

    Called only on an explicit Run (Explore, Compare) or Run validation (scenarios that need several strategies)."""
    args = ["run", "--dataset", DATASET_FILE]
    for kind, params in strategies:
        args += ["--strategy", kind]
        for name, text in params:
            args += ["--param", f"{name}={text}"]
    args += ["--max-rows", str(max_rows)]
    result = run_process(runner, args, timeout=timeout, files={DATASET_FILE: dataset})
    _raise_for_status(result)
    try:
        document = parse_replay(strict_loads(result.stdout))
    except LabUiError as error:
        error.stderr = error.stderr or result.stderr
        error.exit_code = result.exit_code
        error.stdout_excerpt = error.stdout_excerpt or excerpt(result.stdout)
        raise
    _check_matches_request(document, dataset_sha256, [kind for kind, _ in strategies], max_rows, result)
    return RunOutcome(result, document)


def _check_matches_request(document: ReplayDocument, sha256: str, kinds: Sequence[str], max_rows: int,
                           result: ProcessResult) -> None:
    problems = []
    if document.dataset.sha256 != sha256:
        problems.append(f"the tool read a dataset with SHA-256 {document.dataset.sha256}, not {sha256}")
    if [c.kind for c in document.strategies] != list(kinds):
        problems.append(f"expected the strategies {list(kinds)}, got {[c.kind for c in document.strategies]}")
    if document.max_rows != max_rows:
        problems.append(f"the tool applied max_rows={document.max_rows}, not {max_rows}")
    if problems:
        raise LabUiError("result_mismatch", "The result does not describe the run that was requested: "
                         + "; ".join(problems) + ".", exit_code=result.exit_code, stderr=result.stderr)
