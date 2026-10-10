// Two EXISTING Console backtests, inspected side by side, and whether they may fairly be compared.
//
// Strategy Lab replays are signal-only, so the financial side of a comparison can only come from the Console's own backtests (which run
// strategies, risk and the execution simulator). This module never produces a return: it reads two recorded runs' reports, checks that
// the two runs were set up the same way, and says plainly when they were not. It names no winner and ranks nothing.
//
// What a match means here: both runs are completed backtests over the same evaluation period and symbols with the same starting
// capital, fee, slippage, latency, participation and risk settings. What it cannot mean: that the market data was the same bytes. The
// engine API records no data fingerprint, so comparability is never established, only ruled out: matching settings are reported as
// "unverified", and differing ones as "not comparable".

import type { DataSource, RunDetail, RunSummary } from "../api/contract";
import { money, sub, toMicros } from "../lib/decimal";
import { reportFigures, winRateText, type ReportFigure } from "../lib/reportFigures";

export interface Check {
  key: string;
  label: string;
  ok: boolean;
  /** The two runs' values, as text. */
  one: string;
  two: string;
}

export interface Comparability {
  /** `settings_match`: nothing ruled it out, but the market data cannot be checked, so comparability is unverified. */
  verdict: "settings_match" | "not_comparable";
  checks: Check[];
  /** The checks that failed, as sentences. */
  differences: string[];
  /** Cautions that apply even when every check passes. */
  cautions: string[];
}

const decimalEqual = (a: string, b: string): boolean => {
  try {
    return toMicros(a) === toMicros(b);
  } catch {
    return a === b;
  }
};

const symbolsOf = (run: RunDetail): string[] => [...new Set(run.config.strategies.flatMap((s) => s.symbols))].sort();
const same = (a: unknown, b: unknown): boolean => JSON.stringify(a) === JSON.stringify(b);

export function compareRecorded(one: RunDetail, two: RunDetail, source: DataSource | undefined): Comparability {
  const checks: Check[] = [];
  const add = (key: string, label: string, ok: boolean, a: string, b: string) => checks.push({ key, label, ok, one: a, two: b });
  const finished = (r: RunDetail) => r.mode === "backtest" && r.status === "completed" && r.report !== undefined;
  const state = (r: RunDetail) => (r.mode !== "backtest" ? "a live run" : r.status !== "completed" ? r.status : r.report ? "completed backtest" : "no report yet");
  add("finished", "Both are completed backtests with a report", finished(one) && finished(two), state(one), state(two));

  const a = one.config;
  const b = two.config;
  add("period", "Same evaluation period", a.period_start === b.period_start && a.period_end === b.period_end, `${a.period_start} → ${a.period_end ?? "open"}`, `${b.period_start} → ${b.period_end ?? "open"}`);
  add("symbols", "Same symbols traded", same(symbolsOf(one), symbolsOf(two)), symbolsOf(one).join(", ") || "none", symbolsOf(two).join(", ") || "none");
  add("cash", "Same starting capital", decimalEqual(a.simulation.starting_cash, b.simulation.starting_cash), money(a.simulation.starting_cash), money(b.simulation.starting_cash));
  const fee = (r: RunDetail) => `${r.config.simulation.fees.per_share}/share + ${r.config.simulation.fees.per_trade}/trade + ${r.config.simulation.fees.bps} bps`;
  add("fees", "Same fee model", decimalEqual(a.simulation.fees.per_share, b.simulation.fees.per_share) && decimalEqual(a.simulation.fees.per_trade, b.simulation.fees.per_trade) && a.simulation.fees.bps === b.simulation.fees.bps, fee(one), fee(two));
  add("slippage", "Same slippage", a.simulation.slippage_bps === b.simulation.slippage_bps, `${a.simulation.slippage_bps} bps`, `${b.simulation.slippage_bps} bps`);
  const latency = (r: RunDetail) => `${r.config.simulation.latency_ms.signal_to_order} ms to order, ${r.config.simulation.latency_ms.order_to_fill} ms to fill`;
  add("latency", "Same latency", same(a.simulation.latency_ms, b.simulation.latency_ms), latency(one), latency(two));
  add("participation", "Same participation cap", a.simulation.participation_cap === b.simulation.participation_cap, String(a.simulation.participation_cap), String(b.simulation.participation_cap));
  const risk = (r: RunDetail) => `order ${r.config.risk.max_order_quantity} sh, position ${money(r.config.risk.max_position_notional)}, gross ${money(r.config.risk.max_gross_exposure)}, daily loss ${money(r.config.risk.max_daily_loss)}, ${r.config.risk.max_open_orders} open, shorting ${r.config.risk.allow_short ? "on" : "off"}`;
  const riskSame = decimalEqual(a.risk.max_position_notional, b.risk.max_position_notional) && decimalEqual(a.risk.max_gross_exposure, b.risk.max_gross_exposure) && decimalEqual(a.risk.max_daily_loss, b.risk.max_daily_loss)
    && a.risk.max_order_quantity === b.risk.max_order_quantity && a.risk.max_open_orders === b.risk.max_open_orders && a.risk.allow_short === b.risk.allow_short;
  add("risk", "Same risk limits", riskSame, risk(one), risk(two));

  const differences = checks.filter((c) => !c.ok).map((c) => `${c.label.replace(/^Same /, "").replace(/^Both are /, "")}: ${c.one} / ${c.two}`);
  const cautions: string[] = [
    "The Console's engine API records no fingerprint of the market data, so matching settings do not prove the two runs saw the same bars.",
  ];
  if (source === "demo") cautions.push("These runs come from the console's in-browser demo simulator, not the C++ engine: treat the figures as a demonstration.");
  for (const [name, run] of [["Run 1", one], ["Run 2", two]] as const) {
    if (run.config.strategies.length > 1) cautions.push(`${name} traded ${run.config.strategies.length} strategies in one portfolio. Its figures describe the whole portfolio and cannot be attributed to one strategy.`);
  }
  return { verdict: differences.length === 0 ? "settings_match" : "not_comparable", checks, differences, cautions };
}

