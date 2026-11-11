"""Writes the larger synthetic Strategy Lab datasets (tests/fixtures/strategy_lab/datasets/large_*.csv).

The files are checked in; this script is how they were made and how to remake them. It is deterministic: each dataset
has its own fixed seed and uses only random.Random (the same sequence on every platform and Python 3 version), so a
rerun rewrites the files byte for byte. Run it from anywhere:

    python tests/fixtures/strategy_lab/generate_large_datasets.py

Every series is invented. Bars are one per minute from 2026-01-05T14:30:00Z, like the small fixtures; multi-symbol files
list every symbol at each minute, in a fixed order. Closes are rounded to cents and never go below 1.00.
"""

from __future__ import annotations

import math
import random
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Callable, Iterator

OUT_DIR = Path(__file__).resolve().parent / "datasets"
START = datetime(2026, 1, 5, 14, 30, tzinfo=timezone.utc)
HEADER = "symbol,exchange_time,type,price\n"

# A path maker takes a seeded generator and yields closes forever.
Path_ = Callable[[random.Random], Iterator[float]]


def stamp(minute: int) -> str:
    return (START + timedelta(minutes=minute)).strftime("%Y-%m-%dT%H:%M:%SZ")


def cents(price: float) -> str:
    return f"{max(price, 1.0):.2f}"


def gbm(start: float, drift: float, vol: float) -> Path_:
    """Geometric Brownian motion: per-bar log return drift + vol * N(0, 1)."""
    def path(rng: random.Random) -> Iterator[float]:
        price = start
        while True:
            yield price
            price *= math.exp(drift + vol * rng.gauss(0.0, 1.0))
    return path


def ou(mean: float, speed: float, vol: float) -> Path_:
    """Ornstein-Uhlenbeck in log price: pulled back toward log(mean) by `speed` per bar."""
    def path(rng: random.Random) -> Iterator[float]:
        level, target = math.log(mean), math.log(mean)
        while True:
            yield math.exp(level)
            level += speed * (target - level) + vol * rng.gauss(0.0, 1.0)
    return path


def regimes(start: float) -> Path_:
    """Blocks of 300 to 900 bars, cycling uptrend, range, downtrend, high volatility, in that order."""
    def path(rng: random.Random) -> Iterator[float]:
        price = start
        cycle = ("up", "range", "down", "volatile")
        block = 0
        while True:
            kind = cycle[block % len(cycle)]
            anchor = math.log(price)
            for _ in range(rng.randint(300, 900)):
                yield price
                level = math.log(price)
                if kind == "up":
                    level += 0.0006 + 0.0012 * rng.gauss(0.0, 1.0)
                elif kind == "down":
                    level += -0.0006 + 0.0012 * rng.gauss(0.0, 1.0)
                elif kind == "range":
                    level += 0.05 * (anchor - level) + 0.0012 * rng.gauss(0.0, 1.0)
                else:
                    level += 0.0045 * rng.gauss(0.0, 1.0)
                price = math.exp(level)
            block += 1
    return path


def crash_and_recovery(start: float) -> Path_:
    """A calm uptrend, a 25% fall over 30 bars, a volatile bottom, then a slower recovery."""
    def path(rng: random.Random) -> Iterator[float]:
        price = start
        phases = ((2500, 0.0001, 0.0008), (30, math.log(0.75) / 30, 0.002), (700, 0.0, 0.004), (10**9, 0.00008, 0.0015))
        for bars, drift, vol in phases:
            for _ in range(bars):
                yield price
                price *= math.exp(drift + vol * rng.gauss(0.0, 1.0))
    return path


def universe(specs: dict[str, tuple[float, float, float, float]], market_vol: float, shock_at: int) -> Callable[
        [random.Random, int], list[tuple[str, list[float]]]]:
    """Symbols that share a market factor: return = drift + beta * market + idio_vol * N(0, 1). At `shock_at` the
    market falls 8% over 10 bars, so every symbol drops at once in proportion to its beta."""
    def make(rng: random.Random, bars: int) -> list[tuple[str, list[float]]]:
        prices = {symbol: spec[0] for symbol, spec in specs.items()}
        out: dict[str, list[float]] = {symbol: [] for symbol in specs}
        for bar in range(bars):
            market = market_vol * rng.gauss(0.0, 1.0)
            if shock_at <= bar < shock_at + 10:
                market += math.log(0.92) / 10
            for symbol, (_, drift, beta, idio) in specs.items():
                out[symbol].append(prices[symbol])
                prices[symbol] *= math.exp(drift + beta * market + idio * rng.gauss(0.0, 1.0))
        return list(out.items())
    return make


def independent(paths: dict[str, Path_]) -> Callable[[random.Random, int], list[tuple[str, list[float]]]]:
    def make(rng: random.Random, bars: int) -> list[tuple[str, list[float]]]:
        out = []
        for symbol, path in paths.items():
            series = path(random.Random(rng.getrandbits(64)))
            out.append((symbol, [next(series) for _ in range(bars)]))
        return out
    return make


# key -> (seed, bars per symbol, maker). Keep in step with BUILTINS in python/strategy_lab/strategy_lab_ui/datasets.py.
DATASETS = {
    "large_random_walk": (4914_01, 5_000, independent({"AAPL": gbm(180.0, 0.0, 0.0010)})),
    "large_regimes": (4914_02, 10_000, independent({"AAPL": regimes(180.0)})),
    "large_mean_reverting": (4914_03, 4_000, independent({
        "AAPL": ou(180.0, 0.03, 0.0015), "MSFT": ou(410.0, 0.02, 0.0012), "GOOG": ou(150.0, 0.05, 0.0020)})),
    "large_crash_recovery": (4914_04, 6_000, independent({"AAPL": crash_and_recovery(180.0)})),
    "large_universe": (4914_05, 3_000, universe({
        # symbol: (start, drift per bar, beta, idiosyncratic vol)
        "AAPL": (180.0, 0.00010, 1.0, 0.0008),
        "MSFT": (410.0, 0.00008, 0.9, 0.0007),
        "GOOG": (150.0, 0.00000, 1.1, 0.0010),
        "AMZN": (175.0, -0.00005, 1.2, 0.0012),
        "NVDA": (900.0, 0.00015, 1.6, 0.0018),
    }, market_vol=0.0008, shock_at=2_000)),
}


def render(series: list[tuple[str, list[float]]]) -> str:
    lines = [HEADER]
    for minute in range(len(series[0][1])):
        lines.extend(f"{symbol},{stamp(minute)},bar,{cents(closes[minute])}\n" for symbol, closes in series)
    return "".join(lines)


def main(out_dir: Path = OUT_DIR) -> None:
    for key, (seed, bars, make) in DATASETS.items():
        text = render(make(random.Random(seed), bars))
        path = out_dir / f"{key}.csv"
        path.write_bytes(text.encode("ascii"))
        print(f"{path.name}: {text.count(chr(10)) - 1} rows, {len(text)} bytes")


if __name__ == "__main__":
    main()
