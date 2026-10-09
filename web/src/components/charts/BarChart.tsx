import { useId, useMemo, useState } from "react";
import { useElementWidth } from "./useElementWidth";

export interface BarDatum {
  label: string;
  value: number;
  tip?: string;
  tone?: "accent" | "up" | "down" | "warn";
}

interface Props {
  data: BarDatum[];
  height: number;
  format: (value: number) => string;
  label: string;
  tone?: "accent" | "up" | "down" | "warn";
  xLabel?: string;
}

// Ticks on a 1-2-5 step that covers the data in about four intervals.
function niceTicks(max: number): number[] {
  if (max <= 0) return [0, 1];
  const raw = max / 4;
  const magnitude = 10 ** Math.floor(Math.log10(raw));
  const step = ([1, 2, 5, 10].find((m) => m * magnitude >= raw) ?? 10) * magnitude;
  const ticks: number[] = [];
  for (let t = 0; t < max + step / 2; t += step) ticks.push(t);
  if (ticks[ticks.length - 1]! < max) ticks.push(ticks[ticks.length - 1]! + step);
  return ticks;
}

// Vertical bars with a hover tooltip; rounded data-ends anchored to the baseline.
export function BarChart({ data, height, format, label, tone = "accent", xLabel }: Props) {
  const [ref, width] = useElementWidth<HTMLDivElement>();
  const [active, setActive] = useState<number | null>(null);
  const titleId = useId();
  const pad = { top: 10, right: 8, bottom: xLabel ? 36 : 22, left: 44 };
  const ticks = useMemo(() => niceTicks(Math.max(0, ...data.map((d) => d.value))), [data]);
  const max = ticks[ticks.length - 1]!;
  const innerW = Math.max(0, width - pad.left - pad.right);
  const innerH = height - pad.top - pad.bottom;
  const slot = data.length ? innerW / data.length : 0;
  const barW = Math.max(2, Math.min(56, slot - 2));
  const y = (v: number) => pad.top + innerH - (v / max) * innerH;
  const color = `var(--${tone})`;
  const activeDatum = active !== null ? data[active] : undefined;

  return (
    <div ref={ref} className="chart" style={{ height }}>
      {width > 0 && (
        <svg className="svg-chart" width={width} height={height} role="img" aria-labelledby={titleId}>
          <title id={titleId}>{label}</title>
          {ticks.map((t) => (
            <g key={t}>
              <line x1={pad.left} x2={width - pad.right} y1={y(t)} y2={y(t)} stroke={t === 0 ? "var(--chart-axis)" : "var(--chart-grid)"} />
              <text x={pad.left - 8} y={y(t) + 4} textAnchor="end">{format(t)}</text>
            </g>
          ))}
          {data.map((d, i) => {
            const x = pad.left + i * slot + (slot - barW) / 2;
            const h = Math.max(0, (d.value / max) * innerH);
            const r = Math.min(4, barW / 2, h);
            const top = pad.top + innerH - h;
            return (
              <g key={d.label}>
                {h > 0 && (
                  <path
                    d={`M${x},${pad.top + innerH} V${top + r} Q${x},${top} ${x + r},${top} H${x + barW - r} Q${x + barW},${top} ${x + barW},${top + r} V${pad.top + innerH} Z`}
                    fill={d.tone ? `var(--${d.tone})` : color}
                    opacity={active === null || active === i ? 1 : 0.45}
                  />
                )}
                {(data.length <= 12 || i % Math.ceil(data.length / 12) === 0) && (
                  <text x={x + barW / 2} y={pad.top + innerH + 15} textAnchor="middle">{d.label}</text>
                )}
                <rect
                  x={pad.left + i * slot}
                  y={pad.top}
                  width={slot}
                  height={innerH}
                  fill="transparent"
                  tabIndex={0}
                  aria-label={`${d.label}: ${d.tip ?? format(d.value)}`}
                  onMouseEnter={() => setActive(i)}
                  onMouseLeave={() => setActive(null)}
                  onFocus={() => setActive(i)}
                  onBlur={() => setActive(null)}
                />
              </g>
            );
          })}
          {xLabel && <text x={pad.left + innerW / 2} y={height - 4} textAnchor="middle">{xLabel}</text>}
        </svg>
      )}
      {activeDatum && active !== null && (
        <div className="svg-tip" style={{ left: pad.left + active * slot + slot / 2, top: y(activeDatum.value) }}>
          <b>{activeDatum.label}</b> <span className="muted">{activeDatum.tip ?? format(activeDatum.value)}</span>
        </div>
      )}
    </div>
  );
}
