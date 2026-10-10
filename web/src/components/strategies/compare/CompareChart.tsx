import { useMemo, useState, type ReactNode } from "react";
import { etDay } from "../../../lib/exactTime";
import { MEMBERS, type CompareModel, type Member, type Relation } from "../../../strategies/compare";
import { lowerBound, selectedEvent } from "../../../strategies/cursor";
import { useElementWidth } from "../../charts/useElementWidth";
import { layoutFor, MARKER_LIMIT, PAD, SymbolChart, type ChartStyle, type Layout } from "../ReplayChart";

// Compare draws ONE symbol at a time as aligned small multiples: configuration A above configuration B, then a ribbon of what the two did
// on each row. All three share the same horizontal axis (the symbol's rows in recorded order), the same width and the same hover, and the
// shared replay position is the ring on both charts. Each side keeps its own price axis and, for mean reversion, its own z-score panel:
// z-scores and prices are never put on one axis, and the two sides' scales are never forced to agree.

export const SIDE_STYLE: Record<Member, ChartStyle> = {
  A: { color: "var(--cmp-a)", dashes: [undefined, "6 3"], selected: "var(--text)" },
  B: { color: "var(--cmp-b)", dashes: ["2 3", "9 3 2 3"], zDash: "5 2", selected: "var(--text)" },
};

const RELATION_TEXT: Record<Relation, string> = { agree: "agree", differ: "requests differ", not_comparable: "not comparable" };

/** The symbol the charts show: the display filter, else the selected event's symbol, else the first symbol. */
export function chartSymbol(model: CompareModel, cursor: number, filter: string | null): string | undefined {
  return filter ?? selectedEvent(model.members.A, cursor)?.symbol ?? model.symbols[0];
}

function Ribbon({ model, symbol, cursor, hover, onHover, onSelect }: {
  model: CompareModel; symbol: string; cursor: number; hover: number | null; onHover: (i: number | null) => void; onSelect: (position: number) => void;
}) {
  const [ref, width] = useElementWidth<HTMLDivElement>();
  const indexes = model.members.A.indexesBySymbol.get(symbol) ?? [];
  const n = indexes.length;
  const shown = lowerBound(indexes, cursor);
  const innerW = Math.max(10, width - PAD.left - PAD.right);
  const x = (i: number) => PAD.left + (n <= 1 ? innerW / 2 : (i / (n - 1)) * innerW);
  const LANE = 18;
  const dense = shown > MARKER_LIMIT; // as in the price charts: past this many rows only requests and differences are marked
  const lanes = [
    { label: "A requests", y: 2 },
    { label: "B requests", y: 2 + LANE },
    { label: "A vs B", y: 2 + 2 * LANE },
  ];
  const height = 2 + 3 * LANE + 2;
  const locate = (clientX: number, rect: DOMRect): number | null => {
    if (shown === 0) return null;
    return Math.max(0, Math.min(shown - 1, Math.round(n <= 1 ? 0 : ((clientX - rect.left - PAD.left) / innerW) * (n - 1))));
  };
  const counts = useMemo(() => {
    const out = { agree: 0, differ: 0, not_comparable: 0 };
    for (let i = 0; i < shown; i++) out[model.rows[indexes[i]!]!.relation]++;
    return out;
  }, [model, indexes, shown]);

  const body: ReactNode = width > 0 && (
    <svg className="svg-chart" width={width} height={height} role="img"
      aria-label={`Requests and agreement for ${symbol}, ${shown} of ${n} rows revealed: ${counts.agree} agree, ${counts.differ} differ, ${counts.not_comparable} not comparable.`}>
      {lanes.map((lane) => (
        <g key={lane.label}>
          <line x1={PAD.left} x2={width - PAD.right} y1={lane.y + LANE / 2} y2={lane.y + LANE / 2} stroke="var(--chart-grid)" />
          <text x={width - PAD.right + 8} y={lane.y + LANE / 2 + 4}>{lane.label}</text>
        </g>
      ))}
      {Array.from({ length: shown }, (_, i) => {
        const row = model.rows[indexes[i]!]!;
        const cx = x(i);
        const marks: ReactNode[] = [];
        (["A", "B"] as const).forEach((m, lane) => {
          const sides = row.sides[m];
          const cy = lanes[lane]!.y + LANE / 2;
          if (sides.includes("buy")) marks.push(<path key={`${m}b`} d={`M${cx},${cy - 6} l-4.5,9 h9 z`} fill="var(--up)" />);
          if (sides.includes("sell")) marks.push(<path key={`${m}s`} d={`M${cx},${cy + 6} l-4.5,-9 h9 z`} fill="var(--down)" />);
        });
        const cy = lanes[2]!.y + LANE / 2;
        if (row.relation === "differ") marks.push(<rect key="d" x={cx - 1.5} y={cy - 7} width={3} height={14} fill="var(--warn)" />);
        else if (!dense && row.relation === "agree") marks.push(<rect key="a" x={cx - 1} y={cy - 3} width={2} height={6} fill="var(--text-2)" />);
        else if (!dense) marks.push(<circle key="n" cx={cx} cy={cy} r={1.6} fill="none" stroke="var(--text-3)" />);
        return marks.length ? <g key={i}>{marks}</g> : null;
      })}
      <rect x={PAD.left} y={0} width={innerW} height={height} fill="transparent" style={{ cursor: shown ? "crosshair" : "default" }}
        onPointerMove={(e) => onHover(locate(e.clientX, e.currentTarget.getBoundingClientRect()))} onPointerLeave={() => onHover(null)}
        onClick={(e) => { const i = locate(e.clientX, e.currentTarget.getBoundingClientRect()); if (i !== null) onSelect(indexes[i]! + 1); }} />
      {hover !== null && hover < shown && <line x1={x(hover)} x2={x(hover)} y1={0} y2={height} stroke="var(--line-strong)" pointerEvents="none" />}
    </svg>
  );
  const tipRow = hover !== null && hover < shown ? model.rows[indexes[hover]!] : undefined;
  return (
    <div className="lab-symbol">
      <div className="lab-symbol-head"><b>Requests and agreement</b><span className="faint">{symbol}: {counts.agree} agree · {counts.differ} differ · {counts.not_comparable} not comparable, in {shown} revealed rows</span></div>
      <div ref={ref} className="lab-chart" style={{ height }}>{body}</div>
      <p className="faint cmp-ribbon-note" aria-live="off">{tipRow ? `Event ${tipRow.index + 1}: ${RELATION_TEXT[tipRow.relation]}.` : dense ? "Hover or click to pick a row. Past 600 revealed rows only requests and differences are marked; the counts above cover every row." : "Hover or click a mark to pick a row."}</p>
    </div>
  );
}

