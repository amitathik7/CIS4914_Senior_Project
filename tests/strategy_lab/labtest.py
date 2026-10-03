"""Shared helpers for the Strategy Lab process-level tests (stdlib only).

These tests run the real ``strategy_lab_replay`` executable as a subprocess and parse what it
writes with Python's real JSON parser, in strict mode. They are test-only: nothing here is used
by the tool.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path
from typing import Any

EXE = os.environ.get("STRATEGY_LAB_EXE", "")
PROBE = os.environ.get("STRATEGY_LAB_JSON_PROBE", "")
FIXTURES = Path(os.environ.get("STRATEGY_LAB_FIXTURES", ""))


def datasets(name: str) -> str:
    return str(FIXTURES / "datasets" / name)


class StrictJsonError(AssertionError):
    pass


def strict_loads(text: str, exact: bool = False) -> Any:
    """Parse ONE JSON document, refusing anything a strict consumer would refuse.

    Python's parser already rejects a trailing comma, a bare ``.5``, a comma used as a decimal point
    and raw control characters in strings. On top of that this refuses duplicate object keys and the
    non-standard literals NaN, Infinity and -Infinity (which ``json`` accepts by default).

    With ``exact=True`` every number written with a decimal point (a price) is read as a
    ``decimal.Decimal`` instead of a float, so it is compared digit for digit: a price is an int64
    count of 1e-6 and a float cannot hold the largest ones. Integers are always ``int``.
    """

    def no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        out: dict[str, Any] = {}
        for key, value in pairs:
            if key in out:
                raise StrictJsonError(f"duplicate object key {key!r}")
            out[key] = value
        return out

    def refuse_constant(name: str) -> Any:
        raise StrictJsonError(f"non-standard JSON literal {name}")

    decoder = json.JSONDecoder(object_pairs_hook=no_duplicates, parse_constant=refuse_constant,
                               parse_float=Decimal if exact else float)
    value, end = decoder.raw_decode(text)
    if text[end:] != "\n":
        raise StrictJsonError(f"expected exactly one document followed by one newline, got trailing {text[end:]!r}")
    return value


class Run:
    """The result of one process run, with stdout decoded strictly as UTF-8."""

    def __init__(self, completed: subprocess.CompletedProcess[bytes]):
        self.returncode = completed.returncode
        self.stdout_bytes = completed.stdout
        self.stderr_bytes = completed.stderr
        self.stdout = completed.stdout.decode("utf-8")        # strict: invalid UTF-8 raises
        self.stderr = completed.stderr.decode("utf-8", errors="replace")

    def json(self, exact: bool = False) -> Any:
        return strict_loads(self.stdout, exact=exact)


def run_exe(args: list[str], env: dict[str, str] | None = None, exe: str | None = None) -> Run:
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    completed = subprocess.run([exe or EXE, *args], capture_output=True, env=full_env, check=False)
    return Run(completed)


def sma_args(fixture: str, short: str = "2", long: str = "3", symbols: str = "AAPL", *extra: str) -> list[str]:
    return ["run", "--dataset", datasets(fixture), "--strategy", "sma_crossover",
            "--param", f"short_window={short}", "--param", f"long_window={long}",
            "--param", f"symbols={symbols}", *extra]


def result_substring(stdout: str) -> str:
    """The exact bytes of ``result`` in COMPACT output: after '"result":' up to the document's closing brace."""
    marker = '"result":'
    start = stdout.index(marker) + len(marker)
    assert stdout.endswith("}\n")
    return stdout[start:-2]


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


class LabTestCase(unittest.TestCase):
    """Skips cleanly (rather than failing mysteriously) if the test was started by hand without paths."""

    needs_probe = False

    @classmethod
    def setUpClass(cls) -> None:
        if not EXE or not Path(EXE).exists():
            raise unittest.SkipTest("STRATEGY_LAB_EXE is not set to an existing strategy_lab_replay")
        if not FIXTURES.is_dir():
            raise unittest.SkipTest("STRATEGY_LAB_FIXTURES is not set to the fixture directory")
        if cls.needs_probe and (not PROBE or not Path(PROBE).exists()):
            raise unittest.SkipTest("STRATEGY_LAB_JSON_PROBE is not set to an existing strategy_lab_json_probe")


def temp_dir() -> tempfile.TemporaryDirectory[str]:
    return tempfile.TemporaryDirectory(prefix="strategy_lab_test_")


if __name__ == "__main__":
    sys.exit("labtest is a helper module; run run_tests.py")
