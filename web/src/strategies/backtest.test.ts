import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LabError, type BacktestResponse } from "../api/lab";
import { parseBacktest, settingIssues, toSettings, DEFAULT_SETTINGS } from "./backtest";
import { backtestBlocker, backtestRelation, BacktestStore } from "./backtestStore";
import { doubleHorizon, planQuickStart } from "./compareStarts";
import { LabStore } from "./store";
import { FakeClient, MemoryStorage, readyStatus, SHA } from "./testClient";

// A real strategy_lab_backtest document: sma_crossover.csv, SMA 2/3, 10 shares, a 1.00 fee per fill. The Buy decided on
// bar 5 fills at bar 6 (no open column, so its close 4, plus 1 bp), the Sell decided on bar 8 fills at bar 9 (1, less 1 bp).
const REAL = `{"schema":"strategy_lab.backtest","schema_version":"1.0","input":{"dataset":"sma_crossover.csv","sha256":"70634f7ac4a2d233a7188108d6ba7ba9a3c93bf407c38e99d87daacaa75c5e8a","rows":9,"first_exchange_time":"2026-01-05T14:30:00.000000000Z","last_exchange_time":"2026-01-05T14:38:00.000000000Z","symbols":["AAPL"]},"config":{"starting_cash":100000,"slippage_bps":1,"fees":{"per_share":0,"per_trade":1,"bps":0},"risk_policies":true,"risk":{"max_order_quantity":1000,"max_position_notional":25000,"max_gross_exposure":100000,"max_daily_loss":5000,"max_open_orders":50},"periods_per_year":98280,"periods_per_year_source":"inferred_from_bar_spacing","fill_rule":"next_bar_open"},"strategies":[{"kind":"sma_crossover","strategy_id":"sma_crossover","parameters":{"strategy_id":"sma_crossover","short_window":2,"long_window":3,"requested_quantity":10,"symbols":["AAPL"]}}],"report":{"period_start":"2026-01-05T14:30:00.000000000Z","period_end":"2026-01-05T14:38:00.000000000Z","total_return":-0.00032005000000001615,"volatility":0.015995483366081982,"max_drawdown":-0.00032005000000001615,"profit_factor":0,"win_rate":0,"sharpe":-218.51978065887494,"trade_count":1,"winning_trades":0,"losing_trades":1,"fill_count":2,"starting_equity":100000,"ending_equity":99967.995,"gross_profit":0,"gross_loss":32.005,"fees_paid":2},"counts":{"bars_accepted":9,"bars_rejected":0,"signals":2,"approved":2,"resized":0,"rejected":0,"orders":2,"fills":2,"open_orders_at_end":0,"strategy_errors":0,"portfolio_errors":0},"final":{"cash":99967.995,"equity":99967.995,"gross_exposure":0,"reserved_cash":0,"positions":[{"symbol":"AAPL","quantity":0,"average_cost":0,"realized_pnl":-32.005,"unrealized_pnl":0}]},"equity":[{"time":"2026-01-05T14:30:00.000000000Z","equity":100000,"cash":100000,"gross_exposure":0,"drawdown":0},{"time":"2026-01-05T14:31:00.000000000Z","equity":100000,"cash":100000,"gross_exposure":0,"drawdown":0},{"time":"2026-01-05T14:32:00.000000000Z","equity":100000,"cash":100000,"gross_exposure":0,"drawdown":0},{"time":"2026-01-05T14:33:00.000000000Z","equity":100000,"cash":100000,"gross_exposure":0,"drawdown":0},{"time":"2026-01-05T14:34:00.000000000Z","equity":100000,"cash":100000,"gross_exposure":0,"drawdown":0},{"time":"2026-01-05T14:35:00.000000000Z","equity":99998.996,"cash":99958.996,"gross_exposure":40,"drawdown":-1.0040000000044458e-05},{"time":"2026-01-05T14:36:00.000000000Z","equity":99988.996,"cash":99958.996,"gross_exposure":30,"drawdown":-0.00011004000000003344},{"time":"2026-01-05T14:37:00.000000000Z","equity":99978.996,"cash":99958.996,"gross_exposure":20,"drawdown":-0.00021004000000002243},{"time":"2026-01-05T14:38:00.000000000Z","equity":99967.995,"cash":99967.995,"gross_exposure":0,"drawdown":-0.00032005000000001615}],"fills":[{"fill_id":1,"order_id":1,"strategy_id":"sma_crossover","signal_id":1,"symbol":"AAPL","side":"buy","quantity":10,"price":4.0004,"fees":1,"slippage":0.0004,"time":"2026-01-05T14:35:00.000000000Z"},{"fill_id":2,"order_id":2,"strategy_id":"sma_crossover","signal_id":2,"symbol":"AAPL","side":"sell","quantity":10,"price":0.9999,"fees":1,"slippage":-0.0001,"time":"2026-01-05T14:38:00.000000000Z"}],"orders":[{"order_id":1,"signal_id":1,"strategy_id":"sma_crossover","symbol":"AAPL","side":"buy","quantity":10,"filled_quantity":10,"status":"filled","average_fill_price":4.0004,"created_at":"2026-01-05T14:34:00.000000000Z"},{"order_id":2,"signal_id":2,"strategy_id":"sma_crossover","symbol":"AAPL","side":"sell","quantity":10,"filled_quantity":10,"status":"filled","average_fill_price":0.9999,"created_at":"2026-01-05T14:37:00.000000000Z"}],"decisions":[{"signal_id":1,"strategy_id":"sma_crossover","symbol":"AAPL","side":"buy","requested_quantity":10,"time":"2026-01-05T14:34:00.000000000Z","approved":true,"reason":"ok"},{"signal_id":2,"strategy_id":"sma_crossover","symbol":"AAPL","side":"sell","requested_quantity":10,"time":"2026-01-05T14:37:00.000000000Z","approved":true,"reason":"ok"}]}`;
const withSha = (sha: string) => REAL.replace(/"sha256":"[0-9a-f]{64}"/, `"sha256":"${sha}"`);

