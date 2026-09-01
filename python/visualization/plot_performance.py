"""Performance-report visualization (OUTLINE ONLY).

This module will turn a performance export produced by the C++ engine's
analytics layer into Matplotlib charts (equity curve, drawdown, return
distribution, summary table).

It is deliberately decoupled from the engine:

* it does **not** connect to PostgreSQL,
* it does **not** talk to the running C++ process,
* it only reads a file the engine writes (JSON or CSV -- format TBD, see
  ``docs/OPEN_QUESTIONS.md``).

Every function below is a documented stub. Each raises ``NotImplementedError``
so nothing silently produces fake charts.

Intended CLI once implemented::

    python plot_performance.py --input run_42_performance.json --output charts/

Requirements: see ``requirements.txt``.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:  # import only for type checkers, never at runtime
    from matplotlib.axes import Axes
    from matplotlib.figure import Figure


@dataclass
class PerformanceData:
    """In-memory form of one exported run.

    The real field set follows ``domain::PerformanceReport`` plus the time
    series needed for charts. Placeholder shape only -- do not rely on it yet.
    """

    run_id: int = 0
    generated_at: str = ""
    summary: dict[str, float] = field(default_factory=dict)
    #: list of (timestamp, total_equity) samples -- the equity curve
    equity_curve: list[tuple[str, float]] = field(default_factory=list)
    #: list of (timestamp, signed_return) samples
    returns: list[tuple[str, float]] = field(default_factory=list)


def load_performance_export(path: str | Path) -> PerformanceData:
    """Load and validate a performance export written by the C++ engine.

    Parameters
    ----------
    path:
        Path to the export file. Format (JSON vs CSV vs several files) is not
        decided yet.

    Returns
    -------
    PerformanceData

    Raises
    ------
    FileNotFoundError
        If ``path`` does not exist (to be implemented).
    ValueError
        If the file is present but malformed / schema-mismatched.

    TODO
    ----
    * Decide the export schema with the analytics module and pin it here.
    * Parse summary metrics (total return, volatility, max drawdown, profit
      factor, trade count, ...).
    * Parse the equity-curve and per-trade/return series.
    * Validate types, monotonic timestamps, and required keys.
    """
    raise NotImplementedError(
        "load_performance_export: export schema is not defined yet "
        "(see docs/OPEN_QUESTIONS.md)"
    )


def plot_equity_curve(data: PerformanceData, ax: "Axes | None" = None) -> "Axes":
    """Plot total equity over time.

    TODO: line plot of ``data.equity_curve``; log-scale option; mark the
    high-water mark; annotate start/end equity.
    """
    raise NotImplementedError("plot_equity_curve")


def plot_drawdown(data: PerformanceData, ax: "Axes | None" = None) -> "Axes":
    """Plot the drawdown curve (equity / running-peak - 1).

    TODO: derive drawdown from ``data.equity_curve``; fill under the curve;
    annotate the maximum drawdown depth and its date range.
    """
    raise NotImplementedError("plot_drawdown")


def plot_return_distribution(
    data: PerformanceData, ax: "Axes | None" = None
) -> "Axes":
    """Histogram of per-period returns.

    TODO: histogram of ``data.returns``; overlay mean / +-1 std; optional
    normal-fit reference curve.
    """
    raise NotImplementedError("plot_return_distribution")


def render_summary_table(
    data: PerformanceData, ax: "Axes | None" = None
) -> "Axes":
    """Render ``data.summary`` as a small table axis.

    TODO: format each metric (percentages, ratios, counts) and lay it out as a
    Matplotlib table for inclusion in the report figure.
    """
    raise NotImplementedError("render_summary_table")


def build_report_figure(data: PerformanceData) -> "Figure":
    """Compose the full multi-panel report figure.

    Layout (planned): equity curve (top, wide), drawdown (middle), return
    distribution + summary table (bottom row).

    TODO: create the figure/subplots, call the plot_* helpers, tighten layout,
    return the ``Figure`` for the caller to save.
    """
    raise NotImplementedError("build_report_figure")


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render performance charts from a C++-engine export "
        "(NOT IMPLEMENTED YET)."
    )
    parser.add_argument(
        "--input",
        required=True,
        type=Path,
        help="performance export file written by the engine",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("charts"),
        help="directory to write chart images into (default: ./charts)",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="display the figure interactively instead of only saving it",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point.

    Wiring is real so ``--help`` works; the actual work is still stubbed.
    """
    args = _parse_args(argv)

    data = load_performance_export(args.input)  # raises NotImplementedError today

    # TODO: once implemented --
    #   import matplotlib
    #   matplotlib.use("Agg") unless args.show
    #   fig = build_report_figure(data)
    #   args.output.mkdir(parents=True, exist_ok=True)
    #   fig.savefig(args.output / f"run_{data.run_id}_report.png", dpi=150)
    #   if args.show: import matplotlib.pyplot as plt; plt.show()
    _ = data
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
