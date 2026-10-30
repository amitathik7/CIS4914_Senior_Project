import { describe, expect, it } from "vitest";
import type { PerformanceReport, RunDetail } from "../api/contract";
import { reportFigures, winRateText } from "../lib/reportFigures";
import { compareRecorded, metricRows, netPnl, VERDICT_TEXT } from "./recordedCompare";

const report = (extra: Partial<PerformanceReport> = {}): PerformanceReport => ({
  run_id: "r", generated_at: "2026-09-25T20:00:00Z", period_start: "2026-09-21T13:30:00Z", period_end: "2026-09-25T20:00:00Z", total_return: 0.0125, volatility: 0.08,
  max_drawdown: -0.021, profit_factor: 1.8, trade_count: 12, win_rate: 0.58, sharpe: 1.1, ending_equity: "101250.5", fees_paid: "42.35", ...extra,
});

function run(id: string, patch: Partial<RunDetail> = {}, config: Partial<RunDetail["config"]> = {}): RunDetail {
  return {
    run_id: id, name: id, mode: "backtest", status: "completed", created_at: "2026-09-26T00:00:00Z", period_start: "2026-09-21T13:30:00Z", period_end: "2026-09-25T20:00:00Z",
    symbols: ["AAPL"], strategies: ["s"], report: report({ run_id: id }), kill_switch: { engaged: false },
    config: {
      name: id, mode: "backtest", period_start: "2026-09-21T13:30:00Z", period_end: "2026-09-25T20:00:00Z",
      strategies: [{ kind: "sma_crossover", strategy_id: "s", symbols: ["AAPL"], requested_quantity: 100, short_window: 5, long_window: 20 }],
      risk: { max_order_quantity: 1000, max_position_notional: "25000", max_gross_exposure: "100000", max_daily_loss: "5000", max_open_orders: 50, allow_short: false },
      simulation: { starting_cash: "100000", slippage_bps: 1, fees: { per_share: "0.0035", per_trade: "0", bps: 0 }, latency_ms: { signal_to_order: 1, order_to_fill: 5 }, participation_cap: 0.01 },
      ...config,
    },
    ...patch,
  };
}

describe("when two recorded backtests may be compared", () => {
  it("accepts runs set up the same way, and still cannot vouch for the market data", () => {
    const result = compareRecorded(run("one"), run("two"), "engine");
    expect(result.verdict).toBe("settings_match");
    expect(result.differences).toEqual([]);
    expect(result.checks.every((c) => c.ok)).toBe(true);
    expect(result.cautions.join(" ")).toMatch(/no fingerprint of the market data/);
    expect(result.cautions.join(" ")).not.toMatch(/demo/);
  });

  it("never words a settings match as established comparability: the data behind the runs is unverifiable", () => {
    expect(VERDICT_TEXT.settings_match.badge).toMatch(/^Unverified/);
    expect(VERDICT_TEXT.settings_match.explain).toMatch(/cannot be checked.*not shown to be comparable/);
    expect(`${VERDICT_TEXT.settings_match.badge} ${VERDICT_TEXT.settings_match.explain}`).not.toMatch(/same settings|are comparable|is comparable|comparable\.$/i);
    expect(VERDICT_TEXT.not_comparable.badge).toBe("Not comparable");
  });

  it("says when the runs come from the demo simulator", () => {
    expect(compareRecorded(run("one"), run("two"), "demo").cautions.join(" ")).toMatch(/in-browser demo simulator/);
  });

  it("flags every setting that differs, and does not call the runs comparable", () => {
    const other = run("two", {}, {
      period_end: "2026-09-24T20:00:00Z",
      simulation: { starting_cash: "50000", slippage_bps: 3, fees: { per_share: "0.01", per_trade: "0", bps: 0 }, latency_ms: { signal_to_order: 1, order_to_fill: 5 }, participation_cap: 0.01 },
      risk: { max_order_quantity: 500, max_position_notional: "25000", max_gross_exposure: "100000", max_daily_loss: "5000", max_open_orders: 50, allow_short: false },
    });
    const result = compareRecorded(run("one"), other, "demo");
    expect(result.verdict).toBe("not_comparable");
    expect(result.checks.filter((c) => !c.ok).map((c) => c.key)).toEqual(["period", "cash", "fees", "slippage", "risk"]);
    expect(result.differences.join(" | ")).toMatch(/evaluation period/);
    expect(result.differences.join(" | ")).toMatch(/starting capital: \$100,000\.00 \/ \$50,000\.00/);
  });

  it("reads decimals by value, not by spelling", () => {
    const spelled = run("two", {}, { simulation: { starting_cash: "100000.00", slippage_bps: 1, fees: { per_share: "0.003500", per_trade: "0", bps: 0 }, latency_ms: { signal_to_order: 1, order_to_fill: 5 }, participation_cap: 0.01 } });
    expect(compareRecorded(run("one"), spelled, "engine").verdict).toBe("settings_match");
  });

  it("refuses a live or unfinished run, and different symbols", () => {
    expect(compareRecorded(run("one"), run("two", { mode: "live" }), "engine").checks.find((c) => c.key === "finished")).toMatchObject({ ok: false, two: "a live run" });
    expect(compareRecorded(run("one"), run("two", { status: "running", report: undefined }), "engine").checks[0]).toMatchObject({ ok: false, two: "running" });
    const msft = run("two", {}, { strategies: [{ kind: "sma_crossover", strategy_id: "s", symbols: ["MSFT"], requested_quantity: 100, short_window: 5, long_window: 20 }] });
    expect(compareRecorded(run("one"), msft, "engine").differences.join(" ")).toMatch(/symbols traded: AAPL \/ MSFT/);
  });

  it("will not attribute a multi-strategy portfolio to one strategy", () => {
    const two = run("two", {}, { strategies: [
      { kind: "sma_crossover", strategy_id: "a", symbols: ["AAPL"], requested_quantity: 100, short_window: 5, long_window: 20 },
      { kind: "mean_reversion", strategy_id: "b", symbols: ["AAPL"], requested_quantity: 50, lookback: 20, entry_threshold: 2, rearm_threshold: 0.5 },
    ] });
    const result = compareRecorded(run("one"), two, "engine");
    expect(result.cautions.join(" ")).toMatch(/Run 2 traded 2 strategies in one portfolio.*cannot be attributed to one strategy/);
    expect(result.cautions.join(" ")).not.toMatch(/Run 1 traded/);
  });

  it("never ranks the runs", () => {
    const text = JSON.stringify(compareRecorded(run("one"), run("two"), "demo"));
    expect(text).not.toMatch(/\b(better|worse|winner|best|outperform)/i);
  });
});

