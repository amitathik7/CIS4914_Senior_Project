"""Where a run's bars come from: the repository's synthetic fixtures, or an uploaded CSV.

Only identity and basic sanity are checked here (not empty, not huge, looks like text). The
replay tool is the authority on whether a CSV is valid and says so, line and column, itself.
The one thing parsed here is the symbol column of the header-led CSV, only to pre-fill the
allowlist; failure to parse simply yields no suggestion.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Mapping

from .bridge import REPO_ROOT
from .errors import LabUiError

MAX_UPLOAD_BYTES = 8 * 1024 * 1024
FIXTURE_DIR = REPO_ROOT / "tests" / "fixtures" / "strategy_lab" / "datasets"

SYNTHETIC = ("Synthetic fixture checked into the repository (tests/fixtures/strategy_lab/datasets). "
             "Invented bars, one per minute from 2026-01-05T14:30:00Z; not market data.")
UPLOADED = "Uploaded in this browser session. Provenance unknown: the lab cannot tell where these bars came from."


@dataclass(frozen=True)
class BuiltinDataset:
    key: str
    label: str
    description: str
    # strategy kind -> parameter texts that make this fixture show something. Demo presets, not recommendations.
    presets: Mapping[str, Mapping[str, str]]

    @property
    def filename(self) -> str:
        return f"{self.key}.csv"


def _preset(sma: tuple[str, str], mr: tuple[str, str, str], symbols: str) -> dict[str, dict[str, str]]:
    return {
        "sma_crossover": {"short_window": sma[0], "long_window": sma[1], "symbols": symbols},
        "mean_reversion": {"lookback": mr[0], "entry_threshold": mr[1], "rearm_threshold": mr[2], "symbols": symbols},
    }


# The parameter pairs are the ones the lab's own tests use for each fixture (tests/strategy_lab).
BUILTINS: tuple[BuiltinDataset, ...] = (
    BuiltinDataset("sma_crossover", "SMA crossover demo (AAPL, 9 bars)",
                   "Closes 3 2 1 2 3 4 3 2 1: with SMA 2/3 one Buy request (event 5) and one Sell request (event 8).",
                   _preset(("2", "3"), ("4", "1.5", "0.5"), "AAPL")),
    BuiltinDataset("sma_oscillation", "Whipsaw (AAPL, 10 bars)",
                   "Closes alternate 1, 2, 1, 2...: with SMA 1/2 the averages cross on every bar once warm.",
                   _preset(("1", "2"), ("3", "1", "0.25"), "AAPL")),
    BuiltinDataset("mr_rearm", "Mean reversion: re-arm (AAPL, 7 bars)",
                   "Closes 10 10 10 6 9 2 30: a dip (Buy), a partial recovery (re-arm), a second dip (Buy), a spike (Sell).",
                   _preset(("2", "3"), ("4", "1.5", "0.5"), "AAPL")),
    BuiltinDataset("mr_suppression", "Mean reversion: suppression (AAPL, 10 bars)",
                   "A drop that stays low: one Buy, the repeats are suppressed, a constant window re-arms, then a spike Sells.",
                   _preset(("2", "3"), ("4", "1.5", "0.5"), "AAPL")),
    BuiltinDataset("constant_price", "Constant price (AAPL, 30 bars)",
                   "A flat series: equal averages and a constant window, so no signal ever (a valid run with nothing to show).",
                   _preset(("5", "20"), ("20", "2", "0.5"), "AAPL")),
    BuiltinDataset("warmup_boundary", "Warm-up boundary (AAPL, 6 bars)",
                   "Five equal closes then a jump: whether the window is full when the jump arrives decides everything.",
                   _preset(("2", "6"), ("6", "2", "0.5"), "AAPL")),
    BuiltinDataset("two_symbols_interleaved", "Two symbols interleaved (AAPL, MSFT, 18 rows)",
                   "Two series with the same timestamps, interleaved row by row; each symbol is decided on its own.",
                   _preset(("2", "3"), ("4", "1.2", "0.5"), "AAPL,MSFT")),
    BuiltinDataset("tie_aapl_first", "Equal timestamps: AAPL listed first (6 rows)",
                   "Both symbols sell at the same timestamp; the file's row order decides which request is first.",
                   _preset(("1", "2"), ("2", "0.9", "0.1"), "AAPL,MSFT")),
    BuiltinDataset("tie_msft_first", "Equal timestamps: MSFT listed first (6 rows)",
                   "The same data with MSFT listed first at each timestamp.",
                   _preset(("1", "2"), ("2", "0.9", "0.1"), "MSFT,AAPL")),
    BuiltinDataset("bars_and_trades", "Bars with trade rows (AAPL, 5 rows)",
                   "Trade rows sit between the bars; the strategies ignore them and say so.",
                   _preset(("1", "2"), ("2", "0.9", "0.1"), "AAPL")),
    BuiltinDataset("ohlcv_example", "Bars with open/high/low/volume (AAPL, 3 bars)",
                   "Optional OHLCV columns are carried through; the strategies use only the close.",
                   _preset(("1", "2"), ("3", "1", "0.25"), "AAPL")),
)


def builtin(key: str) -> BuiltinDataset:
    for item in BUILTINS:
        if item.key == key:
            return item
    raise KeyError(key)


@dataclass(frozen=True)
class DatasetSource:
    kind: str                                  # "builtin" | "upload"
    key: str                                   # fixture key, or the uploaded file's name
    display_name: str
    sha256: str
    size: int
    provenance: str
    synthetic: bool
    symbols: tuple[str, ...]                   # a suggestion for the allowlist, best effort
    data: bytes = field(repr=False, compare=False, default=b"")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def detect_symbols(data: bytes, limit_rows: int = 200_000, limit_symbols: int = 64) -> tuple[str, ...]:
    """Distinct values of the `symbol` column in first-seen order. Never raises."""
    try:
        lines = data.decode("utf-8-sig", errors="replace").splitlines()
        header = [cell.strip() for cell in lines[0].split(",")] if lines else []
        column = header.index("symbol")
    except (ValueError, IndexError):
        return ()
    seen: dict[str, None] = {}
    for line in lines[1:limit_rows + 1]:
        cells = line.split(",")
        if column < len(cells) and cells[column]:
            seen.setdefault(cells[column], None)
            if len(seen) >= limit_symbols:
                break
    return tuple(seen)


def load_builtin(key: str, directory: Path = FIXTURE_DIR) -> DatasetSource:
    item = builtin(key)
    path = directory / item.filename
    try:
        data = path.read_bytes()
    except OSError as error:
        raise LabUiError("input_missing", f"The fixture {item.filename} could not be read from {directory}.",
                         detail=str(error)) from error
    return DatasetSource("builtin", item.key, item.filename, sha256_hex(data), len(data), SYNTHETIC, True,
                         detect_symbols(data), data)


def from_upload(name: str, data: bytes) -> DatasetSource:
    shown = name or "upload.csv"
    if not data:
        raise LabUiError("input_empty", f"'{shown}' is empty (0 bytes).")
    if len(data) > MAX_UPLOAD_BYTES:
        raise LabUiError("input_too_large",
                         f"'{shown}' is {len(data) / 1048576:.1f} MiB; the limit is {MAX_UPLOAD_BYTES // 1048576} MiB.")
    if b"\x00" in data[:65536]:
        raise LabUiError("input_not_text", f"'{shown}' contains NUL bytes, so it is not a CSV text file.")
    return DatasetSource("upload", shown, shown, sha256_hex(data), len(data), UPLOADED, False,
                         detect_symbols(data), data)


def symbols_param(text: str) -> str:
    """The allowlist text typed by the user, as the tool's `symbols=` value.

    Spaces and tabs around each comma-separated entry are ignored; nothing else is changed: case
    is kept and an empty entry stays empty so the tool can reject it."""
    return ",".join(part.strip(" \t") for part in text.split(","))
