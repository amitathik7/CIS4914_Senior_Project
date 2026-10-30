// The full backtest (strategy_lab_backtest through the gateway's /backtest): reading its strategy_lab.backtest document
// (docs/BACKTESTING.md section 5) into typed values, and the settings form.
//
// Money and prices stay exact decimal text (the tool writes them as JSON numbers with at most six places, read losslessly);
// quantities are int64 text; only derived statistics (returns, drawdown, Sharpe) become doubles.

import { LabError, type BacktestSettings } from "../api/lab";
import { decimalText, derivedNumber, integerText, isObject, parseLossless, safeInteger, type LValue } from "../lib/losslessJson";

export interface BacktestFill {
  fillId: number;
  orderId: number;
  signalId?: number;
  strategyId: string;
  symbol: string;
  side: "buy" | "sell";
  quantity: string;
  price: string;
  fees: string;
  slippage: string;
  time: string;
}

export interface BacktestDecision {
  signalId: number;
  strategyId: string;
  symbol: string;
  side: string;
  requested: string;
  time: string;
  approved: boolean;
  adjusted?: string;
  reason: string;
  policy?: string;
}

export interface BacktestPosition {
  symbol: string;
  quantity: string;
  averageCost: string;
  realizedPnl: string;
  unrealizedPnl: string;
}

export interface EquityPoint {
  time: string;
  equity: string;
  cash: string;
  drawdown: number;
}

export interface BacktestReport {
  periodStart: string;
  periodEnd: string;
  totalReturn: number;
  volatility: number;
  maxDrawdown: number;
  profitFactor?: number;
  profitFactorStatus?: string;
  winRate?: number;
  sharpe?: number;
  tradeCount: number;
  winningTrades: number;
  losingTrades: number;
  fillCount: number;
  startingEquity: string;
  endingEquity: string;
  grossProfit: string;
  grossLoss: string;
  feesPaid: string;
}

export interface BacktestDoc {
  dataset: { name: string; sha256: string; rows: number; symbols: string[] };
  config: { startingCash: string; slippageBps: number; feePerShare: string; feePerTrade: string; feeBps: number; riskPolicies: boolean; periodsPerYear: number; periodsSource: string; fillRule: string };
  strategies: { kind: string; strategyId: string }[];
  report: BacktestReport;
  counts: { signals: number; approved: number; resized: number; rejected: number; orders: number; fills: number; openOrdersAtEnd: number; barsRejected: number };
  final: { cash: string; equity: string; grossExposure: string; positions: BacktestPosition[] };
  equity: EquityPoint[];
  fills: BacktestFill[];
  decisions: BacktestDecision[];
}

const bad = (what: string) => new LabError("malformed_output", `The backtest document is not readable: ${what}.`);

type Obj = { [key: string]: LValue };
const obj = (v: LValue | undefined, what: string): Obj => {
  if (!isObject(v)) throw bad(`${what} is not an object`);
  return v;
};
const arr = (v: LValue | undefined, what: string): LValue[] => {
  if (!Array.isArray(v)) throw bad(`${what} is not a list`);
  return v;
};
const str = (o: Obj, k: string): string => {
  const v = o[k];
  if (typeof v !== "string") throw bad(`${k} is missing`);
  return v;
};
const optStr = (o: Obj, k: string): string | undefined => (typeof o[k] === "string" ? (o[k] as string) : undefined);
const dec = (o: Obj, k: string): string => {
  const v = o[k];
  const text = v === undefined ? undefined : decimalText(v);
  if (text === undefined) throw bad(`${k} is not an exact decimal`);
  return text;
};
const int = (o: Obj, k: string): string => {
  const v = o[k];
  const text = v === undefined ? undefined : integerText(v);
  if (text === undefined) throw bad(`${k} is not an integer`);
  return text;
};
const count = (o: Obj, k: string): number => {
  const v = o[k];
  const n = v === undefined ? undefined : safeInteger(v);
  if (n === undefined) throw bad(`${k} is not a count`);
  return n;
};
const num = (o: Obj, k: string): number => {
  const v = o[k];
  const n = v === undefined ? undefined : derivedNumber(v);
  if (n === undefined) throw bad(`${k} is not a number`);
  return n.value;
};
const optNum = (o: Obj, k: string): number | undefined => (o[k] === undefined ? undefined : num(o, k));
const bool = (o: Obj, k: string): boolean => {
  if (typeof o[k] !== "boolean") throw bad(`${k} is not true or false`);
  return o[k] as boolean;
};