describe("the metrics, in the Report page's own definitions", () => {
  it("lists net P&L (exact), return, drawdown, trades, win rate and fees", () => {
    const one = run("one");
    const two = run("two", { report: report({ total_return: -0.004, ending_equity: "99600.25", trade_count: 3, win_rate: 0.3333, fees_paid: "9" }) });
    const rows = metricRows(one, two, [netPnl(one), netPnl(two)]);
    const by = (key: string) => rows.find((r) => r.key === key)!;
    expect(netPnl(one)).toBe("1250.5");
    expect(netPnl(two)).toBe("-399.75");
    expect([by("pnl").one, by("pnl").two]).toEqual(["+$1,250.50", "−$399.75"]);
    expect([by("total_return").one, by("total_return").two]).toEqual(["+1.25%", "−0.40%"]);
    expect([by("max_drawdown").one, by("trades").two, by("win_rate").two, by("fees").two]).toEqual(["−2.10%", "3", "33%", "$9.00"]);
  });

  it("keeps Sharpe, profit factor and win rate unavailable, with the reason, when their prerequisites are not met", () => {
    const thin = run("two", { report: report({ sharpe: undefined, profit_factor: undefined, profit_factor_status: "no_losing_trades", win_rate: undefined, trade_count: 0 }) });
    const rows = metricRows(run("one"), thin, [undefined, netPnl(thin)]);
    const by = (key: string) => rows.find((r) => r.key === key)!;
    expect(by("sharpe")).toMatchObject({ one: "1.10", two: "—", noteTwo: "needs 3+ sessions" });
    expect(by("profit_factor")).toMatchObject({ one: "1.80", two: "—", noteTwo: "no losing trades" });
    expect(by("win_rate")).toMatchObject({ one: "58%", two: "—", noteTwo: "no closed round trip yet" });
    expect(by("sharpe").noteOne).toBeUndefined();
    expect(by("pnl").one).toBe("—"); // no report: a dash, never $0.00
  });

  it("is the same definition the Report page shows", () => {
    const r = report({ sharpe: undefined, profit_factor: undefined, profit_factor_status: "no_trades" });
    const figures = reportFigures(r);
    expect(figures.map((f) => [f.label, f.value, f.sub])).toEqual([
      ["Total return", "+1.25%", "ending $101,250.50"], ["Max drawdown", "−2.10%", "peak to trough"], ["Volatility", "8.0%", "annualised"],
      ["Sharpe", "—", "needs 3+ sessions"], ["Profit factor", "—", "no closed trades"], ["Trades", "12", "58% winners"], ["Fees", "$42.35", undefined],
    ]);
    expect(figures[0]).toMatchObject({ tone: "up", lead: true });
    expect(winRateText(report({ win_rate: undefined }))).toEqual({ value: "—", sub: "no closed round trip yet", available: false });
  });
});
