"""Plotly figures for Explore and Compare. Pure functions: models, a cursor and a filter in, a Figure out.

Only the visible prefix is drawn. Every value comes from the tool's output: closes from events, averages / mean / z from
`indicators` (a missing value is a gap, never zero), Buy/Sell markers from `signals` placed at the signalling event,
thresholds from the run's own configuration. Python computes nothing about decisions. Signals are distinguished by shape
AND text, not colour alone; indicators by colour AND line style. Colours come from `theme.py` (dark palette).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping, Sequence

import plotly.graph_objects as go
from plotly.subplots import make_subplots

from . import theme
from .replay import ReplayModel, SymbolSeries
from .schema import Event, StrategyConfig, exact_text

MAX_SYMBOL_PANELS = 4
MAX_COMPARE_SYMBOLS = 2

PRICE, BUY, SELL, MUTED = theme.PRICE, theme.BUY, theme.SELL, theme.TEXT_MUTED
Z = theme.ZSCORE

# Which recorded indicators to draw, by strategy kind. A drawing choice, not logic: the values are the tool's.
OVERLAYS: Mapping[str, tuple[str, ...]] = {"sma_crossover": ("short_sma", "long_sma"), "mean_reversion": ("mean",)}
ZPANEL: Mapping[str, str] = {"mean_reversion": "z_score"}
# colour, dash, width: distinguishable by style as well as hue.
_LINE_STYLE = {"short_sma": (theme.SHORT_SMA, "solid", 2.4), "long_sma": (theme.LONG_SMA, "dash", 2.2),
               "mean": (theme.MEAN, "dashdot", 2.2)}


def indicator_names(kind: str) -> tuple[str, ...]:
    names = list(OVERLAYS.get(kind, ()))
    if kind in ZPANEL:
        names.append(ZPANEL[kind])
    return tuple(names)


def _param(config: StrategyConfig, name: str) -> Any:
    return config.parameters.get(name)


def _line_labels(config: StrategyConfig) -> dict[str, str]:
    return {
        "short_sma": f"Short SMA ({_param(config, 'short_window')})",
        "long_sma": f"Long SMA ({_param(config, 'long_window')})",
        "mean": f"Rolling mean (lookback {_param(config, 'lookback')})",
    }


def _hover(model: ReplayModel, series: SymbolSeries) -> list[str]:
    out = []
    for position, index in enumerate(series.event_indexes):
        event = model.events[index]
        price = "no price" if event.price is None else f"close {exact_text(event.price)}"
        decision = f"{series.verdicts[position]}: {series.reasons[position]}" if series.verdicts[position] else "no diagnostics"
        out.append(f"Event {index + 1} of {model.total} - {event.symbol}<br>{event.exchange_time}<br>"
                   f"{event.type}, {price}<br>{decision}")
    return out


def _style(fig: go.Figure, *, height: int, margin: dict[str, int]) -> None:
    """The dark look shared by every figure: backgrounds, axes, grids, legend, hover labels, toolbar."""
    fig.update_xaxes(gridcolor=theme.GRID, linecolor=theme.AXIS, zerolinecolor=theme.AXIS, tickfont=dict(color=MUTED),
                     title_font=dict(color=MUTED))
    fig.update_yaxes(gridcolor=theme.GRID, linecolor=theme.AXIS, zerolinecolor=theme.AXIS, tickfont=dict(color=MUTED),
                     title_font=dict(color=MUTED))
    fig.update_layout(
        template="plotly_dark", height=height, margin=margin, hovermode="closest",
        paper_bgcolor=theme.BACKGROUND, plot_bgcolor=theme.SURFACE, font=dict(size=13, color=theme.TEXT),
        legend=dict(orientation="h", yanchor="bottom", y=1.04, xanchor="left", x=0,
                    font=dict(size=12, color=theme.TEXT), bgcolor="rgba(0,0,0,0)"),
        hoverlabel=dict(bgcolor=theme.SURFACE_RAISED, bordercolor=theme.BORDER, font=dict(color=theme.TEXT, size=13)),
        modebar=dict(bgcolor="rgba(0,0,0,0)", color=MUTED, activecolor=theme.TEXT))


def _empty(message: str) -> go.Figure:
    fig = go.Figure()
    fig.update_xaxes(visible=False)
    fig.update_yaxes(visible=False)
    fig.add_annotation(text=message, x=0.5, y=0.5, xref="paper", yref="paper", showarrow=False,
                       font=dict(size=16, color=MUTED))
    _style(fig, height=360, margin=dict(l=20, r=20, t=20, b=20))
    return fig


def _marker_traces(fig: go.Figure, series: SymbolSeries, row: int, first: bool, y_values: list[float | None],
                   hover: list[str]) -> None:
    """Evaluated / warming up / ignored / trade-row markers, so every revealed row is accounted for. Their legend entries
    are shared by every member of a figure: one entry toggles the same marker kind in all panels."""
    styles = (
        ("Evaluated bar", lambda i: series.types[i] == "bar" and series.verdicts[i] == "evaluated",
         dict(symbol="circle", size=6, color=PRICE)),
        ("Warming up (no indicators yet)", lambda i: series.types[i] == "bar" and series.verdicts[i] == "warming_up",
         dict(symbol="circle-open", size=9, color=MUTED, line=dict(width=2, color=MUTED))),
        ("Ignored by the strategy", lambda i: series.types[i] == "bar" and series.verdicts[i] == "ignored",
         dict(symbol="x", size=9, color=MUTED, line=dict(width=2, color=MUTED))),
        ("Trade row (strategies ignore it)", lambda i: series.types[i] != "bar",
         dict(symbol="diamond-open", size=9, color=MUTED, line=dict(width=2, color=MUTED))),
    )
    for name, keep, marker in styles:
        positions = [i for i in range(len(series.event_indexes)) if keep(i) and y_values[i] is not None]
        if not positions:
            continue
        fig.add_trace(go.Scatter(
            x=[series.times[i] for i in positions], y=[y_values[i] for i in positions], mode="markers",
            name=name, legendgroup=name, showlegend=first, marker=marker,
            text=[hover[i] for i in positions], hoverinfo="text"), row=row, col=1)


def _signal_traces(fig: go.Figure, model: ReplayModel, symbol: str, cursor: int, row: int, first: bool,
                   y_of_event, prefix: str) -> None:
    for side, label, shape, color, position in (("buy", "Buy request", "triangle-up", BUY, "top center"),
                                                ("sell", "Sell request", "triangle-down", SELL, "bottom center")):
        picked = [s for s in model.signals_through(cursor, symbol) if s.side == side]
        points = [(s, y_of_event(s.event_index)) for s in picked]
        points = [(s, y) for s, y in points if y is not None]
        if not points:
            continue
        fig.add_trace(go.Scatter(
            x=[model.time_text(model.events[s.event_index]) for s, _ in points], y=[y for _, y in points],
            mode="markers+text", name=label, legendgroup=label, showlegend=first,
            text=["Buy" if side == "buy" else "Sell"] * len(points), textposition=position,
            textfont=dict(color=color, size=12),
            marker=dict(symbol=shape, size=15, color=color, line=dict(width=1.5, color=theme.BACKGROUND)),
            customdata=[[s.signal_id, s.event_index + 1, s.created_at] for s, _ in points],
            hovertemplate=(prefix + label + " #%{customdata[0]}<br>event %{customdata[1]}<br>%{customdata[2]}"
                           "<extra></extra>")), row=row, col=1)


def _selected_trace(fig: go.Figure, model: ReplayModel, selected: Event | None, symbol: str, row: int,
                    first: bool, y: float | None, prefix: str) -> None:
    if selected is None or selected.symbol != symbol or y is None:
        return
    fig.add_trace(go.Scatter(
        x=[model.time_text(selected)], y=[y], mode="markers", name="Selected event",
        legendgroup="selected", showlegend=first,
        marker=dict(symbol="circle-open", size=22, color=theme.TEXT, line=dict(width=2.5, color=theme.TEXT)),
        hoverinfo="skip"), row=row, col=1)


def _threshold_lines(fig: go.Figure, config: StrategyConfig, row: int) -> None:
    entry, rearm = _param(config, "entry_threshold"), _param(config, "rearm_threshold")
    for value, text, dash, color in ((entry, "entry", "dash", theme.WARN), (rearm, "re-arm", "dot", MUTED)):
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            for sign in (1, -1):
                fig.add_hline(y=sign * value, row=row, col=1, line=dict(color=color, width=1.4, dash=dash),
                              annotation_text=f"{text} {'+' if sign > 0 else '-'}{value:g}",
                              annotation_position="top right" if sign > 0 else "bottom right",
                              annotation_font=dict(size=11, color=color))


@dataclass(frozen=True)
class Member:
    """One strategy run to draw: its model, configuration and the text put before its trace names ('' for Explore)."""

    model: ReplayModel
    config: StrategyConfig
    prefix: str = ""
    title: str = ""


def _panels(config: StrategyConfig) -> int:
    return 2 if config.kind in ZPANEL else 1


def _draw(fig: go.Figure, member: Member, sym: str, cursor: int, selected: Event | None, price_row: int,
          first: bool, lead: bool) -> None:
    """first: this is the first drawn symbol (only it gets legend entries). lead: this is the first member (only it gets
    the entries every member shares: the close line, the markers, Buy/Sell and the selected ring)."""
    model, config, prefix = member.model, member.config, member.prefix
    shared = first and lead
    names = indicator_names(config.kind)
    series = model.series(sym, cursor, names)
    if not series.event_indexes:
        row_suffix = "" if price_row == 1 else str(price_row)
        fig.add_annotation(text=f"No {sym} rows revealed yet", x=0.5, y=0.5, showarrow=False,
                           xref=f"x{row_suffix} domain", yref=f"y{row_suffix} domain", font=dict(color=MUTED))
        return
    hover = _hover(model, series)
    positions = [i for i in range(len(series.event_indexes)) if series.types[i] == "bar" and series.prices[i] is not None]
    fig.add_trace(go.Scatter(x=[series.times[i] for i in positions], y=[series.prices[i] for i in positions], mode="lines",
                             name="Close", legendgroup="Close", showlegend=shared,
                             line=dict(color=PRICE, width=1.6), hoverinfo="skip"), row=price_row, col=1)
    _marker_traces(fig, series, price_row, shared, series.prices, hover)
    labels = _line_labels(config)
    for name in OVERLAYS.get(config.kind, ()):
        color, dash, width = _LINE_STYLE[name]
        fig.add_trace(go.Scatter(x=series.times, y=series.indicators[name], mode="lines", name=prefix + labels[name],
                                 legendgroup=prefix + labels[name], showlegend=first, connectgaps=False,
                                 line=dict(color=color, width=width, dash=dash), hoverinfo="skip"),
                      row=price_row, col=1)
    price_at = {ix: series.prices[p] for p, ix in enumerate(series.event_indexes)}
    _signal_traces(fig, model, sym, cursor, price_row, shared, price_at.get, prefix)
    _selected_trace(fig, model, selected, sym, price_row, shared, price_at.get(selected.index) if selected else None, prefix)
    fig.update_yaxes(title_text="Close", row=price_row, col=1)

    if config.kind in ZPANEL:
        z_row = price_row + 1
        zs = series.indicators[ZPANEL[config.kind]]
        fig.add_trace(go.Scatter(x=series.times, y=zs, mode="lines+markers", name=prefix + "z-score",
                                 legendgroup=prefix + "z", showlegend=first, connectgaps=False,
                                 line=dict(color=Z, width=2), marker=dict(size=6, color=Z), text=hover, hoverinfo="text"),
                      row=z_row, col=1)
        _threshold_lines(fig, config, z_row)
        z_at = {ix: zs[p] for p, ix in enumerate(series.event_indexes)}
        _signal_traces(fig, model, sym, cursor, z_row, False, z_at.get, prefix)
        _selected_trace(fig, model, selected, sym, z_row, False, z_at.get(selected.index) if selected else None, prefix)
        fig.update_yaxes(title_text="z-score", row=z_row, col=1, zeroline=True, zerolinecolor=theme.AXIS)


def _build(members: Sequence[Member], shown: Sequence[str], cursor: int, selected: Event | None, *,
           unit: int) -> go.Figure:
    """One figure, one column of panels: for each shown symbol, each member's price panel (and z panel)."""
    heights: list[float] = []
    titles: list[str] = []
    slots: list[tuple[str, Member, int, bool, bool]] = []
    row = 1
    for n, sym in enumerate(shown):
        for k, member in enumerate(members):
            per = _panels(member.config)
            heights += [0.62, 0.38] if per == 2 else [1.0]
            lead = f"{member.title} - " if member.title else ""
            names = indicator_names(member.config.kind)
            titles += [f"{lead}{sym}: close" + (" and indicators" if names else "")]
            if per == 2:
                titles.append(f"{lead}{sym}: z-score")
            slots.append((sym, member, row, n == 0, k == 0))
            row += per
    fig = make_subplots(rows=row - 1, cols=1, shared_xaxes=True, vertical_spacing=0.07, row_heights=heights,
                        subplot_titles=titles)
    fig.update_annotations(font=dict(color=theme.TEXT, size=13))        # the panel titles, before any other annotation
    for sym, member, price_row, first, lead in slots:
        _draw(fig, member, sym, cursor, selected, price_row, first, lead)
    fig.update_xaxes(type="date", showgrid=True)
    fig.update_xaxes(title_text="Exchange time (UTC)", row=row - 1, col=1)
    fig.update_yaxes(showgrid=True)
    _style(fig, height=60 + sum(int(h * unit) + 40 for h in heights), margin=dict(l=60, r=30, t=70, b=50))
    return fig