/** What the verdict is called and explained as. A match is never worded as established comparability. */
export const VERDICT_TEXT: Readonly<Record<Comparability["verdict"], { badge: string; explain: string }>> = {
  settings_match: {
    badge: "Unverified · settings match",
    explain: "The two runs' settings match, but the market data behind them cannot be checked (the engine API records no fingerprint of it), so they are not shown to be comparable. Read their figures with the cautions below in mind.",
  },
  not_comparable: {
    badge: "Not comparable",
    explain: "These runs were not set up the same way, so their figures should not be set against each other.",
  },
};

export interface MetricRow {
  key: string;
  label: string;
  one: string;
  two: string;
  /** Why a value is a dash on either side, per side. */
  noteOne?: string;
  noteTwo?: string;
}

/** Net P&L = ending equity minus starting capital, in exact decimal arithmetic. */
export function netPnl(run: RunDetail): string | undefined {
  return run.report ? sub(run.report.ending_equity, run.config.simulation.starting_cash) : undefined;
}

const figureText = (f: ReportFigure | undefined): string => f?.value ?? "—";
const reason = (f: ReportFigure | undefined): string | undefined => (f && !f.available ? f.sub : undefined);

/** The metrics both runs' reports define, in the Report page's own words. Missing values stay dashes with their reason. */
export function metricRows(one: RunSummary, two: RunSummary, pnl: readonly [string | undefined, string | undefined]): MetricRow[] {
  const fx = (r: typeof one) => (r.report ? reportFigures(r.report) : []);
  const [fa, fb] = [fx(one), fx(two)];
  const pick = (list: ReportFigure[], key: ReportFigure["key"]) => list.find((f) => f.key === key);
  const rows: MetricRow[] = [
    { key: "pnl", label: "Net P&L", one: pnl[0] ? money(pnl[0], true) : "—", two: pnl[1] ? money(pnl[1], true) : "—" },
  ];
  for (const [key, label] of [["total_return", "Net return"], ["max_drawdown", "Max drawdown"], ["trades", "Completed trades"]] as const) {
    rows.push({ key, label, one: figureText(pick(fa, key)), two: figureText(pick(fb, key)) });
  }
  const win = (r: typeof one) => (r.report ? winRateText(r.report) : undefined);
  rows.push({ key: "win_rate", label: "Win rate", one: win(one)?.value ?? "—", two: win(two)?.value ?? "—", ...(win(one) && !win(one)!.available ? { noteOne: win(one)!.sub } : {}), ...(win(two) && !win(two)!.available ? { noteTwo: win(two)!.sub } : {}) });
  rows.push({ key: "fees", label: "Fees paid", one: figureText(pick(fa, "fees")), two: figureText(pick(fb, "fees")) });
  for (const [key, label] of [["volatility", "Volatility"], ["sharpe", "Sharpe"], ["profit_factor", "Profit factor"]] as const) {
    const [x, y] = [pick(fa, key), pick(fb, key)];
    rows.push({ key, label, one: figureText(x), two: figureText(y), ...(reason(x) ? { noteOne: reason(x)! } : {}), ...(reason(y) ? { noteTwo: reason(y)! } : {}) });
  }
  return rows;
}