function Legend({ layouts }: { layouts: Record<Member, Layout> }) {
  const line = (m: Member, dash: string | undefined) => (
    <svg width="26" height="10" viewBox="0 0 26 10" aria-hidden="true"><line x1="1" x2="25" y1="5" y2="5" stroke={SIDE_STYLE[m].color} strokeWidth="2" strokeDasharray={dash} /></svg>
  );
  return (
    <div className="chart-legend" aria-label="Chart legend">
      <span className="legend-key"><span className="legend-swatch" style={{ background: "var(--chart-line)" }} />Close (shared)</span>
      {MEMBERS.map((m) => (
        <span key={m} className="legend-key">
          <span className="cmp-chip" data-member={m}>{m}</span>
          {layouts[m].overlays.map((o, k) => <span key={o.name} className="legend-key">{line(m, SIDE_STYLE[m].dashes[k])}<span style={{ color: "var(--text-2)" }}>{o.label}</span></span>)}
          {layouts[m].pane && <span className="legend-key">{line(m, SIDE_STYLE[m].zDash)}<span style={{ color: "var(--text-2)" }}>z-score (own panel)</span></span>}
        </span>
      ))}
      <span className="legend-key"><svg width="16" height="12" viewBox="0 0 16 12" aria-hidden="true"><path d="M8 2 l5 8 h-10 z" fill="var(--up)" /></svg>Buy request</span>
      <span className="legend-key"><svg width="16" height="12" viewBox="0 0 16 12" aria-hidden="true"><path d="M8 10 l5 -8 h-10 z" fill="var(--down)" /></svg>Sell request</span>
      <span className="legend-key"><svg width="16" height="12" viewBox="0 0 16 12" aria-hidden="true"><rect x="6.5" y="1" width="3" height="10" fill="var(--warn)" /></svg>Requests differ</span>
      <span className="legend-key"><svg width="16" height="12" viewBox="0 0 16 12" aria-hidden="true"><rect x="7" y="3" width="2" height="6" fill="var(--text-2)" /></svg>Agree</span>
      <span className="legend-key"><svg width="16" height="12" viewBox="0 0 16 12" aria-hidden="true"><circle cx="8" cy="6" r="2" fill="none" stroke="var(--text-3)" /></svg>Not comparable</span>
      <span className="legend-key"><svg width="16" height="12" viewBox="0 0 16 12" aria-hidden="true"><circle cx="8" cy="6" r="4.5" fill="none" stroke="var(--text)" strokeWidth="2" /></svg>Selected event</span>
    </div>
  );
}

export function CompareChart({ model, cursor, filter, onSelect, onStep }: {
  model: CompareModel; cursor: number; filter: string | null; onSelect: (position: number) => void; onStep: (direction: "next" | "previous") => void;
}) {
  const [hover, setHover] = useState<number | null>(null);
  const symbol = chartSymbol(model, cursor, filter);
  const layouts = useMemo(() => ({ A: layoutFor(model.members.A), B: layoutFor(model.members.B) }), [model]);
  const multiDay = useMemo(() => new Set(model.events.map((e) => etDay(e.time))).size > 1, [model]);
  if (!symbol) return <p className="faint">This result has no rows to draw.</p>;
  return (
    <div className="lab-chart-stack">
      <Legend layouts={layouts} />
      {MEMBERS.map((m) => (
        <SymbolChart
          key={m} model={model.members[m]} symbol={symbol} cursor={cursor} layout={layouts[m]} multiDay={multiDay} onSelect={onSelect} onStep={onStep}
          style={SIDE_STYLE[m]} memberLabel={m} hover={hover} onHover={setHover} compact hideAxis={m === "A"}
        />
      ))}
      <Ribbon model={model} symbol={symbol} cursor={cursor} hover={hover} onHover={setHover} onSelect={onSelect} />
      <p className="faint lab-axis-note">
        {filter === null ? `Showing ${symbol}: the chart follows the selected event's symbol; pick one in Display symbol to pin it. ` : `Showing ${symbol} (Display symbol). `}
        Horizontal axis: this symbol's rows in recorded order (the same rows for A and B), labelled in exchange time (ET). Prices are drawn approximately; hover a point for the exact recorded values. Each side has its own price scale.
      </p>
    </div>
  );
}