describe("reading a backtest document", () => {
  it("keeps money and quantities exact and reads every section", () => {
    const doc = parseBacktest(REAL);
    expect(doc.config.startingCash).toBe("100000");
    expect(doc.config.feePerTrade).toBe("1");
    expect(doc.config.fillRule).toBe("next_bar_open");
    expect(doc.strategies).toEqual([{ kind: "sma_crossover", strategyId: "sma_crossover" }]);
    expect(doc.fills.map((f) => [f.side, f.quantity, f.price, f.fees])).toEqual([["buy", "10", "4.0004", "1"], ["sell", "10", "0.9999", "1"]]);
    expect(doc.report.endingEquity).toBe("99967.995");
    expect(doc.report.grossLoss).toBe("32.005");
    expect(doc.report.tradeCount).toBe(1);
    expect(doc.report.losingTrades).toBe(1);
    expect(doc.report.profitFactor).toBe(0);
    expect(doc.final.cash).toBe("99967.995");
    expect(doc.equity).toHaveLength(9);
    expect(doc.decisions.every((d) => d.approved)).toBe(true);
  });

  it("refuses another document type or major version", () => {
    expect(() => parseBacktest('{"schema":"strategy_lab.replay","schema_version":"1.0"}')).toThrow(LabError);
    expect(() => parseBacktest(REAL.replace('"schema_version":"1.0"', '"schema_version":"2.0"'))).toThrow(/Unsupported/);
    expect(() => parseBacktest("not json")).toThrow(/not valid JSON/);
  });
});

describe("settings", () => {
  it("sends only what was typed, as text, and flags what the tool would refuse", () => {
    expect(toSettings(DEFAULT_SETTINGS)).toEqual({});
    expect(toSettings({ risk_policies: false, cash: " 5000 ", fee_bps: "" })).toEqual({ cash: "5000", risk_policies: false });
    expect(settingIssues({ risk_policies: true, cash: "-1", max_open_orders: "2.5", slippage_bps: "0.5" }).map((i) => i.name)).toEqual(["cash", "max_open_orders"]);
  });
});

describe("the backtest store", () => {
  let client: FakeClient;
  let lab: LabStore;
  beforeEach(async () => {
    vi.useFakeTimers();
    client = new FakeClient();
    client.statusResult = { ...readyStatus(), backtest: { state: "ready", name: "strategy_lab_backtest", sha256: SHA } };
    lab = new LabStore(client, { storage: new MemoryStorage(), checkDebounceMs: 0 });
    await lab.connect();
    await vi.runAllTimersAsync();
  });
  afterEach(() => vi.useRealTimers());

  const answer = (raw: string): BacktestResponse => ({
    meta: { gateway: { version: "0.1.0", api: "1" }, runner: { name: "strategy_lab_backtest", size: 1, sha256: SHA },
      dataset: { kind: "builtin", name: "sma_crossover.csv", sha256: SHA, bytes: 1, synthetic: true, provenance: "" },
      request: { kind: "sma_crossover", params: [], settings: [], max_rows: 20000 },
      process: { exit_code: 0, duration_ms: 1, args_file: "", stderr: "", stderr_truncated: false } },
    raw,
  });

  it("runs only on request, sends the draft and settings, and marks the result stale when either changes", async () => {
    const bt = new BacktestStore(client);
    client.backtestImpl = async () => answer(withSha(SHA));
    expect(client.backtests).toHaveLength(0);
    bt.setSetting("cash", "2500");
    expect(await bt.run(lab.getState())).toBe(true);
    expect(client.backtests).toHaveLength(1);
    expect(client.backtests[0]!.settings).toEqual({ cash: "2500" });
    expect(client.backtests[0]!.strategy.kind).toBe("sma_crossover");
    expect(backtestRelation(lab.getState(), bt.getState())).toBe("current");
    bt.setSetting("cash", "3000");
    expect(backtestRelation(lab.getState(), bt.getState())).toBe("stale");
  });

  it("refuses a result for another dataset and keeps the error", async () => {
    const bt = new BacktestStore(client);
    client.backtestImpl = async () => answer(withSha("c3".repeat(32)));
    expect(await bt.run(lab.getState())).toBe(false);
    expect(bt.getState().run.phase).toBe("failed");
    expect(bt.getState().result).toBeUndefined();
  });

  it("explains a missing backtest tool and invalid settings", () => {
    const bt = new BacktestStore(client);
    const state = lab.getState();
    const missing = { ...state, service: { ...state.service, status: { ...state.service.status!, backtest: { state: "missing" as const, message: "Not built." } } } };
    expect(backtestBlocker(missing, bt.getState())).toMatch(/strategy_lab_backtest/);
    bt.setSetting("cash", "lots");
    expect(backtestBlocker(state, bt.getState())).toMatch(/backtest settings/);
  });
});

describe("ML quick starts", () => {
  it("doubles the horizon exactly and only for the ML strategy", () => {
    expect(doubleHorizon({ kind: "ml_logistic", values: { horizon: "9007199254740993" } })!.values.horizon).toBe("18014398509481986");
    expect(doubleHorizon({ kind: "sma_crossover", values: { horizon: "3" } })).toBeUndefined();
  });

  it("says when the engine has no ML strategy", () => {
    expect(planQuickStart("ml_vs_crossover", undefined, undefined)).toEqual({ error: "The engine does not offer the ML (logistic regression) strategy." });
  });
});
