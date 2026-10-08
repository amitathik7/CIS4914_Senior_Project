import type { Bar, EquityPoint, Fill } from "../api/contract";
import { toNumber } from "../lib/decimal";
import { chartTime, type CandlePoint, type LinePoint, type MarkerSpec } from "../components/charts/TimeChart";

export const equityLine = (points: EquityPoint[]): LinePoint[] =>
  points.map((p) => ({ time: chartTime(p.time), value: toNumber(p.equity) }));

export const drawdownLine = (points: EquityPoint[]): LinePoint[] =>
  points.map((p) => ({ time: chartTime(p.time), value: p.drawdown * 100 }));

export const candles = (bars: Bar[]): CandlePoint[] =>
  bars.map((b) => ({ time: chartTime(b.exchange_time), open: toNumber(b.open), high: toNumber(b.high), low: toNumber(b.low), close: toNumber(b.price) }));

// One marker per bar and side; fills inside the same minute collapse into it.
export function fillMarkers(fills: Fill[], symbol: string): MarkerSpec[] {
  const byKey = new Map<string, MarkerSpec & { qty: number }>();
  for (const f of fills) {
    if (f.symbol !== symbol) continue;
    const minute = `${f.filled_at.slice(0, 16)}:00Z`;
    const key = `${minute}|${f.side}`;
    const existing = byKey.get(key);
    if (existing) existing.qty += f.filled_quantity;
    else byKey.set(key, { time: chartTime(minute), side: f.side, qty: f.filled_quantity });
  }
  return [...byKey.values()]
    .sort((a, b) => a.time - b.time)
    .map(({ time, side }) => ({ time, side }));
}

// US sessions never cross midnight UTC, so the UTC date identifies the session.
export function lastSession<T extends { time: string }>(points: T[]): T[] {
  const last = points[points.length - 1];
  if (!last) return points;
  const day = last.time.slice(0, 10);
  return points.filter((p) => p.time.startsWith(day));
}
