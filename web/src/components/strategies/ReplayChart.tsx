import { useMemo, useState, type KeyboardEvent, type ReactNode } from "react";
import { etClock, etDay, etFull } from "../../lib/exactTime";
import { lowerBound, resultOf, revealed, selectedEvent, trackFor, type ReplayModel, type SymbolTrack } from "../../strategies/cursor";
import { useElementWidth } from "../charts/useElementWidth";

// The strategy chart. It is SVG (like BarChart) rather than lightweight-charts because the lab's timestamps can be equal across
// symbols and finer than a second, which a time axis cannot represent: here the x axis is the RECORDED ROW ORDER of one symbol,
// so equal timestamps stay distinct. Only the revealed prefix is drawn; the x range is fixed to the symbol's row count so the
// picture does not rescale as the replay steps. Every price is a plotting coordinate (cursor.plotNumber); hovers show the
// exact recorded text. A value that does not exist is a gap, never zero.

const MAX_PANELS = 4;
export const MARKER_LIMIT = 600; // above this many revealed rows only requests and the selected row get a marker
export const PAD = { top: 26, right: 60, bottom: 14, left: 10 }; // room above and below for the Buy / Sell markers and their labels

interface Overlay {
  name: string;
  label: string;
  tone: "accent" | "warn";
  dashed?: boolean;
}
interface Band {
  param: string;
  text: string;
  value: number;
  kind: "entry" | "rearm";
}
export interface Layout {
  overlays: Overlay[];
  pane?: { name: string; label: string; bands: Band[] };
}

/** How one side of a comparison is drawn: its own colour AND its own dash patterns, so the two sides differ without colour. */
export interface ChartStyle {
  /** A CSS colour (a token): the side's averages, rolling mean and z-score. */
  color: string;
  /** Dash patterns for the first and second price overlay (undefined: solid). */
  dashes: [string | undefined, string | undefined];
  /** Dash pattern of the z-score line. */
  zDash?: string;
  /** The ring around the selected row. */
  selected?: string;
}

export function layoutFor(model: ReplayModel): Layout {
  const run = model.doc.strategies[model.slot]!;
  const text = (name: string) => run.parameters.find((p) => p.name === name)?.text;
  const bars = (name: string) => (text(name) ? ` (${text(name)} bars)` : "");
  if (model.kind === "sma_crossover") {
    return { overlays: [
      { name: "short_sma", label: `Short average${bars("short_window")}`, tone: "accent" },
      { name: "long_sma", label: `Long average${bars("long_window")}`, tone: "warn", dashed: true },
    ] };
  }
  if (model.kind === "mean_reversion") {
    const bands: Band[] = [];
    for (const [param, kind] of [["entry_threshold", "entry"], ["rearm_threshold", "rearm"]] as const) {
      const value = run.parameterNumbers[param];
      if (value !== undefined) bands.push({ param, text: text(param) ?? String(value), value, kind });
    }
    return { overlays: [{ name: "mean", label: `Rolling mean${bars("lookback")}`, tone: "warn", dashed: true }], pane: { name: "z_score", label: "z-score", bands } };
  }
  return { overlays: [] };
}

function niceTicks(min: number, max: number, count = 4): { ticks: number[]; step: number } {
  const span = max - min || 1;
  const raw = span / count;
  const magnitude = 10 ** Math.floor(Math.log10(raw));
  const step = ([1, 2, 2.5, 5, 10].find((m) => m * magnitude >= raw) ?? 10) * magnitude;
  const ticks: number[] = [];
  for (let t = Math.ceil(min / step) * step; t <= max + step * 1e-9; t += step) ticks.push(Math.abs(t) < step * 1e-9 ? 0 : t);
  return { ticks, step };
}
const decimalsFor = (step: number) => Math.min(6, Math.max(0, -Math.floor(Math.log10(step) + 1e-9)));

