// Wire contract between the engine and the web console (docs/adr/0006-web-frontend-api-contract.md).
// Field names and enum spellings follow the C++ domain types and their to_string() output.
//
// Encoding rules:
//   Decimal   exact decimal text, at most 6 places, never an exponent (common::Decimal / Money / Price)
//   Quantity  JSON integer, whole shares (common::Quantity); signed where the C++ field is signed
//   Timestamp RFC 3339 UTC with a Z suffix
//   Absent optionals are omitted, never null.

export type Decimal = string;
export type Quantity = number;
export type Timestamp = string;

export type RunMode = "backtest" | "live";
export type RunStatus = "queued" | "running" | "completed" | "failed" | "stopped";
export type DataSource = "engine" | "demo";

export interface EngineInfo {
  project_version: string;
  data_source: DataSource;
  api_version: "1";
  market_data: { symbols: string[]; first_session: string; last_session: string }; // dates as YYYY-MM-DD
}

export interface StrategyConfig {
  kind: "sma_crossover" | "mean_reversion";
  strategy_id: string;
  symbols: string[];
  requested_quantity: Quantity;
  // sma_crossover
  short_window?: number;
  long_window?: number;
  // mean_reversion
  lookback?: number;
  entry_threshold?: number;
  rearm_threshold?: number;
}

export interface RiskLimits {
  max_order_quantity: Quantity;
  max_position_notional: Decimal;
  max_gross_exposure: Decimal;
  max_daily_loss: Decimal;
  max_open_orders: number;
  allow_short: boolean;
}

export interface SimulationConfig {
  starting_cash: Decimal;
  slippage_bps: number;
  fees: { per_share: Decimal; per_trade: Decimal; bps: number };
  latency_ms: { signal_to_order: number; order_to_fill: number };
  participation_cap: number; // max fraction of a bar's volume one order may take per bar
}

export interface RunConfig {
  name: string;
  mode: RunMode;
  period_start: Timestamp;
  period_end?: Timestamp; // omitted for an open-ended live run
  strategies: StrategyConfig[];
  risk: RiskLimits;
  simulation: SimulationConfig;
}

export interface PerformanceReport {
  run_id: string;
  generated_at: Timestamp;
  period_start: Timestamp;
  period_end: Timestamp;
  total_return: number;    // fractional, 0.12 == +12%
  volatility: number;      // annualised std-dev of bar returns
  max_drawdown: number;    // most negative peak-to-trough, <= 0
  profit_factor?: number;  // omitted when there is no losing trade (see profit_factor_status)
  profit_factor_status?: "no_losing_trades" | "no_trades";
  trade_count: number;     // closed round trips
  win_rate?: number;
  sharpe?: number;        // annualised from session-close returns; omitted under 3 sessions
  ending_equity: Decimal;
  fees_paid: Decimal;
}

export interface RunSummary {
  run_id: string;
  name: string;
  mode: RunMode;
  status: RunStatus;
  progress?: number; // 0..1 while running a backtest
  created_at: Timestamp;
  period_start: Timestamp;
  period_end?: Timestamp;
  clock?: Timestamp; // the run's simulated "now"
  symbols: string[];
  strategies: string[]; // strategy_ids
  report?: PerformanceReport;
  error?: string;
}

export interface RunDetail extends RunSummary {
  config: RunConfig;
  kill_switch: { engaged: boolean; changed_at?: Timestamp; reason?: string };
}

export interface Bar {
  symbol: string;
  exchange_time: Timestamp; // bar open time
  open: Decimal;
  high: Decimal;
  low: Decimal;
  price: Decimal; // close
  volume: Quantity;
  sequence: number;
}

export type SignalSide = "buy" | "sell" | "flat";
export type OrderSide = "buy" | "sell";
export type OrderType = "market" | "limit";
export type OrderStatus =
  | "new" | "pending_risk" | "rejected" | "approved" | "working"
  | "partially_filled" | "filled" | "cancelled" | "expired";

export interface TradeSignal {
  id: number;
  strategy_id: string;
  symbol: string;
  side: SignalSide;
  created_at: Timestamp;
  requested_quantity?: Quantity;
  order_type?: OrderType;
  limit_price?: Decimal;
  metadata: Record<string, string>;
}

export type RiskOutcome = "approved" | "resized" | "rejected";

