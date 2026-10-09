import { describe, expect, it } from "vitest";
import type { BacktestRequest, RunConfig } from "../api/contract";
import { toMicros } from "../lib/decimal";
import { barAt, minutesBetween } from "./market";
import { validateBacktest } from "./mockClient";
import { DEFAULT_RISK, DEFAULT_SIMULATION, PRESET_BACKTESTS, sma } from "./presets";
import { Simulation } from "./simulation";

function run(request: BacktestRequest) {
  const config: RunConfig = { ...request, mode: "backtest" };
  const sim = new Simulation("test", config);
  for (const t of minutesBetween(Date.parse(request.period_start), Date.parse(request.period_end))) {
    sim.step(t, sim.symbols.map((s) => barAt(s, t)).filter((b) => b !== undefined));
  }
  return sim;
}

describe("demo simulation", () => {
  it("keeps equity minus starting cash equal to realized plus unrealized, to the micro", () => {
    for (const preset of PRESET_BACKTESTS) {
      const snap = run(preset).snapshot();
      const lhs = toMicros(snap.total_equity) - toMicros(preset.simulation.starting_cash);
      expect(lhs).toBe(toMicros(snap.realized_pnl) + toMicros(snap.unrealized_pnl));
      expect(toMicros(snap.buying_power)).toBe(toMicros(snap.cash) - toMicros(snap.reserved_cash));
    }
  });

  it("is deterministic", () => {
    const a = run(PRESET_BACKTESTS[0]!);
    const b = run(PRESET_BACKTESTS[0]!);
    expect(JSON.stringify(a.fills)).toBe(JSON.stringify(b.fills));
    expect(a.snapshot()).toEqual(b.snapshot());
  });

  it("produces signals, fills and risk outcomes of every kind across the presets", () => {
    const sims = PRESET_BACKTESTS.map(run);
    const outcomes = new Set(sims.flatMap((s) => s.decisions.map((d) => d.outcome)));
    expect(outcomes).toEqual(new Set(["approved", "resized", "rejected"]));
    for (const s of sims) {
      expect(s.signals.length).toBeGreaterThan(0);
      expect(s.fills.length).toBeGreaterThan(0);
    }
  });

  it("never goes short when short selling is disabled", () => {
    const sim = run(PRESET_BACKTESTS[0]!);
    expect(sim.snapshot().positions.every((p) => p.quantity >= 0)).toBe(true);
  });

  it("fills each order exactly to its approved quantity or leaves the remainder working", () => {
    const sim = run(PRESET_BACKTESTS[2]!);
    for (const order of sim.orderList()) {
      const filled = sim.fills.filter((f) => f.order_id === order.id).reduce((a, f) => a + f.filled_quantity, 0);
      expect(filled).toBe(order.filled_quantity);
      if (order.status === "filled") expect(filled).toBe(order.quantity);
      if (order.status === "rejected") expect(filled).toBe(0);
    }
  });

  it("emits an SMA buy only after the long window has warmed up", () => {
    const sim = run({ ...PRESET_BACKTESTS[0]!, strategies: [sma(["AAPL"], 5, 20)] });
    const firstBuy = sim.signals[0];
    expect(firstBuy).toBeDefined();
    const bars = sim.bars.get("AAPL")!;
    const index = bars.findIndex((b) => b.exchange_time === firstBuy!.created_at);
    expect(index).toBeGreaterThanOrEqual(20);
  });

  it("rejects invalid backtest requests with the offending field", () => {
    const base: BacktestRequest = { ...PRESET_BACKTESTS[0]!, risk: DEFAULT_RISK, simulation: DEFAULT_SIMULATION };
    expect(() => validateBacktest({ ...base, strategies: [sma(["AAPL"], 20, 5)] })).toThrow(/Long window/);
    expect(() => validateBacktest({ ...base, period_end: base.period_start })).toThrow(/after the start/);
    expect(() => validateBacktest({ ...base, period_start: "2026-12-01T13:30:00Z", period_end: "2026-12-02T20:00:00Z" })).toThrow(/Recorded data/);
  });
});