export function parseBacktest(raw: string): BacktestDoc {
  let root: LValue;
  try {
    root = parseLossless(raw);
  } catch (error) {
    throw new LabError("malformed_output", `The backtest document is not valid JSON: ${(error as Error).message}`);
  }
  const doc = obj(root, "the document");
  if (doc.schema !== "strategy_lab.backtest") throw new LabError("unsupported_schema", "The service did not answer with a strategy_lab.backtest document.");
  if (typeof doc.schema_version !== "string" || doc.schema_version.split(".")[0] !== "1") {
    throw new LabError("unsupported_schema", `Unsupported backtest document version ${String(doc.schema_version)}.`);
  }
  const input = obj(doc.input, "input");
  const config = obj(doc.config, "config");
  const fees = obj(config.fees, "config.fees");
  const report = obj(doc.report, "report");
  const counts = obj(doc.counts, "counts");
  const final = obj(doc.final, "final");
  return {
    dataset: { name: str(input, "dataset"), sha256: str(input, "sha256"), rows: count(input, "rows"), symbols: arr(input.symbols, "symbols").map(String) },
    config: {
      startingCash: dec(config, "starting_cash"), slippageBps: num(config, "slippage_bps"), feePerShare: dec(fees, "per_share"),
      feePerTrade: dec(fees, "per_trade"), feeBps: num(fees, "bps"), riskPolicies: bool(config, "risk_policies"),
      periodsPerYear: count(config, "periods_per_year"), periodsSource: str(config, "periods_per_year_source"), fillRule: str(config, "fill_rule"),
    },
    strategies: arr(doc.strategies, "strategies").map((s) => {
      const o = obj(s, "strategy");
      return { kind: str(o, "kind"), strategyId: str(o, "strategy_id") };
    }),
    report: {
      periodStart: str(report, "period_start"), periodEnd: str(report, "period_end"), totalReturn: num(report, "total_return"),
      volatility: num(report, "volatility"), maxDrawdown: num(report, "max_drawdown"), profitFactor: optNum(report, "profit_factor"),
      profitFactorStatus: optStr(report, "profit_factor_status"), winRate: optNum(report, "win_rate"), sharpe: optNum(report, "sharpe"),
      tradeCount: count(report, "trade_count"), winningTrades: count(report, "winning_trades"), losingTrades: count(report, "losing_trades"),
      fillCount: count(report, "fill_count"), startingEquity: dec(report, "starting_equity"), endingEquity: dec(report, "ending_equity"),
      grossProfit: dec(report, "gross_profit"), grossLoss: dec(report, "gross_loss"), feesPaid: dec(report, "fees_paid"),
    },
    counts: {
      signals: count(counts, "signals"), approved: count(counts, "approved"), resized: count(counts, "resized"), rejected: count(counts, "rejected"),
      orders: count(counts, "orders"), fills: count(counts, "fills"), openOrdersAtEnd: count(counts, "open_orders_at_end"), barsRejected: count(counts, "bars_rejected"),
    },
    final: {
      cash: dec(final, "cash"), equity: dec(final, "equity"), grossExposure: dec(final, "gross_exposure"),
      positions: arr(final.positions, "positions").map((p) => {
        const o = obj(p, "position");
        return { symbol: str(o, "symbol"), quantity: int(o, "quantity"), averageCost: dec(o, "average_cost"), realizedPnl: dec(o, "realized_pnl"), unrealizedPnl: dec(o, "unrealized_pnl") };
      }),
    },
    equity: arr(doc.equity, "equity").map((e) => {
      const o = obj(e, "equity point");
      return { time: str(o, "time"), equity: dec(o, "equity"), cash: dec(o, "cash"), drawdown: optNum(o, "drawdown") ?? 0 };
    }),
    fills: arr(doc.fills, "fills").map((f) => {
      const o = obj(f, "fill");
      const side = str(o, "side");
      if (side !== "buy" && side !== "sell") throw bad("a fill side is neither buy nor sell");
      return {
        fillId: count(o, "fill_id"), orderId: count(o, "order_id"), signalId: o.signal_id === undefined ? undefined : count(o, "signal_id"),
        strategyId: optStr(o, "strategy_id") ?? "", symbol: str(o, "symbol"), side, quantity: int(o, "quantity"), price: dec(o, "price"),
        fees: dec(o, "fees"), slippage: dec(o, "slippage"), time: str(o, "time"),
      };
    }),
    decisions: arr(doc.decisions, "decisions").map((d) => {
      const o = obj(d, "decision");
      return {
        signalId: count(o, "signal_id"), strategyId: str(o, "strategy_id"), symbol: str(o, "symbol"), side: str(o, "side"),
        requested: int(o, "requested_quantity"), time: str(o, "time"), approved: bool(o, "approved"),
        adjusted: o.adjusted_quantity === undefined ? undefined : int(o, "adjusted_quantity"), reason: str(o, "reason"), policy: optStr(o, "policy"),
      };
    }),
  };
}