def build_figure(model: ReplayModel, config: StrategyConfig, cursor: int, symbol: str | None,
                 selected: Event | None) -> go.Figure:
    if cursor <= 0:
        return _empty(f"Replay position 0 of {model.total}: nothing revealed yet.<br>"
                      "Use Next bar or Next signal, or drag the timeline.")
    shown = [symbol] if symbol is not None else list(model.symbols)[:MAX_SYMBOL_PANELS]
    return _build([Member(model, config)], shown, cursor, selected, unit=420)


def build_compare_figure(members: Sequence[Member], symbols: Sequence[str], cursor: int, symbol: str | None,
                         selected: Event | None, total: int) -> go.Figure:
    """Both members' panels in ONE figure with shared x axes, so a zoom or pan moves them together. The same cursor
    and display filter apply to every panel; each member's traces are named with its label."""
    if cursor <= 0:
        return _empty(f"Replay position 0 of {total}: nothing revealed yet.<br>"
                      "Use Next bar or Next signal, or drag the timeline.")
    shown = [symbol] if symbol is not None else list(symbols)[:MAX_COMPARE_SYMBOLS]
    return _build(members, shown, cursor, selected, unit=330)


def panel_note(model: ReplayModel, symbol: str | None) -> str | None:
    if symbol is None and len(model.symbols) > MAX_SYMBOL_PANELS:
        return (f"The chart shows the first {MAX_SYMBOL_PANELS} of {len(model.symbols)} symbols. "
                "Choose a symbol to see another one.")
    return None


def compare_panel_note(symbols: Sequence[str], symbol: str | None) -> str | None:
    if symbol is None and len(symbols) > MAX_COMPARE_SYMBOLS:
        return (f"The chart shows the first {MAX_COMPARE_SYMBOLS} of {len(symbols)} symbols (each with both configurations). "
                "Choose a symbol to see another one; counts and tables always cover the display filter.")
    return None
