import { Rng, hash32 } from "./prng";

// Synthetic one-minute bars on a fixed trading calendar. Prices are integer micros rounded to cents.

export interface RawBar {
  symbol: string;
  time: number; // bar open, epoch ms
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
}

export const SESSION_OPEN_MIN = 13 * 60 + 30; // 09:30 New York during daylight time, in UTC minutes
export const BARS_PER_SESSION = 390;
const MINUTE = 60_000;
const DAY = 86_400_000;
const HOLIDAYS = new Set(["2026-09-07", "2026-11-26", "2026-12-25"]);

export const FIRST_SESSION = "2026-08-03";
export const LAST_SESSION = "2026-12-31";
// Backtests may use completed sessions only; the live demo session starts after this.
export const LAST_BACKTEST_SESSION = "2026-10-07";
export const LIVE_SESSION = "2026-10-08";

const PROFILES: Record<string, { start: number; vol: number; volume: number }> = {
  AAPL: { start: 231.4, vol: 0.24, volume: 52_000 },
  MSFT: { start: 428.1, vol: 0.22, volume: 21_000 },
  SPY: { start: 562.7, vol: 0.14, volume: 88_000 },
  NVDA: { start: 121.3, vol: 0.46, volume: 210_000 },
  AMZN: { start: 186.9, vol: 0.3, volume: 47_000 },
};

export const SYMBOLS = Object.keys(PROFILES);

let calendar: string[] | undefined;

export function sessionDates(): string[] {
  if (calendar) return calendar;
  const out: string[] = (calendar = []);
  for (let t = Date.parse(`${FIRST_SESSION}T00:00:00Z`); t <= Date.parse(`${LAST_SESSION}T00:00:00Z`); t += DAY) {
    const d = new Date(t);
    const iso = d.toISOString().slice(0, 10);
    if (d.getUTCDay() !== 0 && d.getUTCDay() !== 6 && !HOLIDAYS.has(iso)) out.push(iso);
  }
  return out;
}

export function sessionOpen(date: string): number {
  return Date.parse(`${date}T00:00:00Z`) + SESSION_OPEN_MIN * MINUTE;
}

const cache = new Map<string, Map<number, RawBar>>();

const toCents = (dollars: number) => Math.round(dollars * 100) * 10_000;

function generate(symbol: string): Map<number, RawBar> {
  const profile = PROFILES[symbol];
  if (!profile) throw new Error(`unknown symbol ${symbol}`);
  const rng = new Rng(hash32(symbol));
  const perMinute = profile.vol / Math.sqrt(252 * BARS_PER_SESSION);
  const bars = new Map<number, RawBar>();
  let last = profile.start;
  let trend = 0;

  for (const date of sessionDates()) {
    last *= Math.exp(perMinute * 6 * rng.normal()); // overnight gap
    const open = sessionOpen(date);
    for (let i = 0; i < BARS_PER_SESSION; i++) {
      if (i % 45 === 0) trend = perMinute * 0.08 * rng.normal(); // slow regimes give the strategies something to find
      const u = i / (BARS_PER_SESSION - 1);
      const intraday = 0.75 + 2.8 * (u - 0.5) ** 2; // U-shaped activity
      const o = last;
      let hi = o;
      let lo = o;
      let p = o;
      for (let k = 0; k < 6; k++) {
        p *= Math.exp(trend / 6 + (perMinute * intraday * rng.normal()) / Math.sqrt(6));
        hi = Math.max(hi, p);
        lo = Math.min(lo, p);
      }
      last = p;
      const volume = Math.round(profile.volume * intraday * rng.logNormal(1, 0.45) / 10) * 10;
      const time = open + i * MINUTE;
      bars.set(time, { symbol, time, open: toCents(o), high: toCents(hi), low: toCents(lo), close: toCents(p), volume });
    }
  }
  return bars;
}

export function barAt(symbol: string, time: number): RawBar | undefined {
  let series = cache.get(symbol);
  if (!series) {
    series = generate(symbol);
    cache.set(symbol, series);
  }
  return series.get(time);
}

// Bar-open timestamps from `start` (inclusive) to `end` (exclusive), sessions only.
export function minutesBetween(start: number, end: number): number[] {
  const out: number[] = [];
  for (const date of sessionDates()) {
    const open = sessionOpen(date);
    for (let i = 0; i < BARS_PER_SESSION; i++) {
      const t = open + i * MINUTE;
      if (t >= start && t < end) out.push(t);
    }
  }
  return out;
}

// The next bar-open after `time`, or undefined past the end of the calendar.
export function nextMinute(time: number): number | undefined {
  for (const date of sessionDates()) {
    const open = sessionOpen(date);
    const close = open + BARS_PER_SESSION * MINUTE;
    if (time + MINUTE < close && time + MINUTE >= open) return time + MINUTE;
    if (open > time) return open;
  }
  return undefined;
}

export function sessionOf(time: number): string {
  return new Date(time).toISOString().slice(0, 10);
}
