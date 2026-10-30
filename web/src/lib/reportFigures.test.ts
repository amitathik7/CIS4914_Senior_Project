import { describe, expect, it } from "vitest";
import type { PerformanceReport } from "../api/contract";
import { money } from "./decimal";
import { int, percent, ratioText } from "./format";
import { reportFigures, winRateText } from "./reportFigures";

const report = (extra: Partial<PerformanceReport> = {}): PerformanceReport => ({
  run_id: "r", generated_at: "2026-09-25T20:00:00Z", period_start: "2026-09-21T13:30:00Z", period_end: "2026-09-25T20:00:00Z", total_return: 0.0125, volatility: 0.08,
  max_drawdown: -0.021, profit_factor: 1.8, trade_count: 12, win_rate: 0.58, sharpe: 1.1, ending_equity: "101250.5", fees_paid: "42.35", ...extra,
});

/** The figures exactly as the Report page wrote them inline before they were moved to reportFigures.ts (copied from that page). */
function asTheReportPageWroteThem(r: PerformanceReport) {
  const tone = (n: number) => (n > 0 ? "up" : n < 0 ? "down" : undefined);
  return [
    { label: "Total return", value: percent(r.total_return), tone: tone(r.total_return), sub: `ending ${money(r.ending_equity)}`, lead: true },
    { label: "Max drawdown", value: percent(r.max_drawdown), sub: "peak to trough" },
    { label: "Volatility", value: percent(r.volatility, 1, false), sub: "annualised" },
    { label: "Sharpe", value: ratioText(r.sharpe), sub: r.sharpe !== undefined ? "from daily returns" : "needs 3+ sessions" },
    {
      label: "Profit factor", value: r.profit_factor !== undefined ? ratioText(r.profit_factor) : "—",
      sub: r.profit_factor_status === "no_losing_trades" ? "no losing trades" : r.profit_factor_status === "no_trades" ? "no closed trades" : "gross win / gross loss",
    },
    { label: "Trades", value: int(r.trade_count), sub: r.win_rate !== undefined ? `${percent(r.win_rate, 0, false)} winners` : "closed round trips" },
    { label: "Fees", value: money(r.fees_paid) },
  ];
}

const shown = (r: PerformanceReport) => reportFigures(r).map(({ label, value, tone, sub, lead }) => ({ label, value, tone, sub, lead }));

/** A report with the named optional figures absent (the engine omits them; passing undefined would still count as present). */
const without = (r: PerformanceReport, ...keys: (keyof PerformanceReport)[]): PerformanceReport => {
  const copy: Record<string, unknown> = { ...r };
  for (const key of keys) delete copy[key];
  return copy as unknown as PerformanceReport;
};

describe("the Report page's figures, now defined once", () => {
  const cases: [string, PerformanceReport][] = [
    ["a profitable run with every figure", report()],
    ["a losing run with no Sharpe (under three sessions) and no losing trade", without(report({ total_return: -0.034, profit_factor_status: "no_losing_trades" }), "sharpe", "profit_factor")],
    ["a run with no trades at all", without(report({ total_return: 0, max_drawdown: 0, trade_count: 0, profit_factor_status: "no_trades", fees_paid: "0" }), "sharpe", "profit_factor", "win_rate")],
    ["a run with a profit factor but no win rate and no status", without(report(), "win_rate")],
  ];

  it.each(cases)("is word for word what the page showed: %s", (_name, r) => {
    expect(shown(r)).toEqual(asTheReportPageWroteThem(r));
  });

  it("keeps a figure that does not exist a dash with its reason, never zero", () => {
    const r = without(report({ profit_factor_status: "no_losing_trades", trade_count: 0 }), "sharpe", "profit_factor", "win_rate");
    const by = (key: string) => reportFigures(r).find((f) => f.key === key)!;
    expect(by("sharpe")).toMatchObject({ value: ratioText(undefined), sub: "needs 3+ sessions", available: false });
    expect(by("profit_factor")).toMatchObject({ value: "—", sub: "no losing trades", available: false });
    expect(by("total_return").available).toBe(true);
    expect(winRateText(r)).toEqual({ value: "—", sub: "no closed round trip yet", available: false });
    expect(winRateText(report())).toMatchObject({ value: percent(0.58, 0, false), available: true });
  });

  it("colours a gain or a loss, and nothing at zero (the sign is in the text as well)", () => {
    const tone = (total_return: number) => reportFigures(report({ total_return }))[0]!.tone;
    expect([tone(0.01), tone(-0.01), tone(0)]).toEqual(["up", "down", undefined]);
  });
});