// ---- the settings form ---------------------------------------------------------------------------------------------------

export type SettingName = Exclude<keyof BacktestSettings, "risk_policies">;

export interface SettingField {
  name: SettingName;
  label: string;
  help: string;
  /** The engine's default (config::SimulationConfig / config::RiskLimits), shown as the placeholder. */
  placeholder: string;
  group: "account" | "costs" | "risk";
  whole?: boolean;
}

export const SETTING_FIELDS: readonly SettingField[] = [
  { name: "cash", label: "Starting cash", help: "Account currency.", placeholder: "100000", group: "account" },
  { name: "slippage_bps", label: "Slippage (bps)", help: "Moves every fill against the order: buys pay more, sells receive less.", placeholder: "1", group: "costs" },
  { name: "fee_per_trade", label: "Fee per fill", help: "A fixed charge on every execution.", placeholder: "0", group: "costs" },
  { name: "fee_per_share", label: "Fee per share", help: "Charged per share executed.", placeholder: "0", group: "costs" },
  { name: "fee_bps", label: "Fee (bps of notional)", help: "A share of the executed value.", placeholder: "0", group: "costs" },
  { name: "max_order_quantity", label: "Max order size", help: "Larger requests are resized down to it.", placeholder: "1000", group: "risk", whole: true },
  { name: "max_position_notional", label: "Max position value", help: "A buy that would make one position worth more is rejected.", placeholder: "25000", group: "risk" },
  { name: "max_gross_exposure", label: "Max gross exposure", help: "A buy that would take total exposure above it is rejected.", placeholder: "100000", group: "risk" },
  { name: "max_daily_loss", label: "Max daily loss", help: "Buys stop for the rest of the day once the day's loss reaches it.", placeholder: "5000", group: "risk" },
  { name: "max_open_orders", label: "Max open orders", help: "No new order while this many are still working.", placeholder: "50", group: "risk", whole: true },
];

const PLAIN = /^\d*\.?\d+$/;
const WHOLE = /^\d+$/;

/** The text of each field; an empty field means "the engine's default". */
export type SettingsDraft = Partial<Record<SettingName, string>> & { risk_policies: boolean };

export const DEFAULT_SETTINGS: SettingsDraft = { risk_policies: true };

/** Problems the form can see before sending (the tool checks the rest and explains in its own words). */
export function settingIssues(draft: SettingsDraft): { name: SettingName; message: string }[] {
  const out: { name: SettingName; message: string }[] = [];
  for (const f of SETTING_FIELDS) {
    const text = (draft[f.name] ?? "").trim();
    if (text === "") continue;
    if (f.whole ? !WHOLE.test(text) : !PLAIN.test(text)) out.push({ name: f.name, message: f.whole ? "A whole number, such as 100." : "A plain number, such as 100 or 0.5." });
  }
  return out;
}

export function toSettings(draft: SettingsDraft): BacktestSettings {
  const out: BacktestSettings = {};
  for (const f of SETTING_FIELDS) {
    const text = (draft[f.name] ?? "").trim();
    if (text !== "") out[f.name] = text;
  }
  if (!draft.risk_policies) out.risk_policies = false;
  return out;
}
