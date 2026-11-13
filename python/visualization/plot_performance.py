"""Charts for one finished run, from the results file the engine writes.

    trading_engine run --bars FILE --report run.json
    python plot_performance.py --input run.json --output charts/

Reads only that file (schema ``trading_engine.results/1``, see
``include/trading_engine/analytics/results_export.hpp``); never the database
or a running engine. Writes ``<output>/<name>_report.png``: equity curve with
fills, drawdown, the distribution of per-sample returns and the summary.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from matplotlib.axes import Axes
    from matplotlib.figure import Figure

SCHEMA = "trading_engine.results/1"

_FRACTION = re.compile(r"\.(\d+)")


@dataclass
class Fill:
    time: datetime
    symbol: str
    quantity: int
    price: float
    fees: float


@dataclass
class PerformanceData:
    name: str
    mode: str
    source: str
    symbols: list[str]
    summary: dict[str, Any]
    equity_curve: list[tuple[datetime, float]] = field(default_factory=list)
    fills: list[Fill] = field(default_factory=list)
    stages: list[dict[str, Any]] = field(default_factory=list)

    @property
    def returns(self) -> list[float]:
        """Simple return between consecutive equity samples."""
        values = [equity for _, equity in self.equity_curve]
        return [b / a - 1.0 for a, b in zip(values, values[1:]) if a != 0]

    @property
    def drawdown(self) -> list[tuple[datetime, float]]:
        """Equity over its running peak, minus one (<= 0)."""
        out, peak = [], -math.inf
        for when, equity in self.equity_curve:
            peak = max(peak, equity)
            out.append((when, equity / peak - 1.0 if peak > 0 else 0.0))
        return out


def _time(text: Any, where: str) -> datetime:
    if not isinstance(text, str):
        raise ValueError(f"{where}: expected an ISO-8601 time, got {text!r}")
    # The engine writes nanoseconds; datetime keeps microseconds.
    trimmed = _FRACTION.sub(lambda m: "." + m.group(1)[:6].ljust(6, "0"), text, count=1)
    try:
        when = datetime.fromisoformat(trimmed.replace("Z", "+00:00"))
    except ValueError as e:
        raise ValueError(f"{where}: {e}") from None
    return when if when.tzinfo else when.replace(tzinfo=timezone.utc)


def _number(value: Any, where: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{where}: expected a number, got {value!r}")
    return float(value)


def load_performance_export(path: str | Path) -> PerformanceData:
    """Read and check a results file. Raises ValueError if it is not one."""
    with open(path, encoding="utf-8") as f:
        try:
            doc = json.load(f)
        except json.JSONDecodeError as e:
            raise ValueError(f"{path}: not JSON: {e}") from None
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        raise ValueError(f"{path}: not a {SCHEMA} file")
    for key, kind in (("summary", dict), ("equity_curve", list), ("fills", list)):
        if not isinstance(doc.get(key), kind):
            raise ValueError(f"{path}: '{key}' is missing or not a {kind.__name__}")

    curve: list[tuple[datetime, float]] = []
    for i, point in enumerate(doc["equity_curve"]):
        where = f"equity_curve[{i}]"
        when = _time(point.get("time"), where + ".time")
        if curve and when < curve[-1][0]:
            raise ValueError(f"{where}.time: goes back in time")
        curve.append((when, _number(point.get("equity"), where + ".equity")))
    if not curve:
        raise ValueError(f"{path}: the equity curve is empty")

    fills = []
    for i, f in enumerate(doc["fills"]):
        where = f"fills[{i}]"
        fills.append(Fill(
            time=_time(f.get("time"), where + ".time"),
            symbol=str(f.get("symbol", "")),
            quantity=int(_number(f.get("quantity"), where + ".quantity")),
            price=_number(f.get("price"), where + ".price"),
            fees=_number(f.get("fees", 0), where + ".fees"),
        ))

    return PerformanceData(
        name=str(doc.get("name", "run")),
        mode=str(doc.get("mode", "")),
        source=str(doc.get("source", "")),
        symbols=[str(s) for s in doc.get("symbols", [])],
        summary=doc["summary"],
        equity_curve=curve,
        fills=fills,
        stages=list(doc.get("stages", [])),
    )


# --- charts ----------------------------------------------------------------

_INK = "#1f2933"
_MUTED = "#7b8794"
_GRID = "#e4e7eb"
_LINE = "#2563eb"
_LOSS = "#c2410c"
_BUY = "#15803d"
_SELL = "#b91c1c"


def _axes(ax: "Axes | None") -> "Axes":
    if ax is None:
        import matplotlib.pyplot as plt

        _, ax = plt.subplots()
    ax.grid(True, color=_GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(_MUTED)
    ax.tick_params(colors=_INK, labelsize=8)
    return ax


def _equity_at(data: PerformanceData, when: datetime) -> float:
    equity = data.equity_curve[0][1]
    for t, e in data.equity_curve:
        if t > when:
            break
        equity = e
    return equity


def plot_equity_curve(data: PerformanceData, ax: "Axes | None" = None) -> "Axes":
    """Total equity over time, with each fill marked (up: buy, down: sell)."""
    ax = _axes(ax)
    times = [t for t, _ in data.equity_curve]
    values = [e for _, e in data.equity_curve]
    ax.plot(times, values, color=_LINE, linewidth=1.4)
    ax.axhline(values[0], color=_MUTED, linewidth=0.8, linestyle="--")
    for side, color, marker in (("buy", _BUY, "^"), ("sell", _SELL, "v")):
        picked = [f for f in data.fills if (f.quantity > 0) == (side == "buy")]
        if picked:
            ax.scatter([f.time for f in picked], [_equity_at(data, f.time) for f in picked],
                       color=color, marker=marker, s=22, zorder=3, label=f"{side} ({len(picked)})")
    if data.fills:
        ax.legend(loc="upper left", fontsize=8, frameon=False)
    ax.set_title("Equity", loc="left", fontsize=10, color=_INK)
    ax.yaxis.set_major_formatter(_money_formatter())
    return ax


def plot_drawdown(data: PerformanceData, ax: "Axes | None" = None) -> "Axes":
    """Drawdown from the running peak, with the deepest point marked."""
    ax = _axes(ax)
    series = data.drawdown
    times = [t for t, _ in series]
    depth = [d * 100.0 for _, d in series]
    ax.fill_between(times, depth, 0.0, color=_LOSS, alpha=0.25, linewidth=0)
    ax.plot(times, depth, color=_LOSS, linewidth=1.0)
    worst = min(range(len(depth)), key=depth.__getitem__)
    if depth[worst] < 0:
        ax.annotate(f"{depth[worst]:.3f}%", (times[worst], depth[worst]), xytext=(4, -10),
                    textcoords="offset points", fontsize=8, color=_LOSS)
    ax.set_title("Drawdown (%)", loc="left", fontsize=10, color=_INK)
    ax.set_ylim(top=0.0)
    return ax


def plot_return_distribution(data: PerformanceData, ax: "Axes | None" = None) -> "Axes":
    """Histogram of per-sample returns in basis points, with the mean."""
    ax = _axes(ax)
    bps = [r * 1e4 for r in data.returns]
    if bps:
        ax.hist(bps, bins=min(40, max(5, len(bps) // 4)), color=_LINE, alpha=0.75)
        mean = sum(bps) / len(bps)
        ax.axvline(mean, color=_INK, linewidth=1.0, linestyle="--", label=f"mean {mean:.2f} bp")
        ax.legend(loc="upper right", fontsize=8, frameon=False)
    ax.set_title("Returns per sample (bp)", loc="left", fontsize=10, color=_INK)
    return ax


def _percent(value: Any) -> str:
    return "-" if value is None else f"{value * 100:.2f}%"


def _ratio(value: Any) -> str:
    return "-" if value is None else f"{value:.2f}"


def _money(value: Any) -> str:
    return "-" if value is None else f"{value:,.2f}"


def summary_rows(data: PerformanceData) -> list[tuple[str, str]]:
    s = data.summary
    return [
        ("Starting equity", _money(s.get("starting_equity"))),
        ("Ending equity", _money(s.get("ending_equity"))),
        ("Total return", _percent(s.get("total_return"))),
        ("Max drawdown", _percent(s.get("max_drawdown"))),
        ("Volatility (ann.)", _percent(s.get("volatility"))),
        ("Sharpe (ann.)", _ratio(s.get("sharpe"))),
        ("Profit factor", _ratio(s.get("profit_factor"))),
        ("Win rate", _percent(s.get("win_rate"))),
        ("Trades / fills", f"{s.get('trade_count', 0)} / {s.get('fill_count', 0)}"),
        ("Fees", _money(s.get("fees_paid"))),
    ]


def render_summary_table(data: PerformanceData, ax: "Axes | None" = None) -> "Axes":
    """The summary metrics as a two-column table."""
    if ax is None:
        import matplotlib.pyplot as plt

        _, ax = plt.subplots()
    ax.axis("off")
    table = ax.table(cellText=summary_rows(data), colLoc="left", cellLoc="left", loc="upper left",
                     colWidths=[0.6, 0.4])
    table.auto_set_font_size(False)
    table.set_fontsize(8.5)
    for (_, col), cell in table.get_celld().items():
        cell.set_edgecolor(_GRID)
        cell.set_text_props(color=_INK, ha="right" if col == 1 else "left")
        cell.set_height(0.085)
    ax.set_title("Summary", loc="left", fontsize=10, color=_INK)
    return ax


def build_report_figure(data: PerformanceData) -> "Figure":
    """Equity (top), drawdown (middle), returns and summary (bottom)."""
    import matplotlib.pyplot as plt

    fig = plt.figure(figsize=(11, 8.5), constrained_layout=True)
    grid = fig.add_gridspec(3, 2, height_ratios=[2.2, 1.1, 1.6])
    from matplotlib.dates import AutoDateLocator, ConciseDateFormatter

    equity = fig.add_subplot(grid[0, :])
    plot_equity_curve(data, equity)
    drawdown = plot_drawdown(data, fig.add_subplot(grid[1, :], sharex=equity))
    locator = AutoDateLocator()
    drawdown.xaxis.set_major_locator(locator)
    drawdown.xaxis.set_major_formatter(ConciseDateFormatter(locator))
    equity.tick_params(labelbottom=False)
    plot_return_distribution(data, fig.add_subplot(grid[2, 0]))
    render_summary_table(data, fig.add_subplot(grid[2, 1]))
    s = data.summary
    period = f"{str(s.get('period_start', ''))[:16]} to {str(s.get('period_end', ''))[:16]}".replace("T", " ")
    fig.suptitle(f"{data.name}  ·  {data.mode}  ·  {data.source}  ·  {period} UTC",
                 x=0.01, ha="left", fontsize=11, color=_INK)
    return fig


def _money_formatter():
    from matplotlib.ticker import FuncFormatter

    return FuncFormatter(lambda v, _: f"{v:,.0f}")


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Chart a run's results file (trading_engine run --report).")
    parser.add_argument("--input", required=True, type=Path, help="results file written by the engine")
    parser.add_argument("--output", type=Path, default=Path("charts"),
                        help="directory for the PNG (default: ./charts)")
    parser.add_argument("--show", action="store_true", help="also open the figure in a window")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        data = load_performance_export(args.input)
    except (OSError, ValueError) as e:
        print(f"plot_performance: {e}")
        return 1

    import matplotlib

    if not args.show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig = build_report_figure(data)
    args.output.mkdir(parents=True, exist_ok=True)
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", data.name) or "run"
    out = args.output / f"{safe}_report.png"
    fig.savefig(out, dpi=150)
    print(out)
    if args.show:
        plt.show()
    plt.close(fig)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