function domain(values: (number | null)[], pad = 0.08): [number, number] {
  let lo = Infinity;
  let hi = -Infinity;
  for (const v of values) {
    if (v === null) continue;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (!Number.isFinite(lo)) return [0, 1];
  if (lo === hi) return [lo - (Math.abs(lo) * 0.01 || 1), hi + (Math.abs(hi) * 0.01 || 1)];
  const p = (hi - lo) * pad;
  return [lo - p, hi + p];
}

/** A path through the values that skips rows which carry no value by design (ignored rows, trades) but breaks where a value is missing on an evaluated row. */
function pathFor(track: SymbolTrack, values: readonly (number | null)[], shown: number, x: (i: number) => number, y: (v: number) => number, barsOnly: boolean): string {
  let d = "";
  let pen = false;
  for (let i = 0; i < shown; i++) {
    if (barsOnly && track.types[i] !== "bar") continue;
    const v = values[i];
    if (v === null || v === undefined) {
      if (track.verdicts[i] === "ignored" || track.types[i] !== "bar") continue;
      pen = false;
      continue;
    }
    d += `${pen ? "L" : "M"}${x(i).toFixed(1)},${y(v).toFixed(1)}`;
    pen = true;
  }
  return d;
}

const TONE = { accent: "var(--accent)", warn: "var(--warn)" } as const;

function Marker({ x, y, side, label, verdict, type }: { x: number; y: number; side: string | null; label: boolean; verdict: string | undefined; type: string }) {
  if (side === "buy") return <g><path d={`M${x},${y + 5} l-5.5,9 h11 z`} fill="var(--up)" />{label && <text x={x} y={y + 25} textAnchor="middle" fill="var(--up)">Buy</text>}</g>;
  if (side === "sell") return <g><path d={`M${x},${y - 5} l-5.5,-9 h11 z`} fill="var(--down)" />{label && <text x={x} y={y - 18} textAnchor="middle" fill="var(--down)">Sell</text>}</g>;
  if (type !== "bar") return <path d={`M${x},${y - 3.5} l3.5,3.5 l-3.5,3.5 l-3.5,-3.5 z`} fill="none" stroke="var(--text-3)" strokeWidth={1.2} />;
  if (verdict === "warming_up") return <circle cx={x} cy={y} r={3} fill="var(--surface)" stroke="var(--text-3)" strokeWidth={1.3} />;
  if (verdict === "ignored") return <path d={`M${x - 3},${y - 3} l6,6 M${x + 3},${y - 3} l-6,6`} stroke="var(--text-3)" strokeWidth={1.3} />;
  if (verdict === undefined) return <rect x={x - 2.5} y={y - 2.5} width={5} height={5} fill="none" stroke="var(--warn)" strokeWidth={1.2} />;
  return <circle cx={x} cy={y} r={2} fill="var(--chart-line)" />;
}

export interface SymbolChartProps {
  model: ReplayModel;
  symbol: string;
  cursor: number;
  layout: Layout;
  multiDay: boolean;
  onSelect: (position: number) => void;
  onStep: (direction: "next" | "previous") => void;
  /** Compare: the side's colours and dashes, a name for the chart, shared hover, and a compact height. */
  style?: ChartStyle;
  memberLabel?: string;
  hover?: number | null;
  onHover?: (position: number | null) => void;
  compact?: boolean;
  hideAxis?: boolean;
}

export function SymbolChart({ model, symbol, cursor, layout, multiDay, onSelect, onStep, style, memberLabel, hover: sharedHover, onHover, compact, hideAxis }: SymbolChartProps) {
  const [ref, width] = useElementWidth<HTMLDivElement>();
  const [ownHover, setOwnHover] = useState<number | null>(null);
  const hover = sharedHover !== undefined ? sharedHover : ownHover;
  const setHover = onHover ?? setOwnHover;
  const selectedColor = style?.selected ?? "var(--accent)";
  const names = useMemo(() => [...layout.overlays.map((o) => o.name), ...(layout.pane ? [layout.pane.name] : [])], [layout]);
  const track = trackFor(model, symbol, names);
  const n = track.indexes.length;
  const shown = revealed(track, cursor);
  const sel = selectedEvent(model, cursor);
  const selIndex = sel && sel.symbol === symbol ? lowerBound(track.indexes, sel.index) : -1;

  const priceH = compact ? (layout.pane ? 170 : 190) : layout.pane ? 200 : 250;
  const paneH = layout.pane ? (compact ? 110 : 130) : 0;
  const axisH = hideAxis ? 6 : 24;
  const innerW = Math.max(10, width - PAD.left - PAD.right);
  const x = (i: number) => PAD.left + (n <= 1 ? innerW / 2 : (i / (n - 1)) * innerW);

  const price = useMemo(() => {
    const all = [...track.close.slice(0, shown), ...layout.overlays.flatMap((o) => track.indicators[o.name]!.slice(0, shown))];
    const [lo, hi] = domain(all);
    const { ticks, step } = niceTicks(lo, hi);
    const y = (v: number) => PAD.top + (1 - (v - lo) / (hi - lo)) * (priceH - PAD.top - PAD.bottom);
    return { lo, hi, ticks, step, y };
  }, [track, shown, layout, priceH]);

  const pane = useMemo(() => {
    if (!layout.pane) return undefined;
    const z = track.indicators[layout.pane.name]!.slice(0, shown);
    const bound = Math.max(0.5, ...z.map((v) => (v === null ? 0 : Math.abs(v))), ...layout.pane.bands.map((b) => b.value)) * 1.15;
    const { ticks, step } = niceTicks(-bound, bound, 4);
    const y = (v: number) => PAD.top + (1 - (v + bound) / (2 * bound)) * (paneH - PAD.top - PAD.bottom);
    return { bound, ticks, step, y };
  }, [track, shown, layout, paneH]);

  const showMarkers = shown <= MARKER_LIMIT;
  const signalCount = useMemo(() => track.sides.slice(0, shown).filter(Boolean).length, [track, shown]);
  const totalH = priceH + paneH + axisH;
  const fmtPrice = (v: number) => v.toFixed(decimalsFor(price.step));

  const axisLabels = useMemo(() => {
    // Labels are spread over the REVEALED part of the axis (the axis itself is fixed to all rows), so an early position does not cram them together.
    const revealedW = n <= 1 ? innerW : (Math.max(0, shown - 1) / (n - 1)) * innerW;
    const count = Math.min(shown, revealedW < 90 ? 1 : Math.max(2, Math.floor(revealedW / 90)));
    const picks = new Set<number>();
    for (let k = 0; k < count; k++) picks.add(count === 1 ? 0 : Math.round((k * (shown - 1)) / (count - 1)));
    return [...picks].map((i) => {
      const iso = model.events[track.indexes[i]!]!.time;
      return { i, text: multiDay ? `${etDay(iso).slice(5)} ${etClock(iso).slice(0, 5)}` : etClock(iso).slice(0, 8) };
    });
  }, [shown, innerW, model, track, multiDay]);

  const locate = (clientX: number, rect: DOMRect): number | null => {
    if (shown === 0) return null;
    const ratio = (clientX - rect.left - PAD.left) / innerW;
    return Math.max(0, Math.min(shown - 1, Math.round(n <= 1 ? 0 : ratio * (n - 1))));
  };

  const key = (e: KeyboardEvent) => {
    if (e.key === "ArrowRight") { e.preventDefault(); onStep("next"); }
    else if (e.key === "ArrowLeft") { e.preventDefault(); onStep("previous"); }
  };

  const tip = hover !== null && hover < shown ? hover : null;
  const tipEvent = tip !== null ? model.events[track.indexes[tip]!]! : undefined;
  const tipResult = tipEvent ? resultOf(model, tipEvent) : undefined;
  const guideX = selIndex >= 0 && selIndex < shown ? x(selIndex) : undefined;

  const body: ReactNode =
    width > 0 && (
      <svg className="svg-chart" width={width} height={totalH} role="img" aria-label={`${memberLabel ? `${memberLabel}, ` : ""}${symbol}: close price${layout.pane ? ", rolling mean and z-score" : layout.overlays.length ? " and averages" : ""}. ${shown} of ${n} rows revealed.`}>
        {shown > 0 && price.ticks.map((t) => (
          <g key={t}>
            <line x1={PAD.left} x2={width - PAD.right} y1={price.y(t)} y2={price.y(t)} stroke="var(--chart-grid)" />
            <text x={width - PAD.right + 8} y={price.y(t) + 4}>{fmtPrice(t)}</text>
          </g>
        ))}
        {shown > 0 && <path d={pathFor(track, track.close, shown, x, price.y, true)} fill="none" stroke="var(--chart-line)" strokeWidth={1.6} />}
        {layout.overlays.map((o, k) => (
          <path key={o.name} d={pathFor(track, track.indicators[o.name]!, shown, x, price.y, false)} fill="none" stroke={style ? style.color : TONE[o.tone]} strokeWidth={style ? 1.9 : 1.6}
            strokeDasharray={style ? style.dashes[k] : o.dashed ? "5 3" : undefined} />
        ))}
        {pane && layout.pane && (
          <g transform={`translate(0,${priceH})`}>
            <line x1={PAD.left} x2={width - PAD.right} y1={0} y2={0} stroke="var(--line)" />
            {pane.ticks.map((t) => (
              <g key={t}>
                <line x1={PAD.left} x2={width - PAD.right} y1={pane.y(t)} y2={pane.y(t)} stroke={t === 0 ? "var(--chart-axis)" : "var(--chart-grid)"} />
                <text x={width - PAD.right + 8} y={pane.y(t) + 4}>{t.toFixed(decimalsFor(pane.step))}</text>
              </g>
            ))}
            {layout.pane.bands.flatMap((b) => [b.value, -b.value].map((v) => (
              <g key={`${b.param}${v}`}>
                <line x1={PAD.left} x2={width - PAD.right} y1={pane.y(v)} y2={pane.y(v)} stroke={b.kind === "entry" ? "var(--warn)" : "var(--text-3)"} strokeDasharray={b.kind === "entry" ? "6 3" : "2 3"} />
                <text x={PAD.left + 4} y={pane.y(v) + (v > 0 ? -4 : 11)} fill={b.kind === "entry" ? "var(--warn)" : "var(--text-3)"}>{b.kind === "entry" ? "entry" : "re-arm"} {v > 0 ? "+" : "−"}{b.text}</text>
              </g>
            )))}
            {shown > 0 && <path d={pathFor(track, track.indicators[layout.pane.name]!, shown, x, pane.y, false)} fill="none" stroke={style ? style.color : "var(--accent)"} strokeWidth={style ? 1.9 : 1.6} strokeDasharray={style?.zDash} />}
            {track.sides.slice(0, shown).map((side, i) => {
              const z = track.indicators[layout.pane!.name]![i];
              return side && z !== null && z !== undefined ? <Marker key={i} x={x(i)} y={pane.y(z)} side={side} label={false} verdict="evaluated" type="bar" /> : null;
            })}
            {selIndex >= 0 && selIndex < shown && track.indicators[layout.pane.name]![selIndex] != null && (
              <circle cx={x(selIndex)} cy={pane.y(track.indicators[layout.pane.name]![selIndex]!)} r={6.5} fill="none" stroke={selectedColor} strokeWidth={2} />
            )}
          </g>
        )}
        {guideX !== undefined && <line x1={guideX} x2={guideX} y1={PAD.top} y2={priceH + paneH} stroke={selectedColor} strokeOpacity={0.35} strokeDasharray="3 3" />}
        {Array.from({ length: shown }, (_, i) => {
          const y = track.close[i];
          if (y === null || y === undefined) return null;
          const side = track.sides[i] ?? null;
          if (!showMarkers && !side) return null;
          return <Marker key={i} x={x(i)} y={price.y(y)} side={side} label={signalCount <= 40} verdict={track.verdicts[i]} type={track.types[i]!} />;
        })}
        {selIndex >= 0 && selIndex < shown && track.close[selIndex] != null && <circle cx={x(selIndex)} cy={price.y(track.close[selIndex]!)} r={7} fill="none" stroke={selectedColor} strokeWidth={2} />}
        <g transform={`translate(0,${priceH + paneH})`}>
          <line x1={PAD.left} x2={width - PAD.right} y1={0} y2={0} stroke="var(--chart-axis)" />
          {!hideAxis && axisLabels.map(({ i, text }) => <text key={i} x={x(i)} y={16} textAnchor={x(i) < PAD.left + 28 ? "start" : x(i) > width - PAD.right - 28 ? "end" : "middle"}>{text}</text>)}
          {!hideAxis && <text x={width - PAD.right + 8} y={16}>ET</text>}
        </g>
        <rect x={PAD.left} y={0} width={innerW} height={priceH + paneH} fill="transparent" style={{ cursor: shown ? "crosshair" : "default" }}
          onPointerMove={(e) => setHover(locate(e.clientX, e.currentTarget.getBoundingClientRect()))}
          onPointerLeave={() => setHover(null)}
          onClick={(e) => { const i = locate(e.clientX, e.currentTarget.getBoundingClientRect()); if (i !== null) onSelect(track.indexes[i]! + 1); }} />
        {tip !== null && <line x1={x(tip)} x2={x(tip)} y1={PAD.top} y2={priceH + paneH} stroke="var(--line-strong)" pointerEvents="none" />}
      </svg>
    );

  return (
    <div className="lab-symbol">
      <div className="lab-symbol-head">
        {memberLabel && <span className="cmp-chip" data-member={memberLabel}>{memberLabel}</span>}
        <b>{symbol}</b>
        <span className="faint">{shown} of {n} rows revealed{n > MARKER_LIMIT && shown > MARKER_LIMIT ? " · per-row markers hidden above 600 rows" : ""}</span>
      </div>
      <div ref={ref} className="lab-chart" tabIndex={0} onKeyDown={key} style={{ height: totalH }} aria-label={`${memberLabel ? `${memberLabel}, ` : ""}${symbol} chart. Arrow keys step through events.`}>
        {shown === 0 ? <div className="lab-chart-empty">Nothing revealed for {symbol} yet. Step forward to reveal its first row.</div> : null}
        {body}
        {tip !== null && tipEvent && tipResult && (
          <div className="svg-tip lab-tip" data-below={(track.close[tip] != null ? price.y(track.close[tip]!) : 30) < 110 ? "true" : undefined}
            style={{ left: Math.min(Math.max(x(tip), 90), Math.max(90, width - 90)), top: Math.max(8, (track.close[tip] != null ? price.y(track.close[tip]!) : 30)) }}>
            <b>{memberLabel ? `${memberLabel} · ` : ""}{tipEvent.symbol}</b> <span className="muted">event {tipEvent.index + 1}</span>
            <div className="muted">{etFull(tipEvent.time)}</div>
            <div>{tipEvent.price === undefined ? "no price" : `${tipEvent.type === "bar" ? "close" : tipEvent.type} ${tipEvent.price}`}</div>
            {layout.overlays.map((o) => <div key={o.name} className="muted">{o.label.replace(/ \(.*\)$/, "")}: {tipResult.indicators[o.name]?.text ?? "unavailable"}</div>)}
            {layout.pane && <div className="muted">{layout.pane.label}: {tipResult.indicators[layout.pane.name]?.text ?? "unavailable"}</div>}
            <div>{tipResult.available ? `${tipResult.verdict}: ${tipResult.reason}` : "no diagnostics"}{track.sides[tip] ? ` · ${track.sides[tip] === "buy" ? "Buy" : "Sell"} request` : ""}</div>
          </div>
        )}
      </div>
    </div>
  );
}

function Legend({ layout, pane }: { layout: Layout; pane: boolean }) {
  const glyph = (node: ReactNode) => <svg width="16" height="12" viewBox="0 0 16 12" aria-hidden="true">{node}</svg>;
  return (
    <div className="chart-legend" aria-label="Chart legend">
      <span className="legend-key"><span className="legend-swatch" style={{ background: "var(--chart-line)" }} />Close</span>
      {layout.overlays.map((o) => (
        <span key={o.name} className="legend-key" style={{ color: TONE[o.tone] }}>
          <span className={`legend-swatch ${o.dashed ? "legend-swatch-dashed" : ""}`} style={{ background: TONE[o.tone] }} />
          <span style={{ color: "var(--text-2)" }}>{o.label}</span>
        </span>
      ))}
      {pane && <span className="legend-key"><span className="legend-swatch" style={{ background: "var(--accent)" }} />z-score (own panel)</span>}
      <span className="legend-key">{glyph(<path d="M8 2 l5 8 h-10 z" fill="var(--up)" />)}Buy request</span>
      <span className="legend-key">{glyph(<path d="M8 10 l5 -8 h-10 z" fill="var(--down)" />)}Sell request</span>
      <span className="legend-key">{glyph(<circle cx="8" cy="6" r="3" fill="var(--surface)" stroke="var(--text-3)" strokeWidth="1.3" />)}Warming up</span>
      <span className="legend-key">{glyph(<path d="M5 3 l6 6 M11 3 l-6 6" stroke="var(--text-3)" strokeWidth="1.3" />)}Ignored</span>
      <span className="legend-key">{glyph(<circle cx="8" cy="6" r="2" fill="var(--chart-line)" />)}Evaluated</span>
      <span className="legend-key">{glyph(<circle cx="8" cy="6" r="4.5" fill="none" stroke="var(--accent)" strokeWidth="2" />)}Selected event</span>
    </div>
  );
}

export function ReplayChart({ model, cursor, symbol, onSelect, onStep }: {
  model: ReplayModel;
  cursor: number;
  symbol: string | null;
  onSelect: (position: number) => void;
  onStep: (direction: "next" | "previous") => void;
}) {
  const layout = useMemo(() => layoutFor(model), [model]);
  const symbols = symbol === null ? model.symbols.slice(0, MAX_PANELS) : [symbol];
  const multiDay = useMemo(() => new Set(model.events.map((e) => etDay(e.time))).size > 1, [model]);
  return (
    <div className="lab-chart-stack">
      <Legend layout={layout} pane={!!layout.pane} />
      {layout.overlays.length === 0 && !layout.pane && <p className="faint" style={{ fontSize: "var(--fs-sm)" }}>No chart layout is defined for this strategy kind, so only the close and the markers are drawn. Every recorded value is in the tables below.</p>}
      {symbols.map((s) => (
        <SymbolChart key={s} model={model} symbol={s} cursor={cursor} layout={layout} multiDay={multiDay} onSelect={onSelect} onStep={onStep} />
      ))}
      {symbol === null && model.symbols.length > MAX_PANELS && (
        <p className="faint" style={{ fontSize: "var(--fs-sm)" }}>Showing the first {MAX_PANELS} of {model.symbols.length} symbols. Pick one in Display symbol to see the others.</p>
      )}
      <p className="faint lab-axis-note">Horizontal axis: this symbol's rows in recorded order, labelled in exchange time (ET). Prices are drawn approximately; hover a point for the exact recorded values.</p>
    </div>
  );
}