export interface RiskCheck {
  policy: string;
  passed: boolean;
  detail: string;
  limit?: string;
  observed?: string;
}

export interface RiskDecision {
  id: number;
  signal_id: number;
  decided_at: Timestamp;
  symbol: string;
  side: SignalSide;
  strategy_id: string;
  outcome: RiskOutcome;
  requested_quantity: Quantity;
  approved_quantity: Quantity;
  reason?: string; // first failing policy's code
  checks: RiskCheck[];
  order_id?: number;
}

export interface OrderTransition {
  status: OrderStatus;
  at: Timestamp;
  note?: string;
}

export interface Order {
  id: number;
  run_id: string;
  origin_signal: number;
  strategy_id: string;
  symbol: string;
  side: OrderSide;
  type: OrderType;
  quantity: Quantity;
  limit_price?: Decimal;
  status: OrderStatus;
  created_at: Timestamp;
  updated_at: Timestamp;
  filled_quantity: Quantity;
  average_fill_price?: Decimal;
  reject_reason?: string;
  history: OrderTransition[];
}

export interface Fill {
  id: number;
  order_id: number;
  run_id: string;
  symbol: string;
  side: OrderSide;
  filled_quantity: Quantity;
  fill_price: Decimal;
  fees: Decimal;
  slippage: Decimal; // per share, signed against the trader
  filled_at: Timestamp;
  status: "filled" | "partially_filled";
  sequence: number;
}

export interface Position {
  symbol: string;
  quantity: Quantity; // > 0 long, < 0 short
  average_cost: Decimal;
  realized_pnl: Decimal;
  unrealized_pnl: Decimal;
  mark_price: Decimal;
  market_value: Decimal;
}

export interface PendingOrder {
  order_id: number;
  symbol: string;
  side: OrderSide;
  remaining_quantity: Quantity;
  reserved_cash: Decimal;
}

export interface PortfolioSnapshot {
  run_id: string;
  as_of: Timestamp;
  cash: Decimal;
  total_equity: Decimal;
  positions: Position[];
  gross_exposure: Decimal;
  net_exposure: Decimal;
  pending_orders: PendingOrder[];
  reserved_cash: Decimal;
  buying_power: Decimal;
  session_pnl: Decimal; // equity change since the current session opened
  realized_pnl: Decimal;
  unrealized_pnl: Decimal;
}

export interface EquityPoint {
  time: Timestamp;
  equity: Decimal;
  cash: Decimal;
  drawdown: number; // fractional, <= 0
}

export interface Trade {
  symbol: string;
  strategy_id: string;
  quantity: Quantity;
  entry_time: Timestamp;
  exit_time: Timestamp;
  entry_price: Decimal;
  exit_price: Decimal;
  pnl: Decimal; // after fees
  fees: Decimal;
}

export type Stage = "ingest" | "strategy" | "risk" | "execution" | "portfolio" | "persistence";

export interface StageMetrics {
  stage: Stage;
  events: number;
  errors: number;
  latency_us: { count: number; min: number; p50: number; p99: number; max: number };
}

export interface MetricsSample {
  time: Timestamp; // wall-clock sample time
  events_per_sec: number;
  queue_depth: number;
  p99_us: number; // end-to-end, ingest to portfolio
}

export interface SystemMetrics {
  run_id: string;
  as_of: Timestamp;
  total_events: number;
  total_errors: number;
  dropped_events: number;
  queue_capacity: number;
  stages: StageMetrics[];
  history: MetricsSample[];
  latency_histogram: { upper_us: number; count: number }[]; // end-to-end
}

// Server -> client stream (SSE or WebSocket), one JSON object per message.
export type RunEvent =
  | { type: "run"; run: RunSummary }
  | { type: "bar"; bar: Bar }
  | { type: "signal"; signal: TradeSignal }
  | { type: "risk"; decision: RiskDecision }
  | { type: "order"; order: Order }
  | { type: "fill"; fill: Fill }
  | { type: "portfolio"; snapshot: PortfolioSnapshot }
  | { type: "metrics"; metrics: SystemMetrics };

export interface BacktestRequest {
  name: string;
  period_start: Timestamp;
  period_end: Timestamp;
  strategies: StrategyConfig[];
  risk: RiskLimits;
  simulation: SimulationConfig;
}

export interface ApiError {
  error: { code: string; message: string; field?: string };
}
