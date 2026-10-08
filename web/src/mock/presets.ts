import type { BacktestRequest, RiskLimits, SimulationConfig, StrategyConfig } from "../api/contract";

// Defaults mirror config/config.example.json, plus a per-share fee and a participation cap.
export const DEFAULT_RISK: RiskLimits = {
  max_order_quantity: 1000,
  max_position_notional: "25000",
  max_gross_exposure: "100000",
  max_daily_loss: "5000",
  max_open_orders: 50,
  allow_short: false,
};

export const DEFAULT_SIMULATION: SimulationConfig = {
  starting_cash: "100000",
  slippage_bps: 1,
  fees: { per_share: "0.0035", per_trade: "0", bps: 0 },
  latency_ms: { signal_to_order: 1, order_to_fill: 5 },
  participation_cap: 0.01,
};

export const sma = (symbols: string[], short = 5, long = 20, quantity = 100, id = "sma_crossover"): StrategyConfig => ({
  kind: "sma_crossover",
  strategy_id: id,
  symbols,
  requested_quantity: quantity,
  short_window: short,
  long_window: long,
});

export const meanReversion = (symbols: string[], lookback = 20, entry = 2, rearm = 0.5, quantity = 50, id = "mean_reversion"): StrategyConfig => ({
  kind: "mean_reversion",
  strategy_id: id,
  symbols,
  requested_quantity: quantity,
  lookback,
  entry_threshold: entry,
  rearm_threshold: rearm,
});

const session = (date: string, end = false) => `${date}T${end ? "20:00" : "13:30"}:00Z`;

export const PRESET_BACKTESTS: BacktestRequest[] = [
  {
    name: "SMA 5/20 on large caps",
    period_start: session("2026-09-14"),
    period_end: session("2026-09-18", true),
    strategies: [sma(["AAPL", "MSFT", "SPY"])],
    risk: DEFAULT_RISK,
    simulation: DEFAULT_SIMULATION,
  },
  {
    name: "Mean reversion on SPY, two weeks",
    period_start: session("2026-09-14"),
    period_end: session("2026-09-25", true),
    strategies: [meanReversion(["SPY"], 30, 2.2, 0.4, 40)],
    risk: DEFAULT_RISK,
    simulation: DEFAULT_SIMULATION,
  },
  {
    name: "Combined book, tight limits",
    period_start: session("2026-09-28"),
    period_end: session("2026-10-02", true),
    strategies: [sma(["AAPL", "NVDA"], 10, 40, 200, "sma_10_40"), meanReversion(["MSFT", "AMZN"], 20, 2, 0.5, 60)],
    risk: { ...DEFAULT_RISK, max_position_notional: "15000", max_gross_exposure: "40000", max_order_quantity: 150, max_daily_loss: "1500" },
    simulation: { ...DEFAULT_SIMULATION, slippage_bps: 2, participation_cap: 0.002 },
  },
];

export const LIVE_CONFIG = {
  name: "Paper session",
  strategies: [sma(["AAPL", "MSFT"], 5, 20, 100), meanReversion(["SPY"], 20, 2, 0.5, 40)],
  risk: DEFAULT_RISK,
  simulation: DEFAULT_SIMULATION,
};
