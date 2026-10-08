// In-browser stand-in for the engine pipeline: strategy -> risk -> execution -> portfolio.
// Strategy rules follow docs/strategies/*.md; reservation and buying power follow ADR 0004 / 0005.
// All money is integer micros (Number, well inside 2^53 at demo sizes) and goes out as exact decimal text.

import type {
  Bar, EquityPoint, Fill, Order, OrderStatus, PerformanceReport, PortfolioSnapshot, RiskCheck,
  RiskDecision, RunConfig, SignalSide, Stage, StrategyConfig, SystemMetrics, Trade, TradeSignal,
} from "../api/contract";
import { fromMicros, toMicros } from "../lib/decimal";
import { type RawBar, BARS_PER_SESSION, sessionOf } from "./market";
import { Rng, hash32 } from "./prng";

const M = (text: string) => Number(toMicros(text));
const D = (micros: number) => fromMicros(Math.round(micros));
const CENT = 10_000;
const roundCents = (micros: number) => Math.round(micros / CENT) * CENT;

// Internal times are epoch microseconds so sub-millisecond pipeline steps stay ordered.
export function isoUs(us: number): string {
  const ms = Math.floor(us / 1000);
  return `${new Date(ms).toISOString().slice(0, 19)}.${String(us % 1_000_000).padStart(6, "0")}Z`;
}

// ---- strategies ------------------------------------------------------------------------------

interface Emitted {
  side: SignalSide;
  metadata: Record<string, string>;
}

interface StrategyRuntime {
  config: StrategyConfig;
  onBar(bar: RawBar): Emitted | undefined;
}

const num = (v: number) => String(Number(v.toPrecision(10)));

function smaCrossover(config: StrategyConfig): StrategyRuntime {
  const short = config.short_window ?? 5;
  const long = config.long_window ?? 20;
  const state = new Map<string, { closes: number[]; relation: number }>();
  return {
    config,
    onBar(bar) {
      const s = state.get(bar.symbol) ?? { closes: [], relation: 0 };
      state.set(bar.symbol, s);
      s.closes.push(bar.close);
      if (s.closes.length > long) s.closes.shift();
      if (s.closes.length < long) return undefined;
      const longSum = s.closes.reduce((a, b) => a + b, 0);
      const shortSum = s.closes.slice(-short).reduce((a, b) => a + b, 0);
      // Exact comparison by cross-multiplication, as in the C++ strategy.
      const diff = shortSum * long - longSum * short;
      const relation = Math.sign(diff);
      if (relation === 0) return undefined;
      const previous = s.relation;
      s.relation = relation;
      if (previous === 0 || previous === relation) return undefined;
      return {
        side: relation > 0 ? "buy" : "sell",
        metadata: {
          trigger: relation > 0 ? "short_crossed_above_long" : "short_crossed_below_long",
          short_sma: num(shortSum / short / 1e6),
          long_sma: num(longSum / long / 1e6),
          short_window: String(short),
          long_window: String(long),
        },
      };
    },
  };
}

function meanReversion(config: StrategyConfig): StrategyRuntime {
  const lookback = config.lookback ?? 20;
  const entry = config.entry_threshold ?? 2;
  const rearm = config.rearm_threshold ?? 0.5;
  const state = new Map<string, { closes: number[]; latch: "neutral" | "lower" | "upper" }>();
  return {
    config,
    onBar(bar) {
      const s = state.get(bar.symbol) ?? { closes: [], latch: "neutral" as const };
      state.set(bar.symbol, s);
      s.closes.push(bar.close / 1e6);
      if (s.closes.length > lookback) s.closes.shift();
      if (s.closes.length < lookback) return undefined;
      const mean = s.closes.reduce((a, b) => a + b, 0) / lookback;
      const variance = s.closes.reduce((a, c) => a + (c - mean) ** 2, 0) / lookback; // population
      const sd = Math.sqrt(variance);
      const z = sd === 0 ? 0 : (bar.close / 1e6 - mean) / sd;
      const metadata = (trigger: string) => ({
        trigger,
        z_score: num(z),
        mean: num(mean),
        standard_deviation: num(sd),
        lookback: String(lookback),
        entry_threshold: String(entry),
      });
      if (z <= -entry && s.latch !== "lower") {
        s.latch = "lower";
        return { side: "buy", metadata: metadata("z_score_at_or_below_lower_entry") };
      }
      if (z >= entry && s.latch !== "upper") {
        s.latch = "upper";
        return { side: "sell", metadata: metadata("z_score_at_or_above_upper_entry") };
      }
      if (Math.abs(z) <= rearm) s.latch = "neutral";
      return undefined;
    },
  };
}

// ---- telemetry (synthetic timings; event counts are real) ------------------------------------

const STAGES: Stage[] = ["ingest", "strategy", "risk", "execution", "portfolio", "persistence"];
const STAGE_LATENCY: Record<Stage, [median: number, sigma: number]> = {
  ingest: [3.1, 0.35],
  strategy: [7.4, 0.4],
  risk: [4.2, 0.35],
  execution: [9.6, 0.45],
  portfolio: [3.5, 0.3],
  persistence: [140, 0.7],
};
const HISTOGRAM_BOUNDS = [10, 15, 20, 30, 50, 75, 100, 150, 250, 500];
const RESERVOIR = 2048;

class Telemetry {
  readonly rng: Rng;
  readonly stages = new Map<Stage, { events: number; errors: number; samples: number[]; seen: number; min: number; max: number }>();
  readonly histogram = HISTOGRAM_BOUNDS.map((upper_us) => ({ upper_us, count: 0 }));
  history: SystemMetrics["history"] = [];
  dropped = 0;

  constructor(seed: number) {
    this.rng = new Rng(seed);
    for (const s of STAGES) this.stages.set(s, { events: 0, errors: 0, samples: [], seen: 0, min: Infinity, max: 0 });
  }

  record(stage: Stage, n = 1): number {
    const entry = this.stages.get(stage)!;
    const [median, sigma] = STAGE_LATENCY[stage];
    let last = 0;
    for (let i = 0; i < n; i++) {
      const sample = Math.round(this.rng.logNormal(median, sigma) * 10) / 10;
      last = sample;
      entry.events++;
      entry.seen++;
      entry.min = Math.min(entry.min, sample);
      entry.max = Math.max(entry.max, sample);
      if (entry.samples.length < RESERVOIR) entry.samples.push(sample);
      else {
        const j = Math.floor(this.rng.next() * entry.seen);
        if (j < RESERVOIR) entry.samples[j] = sample;
      }
      if (stage === "persistence" && this.rng.next() < 0.0003) entry.errors++;
    }
    return last;
  }

  endToEnd(us: number) {
    const bucket = this.histogram.find((b) => us <= b.upper_us) ?? this.histogram[this.histogram.length - 1]!;
    bucket.count++;
  }

  totalEvents() {
    let n = 0;
    for (const s of this.stages.values()) n += s.events;
    return n;
  }

  p99EndToEnd(): number {
    const total = this.histogram.reduce((a, b) => a + b.count, 0);
    let acc = 0;
    for (const b of this.histogram) {
      acc += b.count;
      if (acc >= total * 0.99) return b.upper_us;
    }
    return 0;
  }
}

function percentile(sorted: number[], p: number) {
  if (sorted.length === 0) return 0;
  return sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))]!;
}

// ---- simulation ------------------------------------------------------------------------------

interface PositionState {
  qty: number;
  cost: number; // signed cost basis of the open quantity, micros
  realized: number;
  mark: number;
}

interface OpenTrade {
  strategy: string;
  entryTime: number;
  entryQty: number;
  entryNotional: number;
  exitQty: number;
  exitNotional: number;
  fees: number;
  realizedAtOpen: number;
  peakQty: number;
}

interface OrderState {
  order: Order;
  remaining: number;
  reserved: number;
}

export class Simulation {
  readonly config: RunConfig;
  readonly runId: string;
  private readonly strategies: StrategyRuntime[];
  private readonly slip: number;
  private readonly participation: number;
  private readonly latency: { toOrder: number; toFill: number };
  private readonly fees: { perShare: number; perTrade: number; bps: number };
  private readonly limits: {
    maxQty: number; maxPosition: number; maxGross: number; maxLoss: number; maxOpen: number; allowShort: boolean;
  };

  readonly startingCash: number;
  cash: number;
  readonly positions = new Map<string, PositionState>();
  private readonly openTrades = new Map<string, OpenTrade>();
  readonly trades: Trade[] = [];
  readonly bars = new Map<string, Bar[]>();
  readonly signals: TradeSignal[] = [];
  readonly decisions: RiskDecision[] = [];
  readonly orders: OrderState[] = [];
  readonly fills: Fill[] = [];
  readonly equity: EquityPoint[] = [];
  readonly telemetry: Telemetry;

  feesPaid = 0;
  clock = 0; // epoch us
  killSwitch: { engaged: boolean; changedAt?: number; reason?: string } = { engaged: false };
  private sequence = new Map<string, number>();
  private fillSequence = 0;
  private peakEquity: number;
  private session = "";
  private sessionStartEquity: number;
  private nextId = { signal: 1, decision: 1, order: 1, fill: 1 };

  constructor(runId: string, config: RunConfig) {
    this.runId = runId;
    this.config = config;
    this.strategies = config.strategies.map((s) => (s.kind === "sma_crossover" ? smaCrossover(s) : meanReversion(s)));
    const sim = config.simulation;
    this.slip = sim.slippage_bps / 10_000;
    this.participation = sim.participation_cap;
    this.latency = { toOrder: sim.latency_ms.signal_to_order * 1000, toFill: sim.latency_ms.order_to_fill * 1000 };
    this.fees = { perShare: M(sim.fees.per_share), perTrade: M(sim.fees.per_trade), bps: sim.fees.bps };
    const r = config.risk;
    this.limits = {
      maxQty: r.max_order_quantity,
      maxPosition: M(r.max_position_notional),
      maxGross: M(r.max_gross_exposure),
      maxLoss: M(r.max_daily_loss),
      maxOpen: r.max_open_orders,
      allowShort: r.allow_short,
    };
    this.startingCash = this.cash = M(sim.starting_cash);
    this.peakEquity = this.sessionStartEquity = this.cash;
    this.telemetry = new Telemetry(hash32(runId));
  }

  get symbols(): string[] {
    return [...new Set(this.config.strategies.flatMap((s) => s.symbols))].sort();
  }

  // ---- portfolio arithmetic ----

  private position(symbol: string): PositionState {
    let p = this.positions.get(symbol);
    if (!p) {
      p = { qty: 0, cost: 0, realized: 0, mark: 0 };
      this.positions.set(symbol, p);
    }
    return p;
  }

  private totalEquity() {
    let value = this.cash;
    for (const p of this.positions.values()) value += p.qty * p.mark;
    return value;
  }

  private exposure() {
    let gross = 0;
    let net = 0;
    for (const p of this.positions.values()) {
      gross += Math.abs(p.qty * p.mark);
      net += p.qty * p.mark;
    }
    return { gross, net };
  }

  private reservedCash() {
    return this.orders.reduce((a, o) => a + o.reserved, 0);
  }

  private openOrders() {
    return this.orders.filter((o) => o.remaining > 0 && (o.order.status === "working" || o.order.status === "partially_filled"));
  }

  private feeFor(qty: number, notional: number) {
    return roundCents(this.fees.perShare * qty + this.fees.perTrade + (notional * this.fees.bps) / 10_000);
  }

  private reserveFor(side: "buy" | "sell", qty: number, ref: number) {
    if (side === "sell" || qty === 0) return 0;
    const notional = qty * Math.round(ref * (1 + this.slip));
    return notional + this.feeFor(qty, notional);
  }

  private applyFill(state: OrderState, qty: number, refPrice: number, at: number) {
    const { order } = state;
    const dir = order.side === "buy" ? 1 : -1;
    const fillPrice = Math.round(refPrice * (1 + dir * this.slip));
    const notional = qty * fillPrice;
    const fee = this.feeFor(qty, notional);
    const p = this.position(order.symbol);
    const before = p.qty;

    this.cash += -dir * notional - fee;
    this.feesPaid += fee;
    p.realized -= fee;

    // Close against the open quantity first, then open or add with the rest.
    let rest = qty;
    if (p.qty !== 0 && Math.sign(p.qty) !== dir) {
      const closing = Math.min(rest, Math.abs(p.qty));
      const portion = Math.round((p.cost * closing) / Math.abs(p.qty));
      p.realized += Math.sign(p.qty) * closing * fillPrice - portion;
      p.cost -= portion;
      p.qty += dir * closing;
      rest -= closing;
      const t = this.openTrades.get(order.symbol);
      if (t) {
        t.exitQty += closing;
        t.exitNotional += closing * fillPrice;
        t.fees += fee;
      }
      if (p.qty === 0) {
        p.cost = 0;
        this.closeTrade(order.symbol, at, p);
      }
    }
    if (rest > 0) {
      if (p.qty === 0) {
        this.openTrades.set(order.symbol, {
          strategy: order.strategy_id,
          entryTime: at,
          entryQty: 0,
          entryNotional: 0,
          exitQty: 0,
          exitNotional: 0,
          fees: rest === qty ? fee : 0,
          realizedAtOpen: p.realized + (rest === qty ? fee : 0),
          peakQty: 0,
        });
      } else if (Math.sign(before) === dir) {
        const t = this.openTrades.get(order.symbol);
        if (t) t.fees += fee;
      }
      p.cost += dir * rest * fillPrice;
      p.qty += dir * rest;
      const t = this.openTrades.get(order.symbol);
      if (t) {
        t.entryQty += rest;
        t.entryNotional += rest * fillPrice;
        t.peakQty = Math.max(t.peakQty, Math.abs(p.qty));
      }
    }
    p.mark = refPrice;

    state.remaining -= qty;
    order.filled_quantity += qty;
    const filledValue = (order.average_fill_price ? M(order.average_fill_price) : 0) * (order.filled_quantity - qty) + notional;
    order.average_fill_price = D(filledValue / order.filled_quantity);
    const status: OrderStatus = state.remaining === 0 ? "filled" : "partially_filled";
    order.status = status;
    order.updated_at = isoUs(at);
    order.history.push({ status, at: isoUs(at), note: `${qty} shares` });
    state.reserved = this.reserveFor(order.side, state.remaining, refPrice);

    this.fills.push({
      id: this.nextId.fill++,
      order_id: order.id,
      run_id: this.runId,
      symbol: order.symbol,
      side: order.side,
      filled_quantity: qty,
      fill_price: D(fillPrice),
      fees: D(fee),
      slippage: D(Math.abs(fillPrice - refPrice)),
      filled_at: isoUs(at),
      status,
      sequence: ++this.fillSequence,
    });
    this.telemetry.record("execution");
    this.telemetry.record("portfolio");
    this.telemetry.record("persistence");
  }

  private closeTrade(symbol: string, at: number, p: PositionState) {
    const t = this.openTrades.get(symbol);
    if (!t) return;
    this.openTrades.delete(symbol);
    this.trades.push({
      symbol,
      strategy_id: t.strategy,
      quantity: t.peakQty,
      entry_time: isoUs(t.entryTime),
      exit_time: isoUs(at),
      entry_price: D(t.entryNotional / Math.max(1, t.entryQty)),
      exit_price: D(t.exitNotional / Math.max(1, t.exitQty)),
      pnl: D(p.realized - t.realizedAtOpen),
      fees: D(t.fees),
    });
  }

  private tryFill(state: OrderState, bar: RawBar, at: number) {
    const cap = Math.max(1, Math.floor(bar.volume * this.participation));
    const qty = Math.min(state.remaining, cap);
    if (qty > 0) this.applyFill(state, qty, bar.close, at);
  }

  // ---- risk ----

  private evaluate(signal: TradeSignal, bar: RawBar): { outcome: RiskDecision["outcome"]; qty: number; reason?: string; checks: RiskCheck[] } {
    const checks: RiskCheck[] = [];
    const requested = signal.requested_quantity ?? 0;
    let qty = requested;
    let reason: string | undefined;
    let rejected = false;
    const px = bar.close;
    const p = this.position(signal.symbol);
    const dir = signal.side === "buy" ? 1 : -1;
    const increases = () => p.qty === 0 || Math.sign(p.qty) === dir || qty > Math.abs(p.qty);

    const check = (policy: string, passed: boolean, detail: string, limit?: string, observed?: string, resized = false) => {
      checks.push({ policy, passed, detail, limit, observed });
      if (!passed && !rejected) {
        if (resized) reason ??= policy;
        else {
          rejected = true;
          reason = policy;
        }
      }
    };

    check("kill_switch", !this.killSwitch.engaged, this.killSwitch.engaged ? "Kill switch is engaged" : "Kill switch is off");

    const sessionPnl = this.totalEquity() - this.sessionStartEquity;
    const lossHit = sessionPnl <= -this.limits.maxLoss;
    check(
      "max_daily_loss",
      !(lossHit && increases()),
      lossHit ? "Session loss limit reached; only reducing orders pass" : "Session loss within limit",
      D(this.limits.maxLoss),
      D(-sessionPnl),
    );

    if (!rejected && dir < 0 && !this.limits.allowShort) {
      const held = Math.max(0, p.qty);
      if (held === 0) check("short_selling", false, `No ${signal.symbol} position to sell; short selling is disabled`, "0", String(p.qty));
      else if (qty > held) {
        qty = held;
        check("short_selling", false, `Resized to the ${held} shares held`, String(held), String(requested), true);
      } else check("short_selling", true, "Sell reduces an existing long");
    }

    if (!rejected) {
      const over = qty > this.limits.maxQty;
      if (over) qty = this.limits.maxQty;
      check("max_order_quantity", !over, over ? `Resized to ${this.limits.maxQty} shares` : "Within limit",
        String(this.limits.maxQty), String(requested), over);
    }

    if (!rejected) {
      const open = this.openOrders().length;
      check("max_open_orders", open < this.limits.maxOpen, `${open} open orders`, String(this.limits.maxOpen), String(open));
    }

    if (!rejected && increases()) {
      const current = Math.abs(p.qty) * px;
      const room = Math.max(0, Math.floor((this.limits.maxPosition - current) / px));
      if (qty > room) {
        qty = room;
        if (room === 0) check("max_position_notional", false, "Position is at its notional limit", D(this.limits.maxPosition), D(current));
        else check("max_position_notional", false, `Resized to ${room} shares to stay under the position limit`, D(this.limits.maxPosition), D(current + requested * px), true);
      } else check("max_position_notional", true, "Within limit", D(this.limits.maxPosition), D(current + qty * px));
    }

    if (!rejected && increases()) {
      const { gross } = this.exposure();
      const room = Math.max(0, Math.floor((this.limits.maxGross - gross) / px));
      if (qty > room) {
        qty = room;
        if (room === 0) check("max_gross_exposure", false, "Portfolio is at its gross exposure limit", D(this.limits.maxGross), D(gross));
        else check("max_gross_exposure", false, `Resized to ${room} shares to stay under gross exposure`, D(this.limits.maxGross), D(gross + requested * px), true);
      } else check("max_gross_exposure", true, "Within limit", D(this.limits.maxGross), D(gross + qty * px));
    }

    if (!rejected && dir > 0) {
      const bp = this.cash - this.reservedCash();
      const need = this.reserveFor("buy", qty, px);
      if (need > bp) {
        const unit = this.reserveFor("buy", 1, px);
        const fit = Math.max(0, Math.floor(bp / unit));
        qty = Math.min(qty, fit);
        if (fit === 0) check("buying_power", false, "Not enough buying power for one share", D(bp), D(need));
        else check("buying_power", false, `Resized to ${fit} shares to fit buying power`, D(bp), D(need), true);
      } else check("buying_power", true, "Covered by buying power", D(bp), D(need));
    }

    if (!rejected && qty <= 0) {
      rejected = true;
      reason ??= "zero_quantity";
    }
    return { outcome: rejected ? "rejected" : qty < requested ? "resized" : "approved", qty: rejected ? 0 : qty, reason, checks };
  }

  // ---- the pipeline, one bar-open timestamp at a time ----

  step(time: number, bars: RawBar[]) {
    const timeUs = time * 1000;
    const session = sessionOf(time);
    if (session !== this.session) {
      this.session = session;
      this.sessionStartEquity = this.totalEquity();
    }

    for (const bar of bars) {
      const seq = (this.sequence.get(bar.symbol) ?? 0) + 1;
      this.sequence.set(bar.symbol, seq);
      const list = this.bars.get(bar.symbol) ?? [];
      this.bars.set(bar.symbol, list);
      list.push({
        symbol: bar.symbol,
        exchange_time: isoUs(timeUs),
        open: D(bar.open),
        high: D(bar.high),
        low: D(bar.low),
        price: D(bar.close),
        volume: bar.volume,
        sequence: seq,
      });
      let e2e = this.telemetry.record("ingest");
      this.telemetry.record("persistence");

      for (const state of this.orders) {
        if (state.remaining > 0 && state.order.symbol === bar.symbol && state.order.status === "partially_filled") {
          this.tryFill(state, bar, timeUs + this.latency.toFill);
        }
      }
      this.position(bar.symbol).mark = bar.close;
      e2e += this.telemetry.record("portfolio");

      for (const strategy of this.strategies) {
        if (!strategy.config.symbols.includes(bar.symbol)) continue;
        e2e += this.telemetry.record("strategy");
        const emitted = strategy.onBar(bar);
        if (!emitted) continue;
        e2e += this.handleSignal(strategy.config, emitted, bar, timeUs);
      }
      this.telemetry.endToEnd(e2e);
    }

    const equity = this.totalEquity();
    this.peakEquity = Math.max(this.peakEquity, equity);
    this.equity.push({
      time: isoUs(timeUs),
      equity: D(equity),
      cash: D(this.cash),
      drawdown: this.peakEquity > 0 ? equity / this.peakEquity - 1 : 0,
    });
    this.clock = timeUs;
  }

  private handleSignal(config: StrategyConfig, emitted: Emitted, bar: RawBar, timeUs: number): number {
    const signal: TradeSignal = {
      id: this.nextId.signal++,
      strategy_id: config.strategy_id,
      symbol: bar.symbol,
      side: emitted.side,
      created_at: isoUs(timeUs),
      requested_quantity: config.requested_quantity,
      order_type: "market",
      metadata: emitted.metadata,
    };
    this.signals.push(signal);
    const riskUs = this.telemetry.record("risk");
    const result = this.evaluate(signal, bar);
    const decidedAt = timeUs + Math.max(1, Math.round(riskUs));
    const side = emitted.side === "sell" ? "sell" : "buy";

    const order: Order = {
      id: this.nextId.order++,
      run_id: this.runId,
      origin_signal: signal.id,
      strategy_id: signal.strategy_id,
      symbol: signal.symbol,
      side,
      type: "market",
      quantity: result.outcome === "rejected" ? signal.requested_quantity ?? 0 : result.qty,
      status: "new",
      created_at: isoUs(timeUs),
      updated_at: isoUs(decidedAt),
      filled_quantity: 0,
      history: [
        { status: "new", at: isoUs(timeUs) },
        { status: "pending_risk", at: isoUs(timeUs + 1) },
      ],
    };
    const decision: RiskDecision = {
      id: this.nextId.decision++,
      signal_id: signal.id,
      decided_at: isoUs(decidedAt),
      symbol: signal.symbol,
      side: signal.side,
      strategy_id: signal.strategy_id,
      outcome: result.outcome,
      requested_quantity: signal.requested_quantity ?? 0,
      approved_quantity: result.qty,
      reason: result.reason,
      checks: result.checks,
      order_id: order.id,
    };
    this.decisions.push(decision);

    const state: OrderState = { order, remaining: 0, reserved: 0 };
    this.orders.push(state);
    if (result.outcome === "rejected") {
      order.status = "rejected";
      order.reject_reason = result.reason;
      order.history.push({ status: "rejected", at: isoUs(decidedAt), note: result.reason });
      this.telemetry.record("persistence");
      return riskUs;
    }
    const workingAt = decidedAt + this.latency.toOrder;
    order.history.push(
      { status: "approved", at: isoUs(decidedAt), note: result.outcome === "resized" ? `resized ${decision.requested_quantity} → ${result.qty}` : undefined },
      { status: "working", at: isoUs(workingAt) },
    );
    order.status = "working";
    state.remaining = result.qty;
    state.reserved = this.reserveFor(side, result.qty, bar.close);
    this.tryFill(state, bar, workingAt + this.latency.toFill);
    return riskUs + this.telemetry.record("execution");
  }

  setKillSwitch(engaged: boolean, reason?: string) {
    this.killSwitch = { engaged, changedAt: this.clock, reason };
  }

  // ---- read models ----

  snapshot(): PortfolioSnapshot {
    const positions = [...this.positions.entries()]
      .filter(([, p]) => p.qty !== 0 || p.realized !== 0)
      .map(([symbol, p]) => ({
        symbol,
        quantity: p.qty,
        average_cost: D(p.qty === 0 ? 0 : p.cost / p.qty),
        realized_pnl: D(p.realized),
        unrealized_pnl: D(p.qty * p.mark - p.cost),
        mark_price: D(p.mark),
        market_value: D(p.qty * p.mark),
      }))
      .sort((a, b) => a.symbol.localeCompare(b.symbol));
    const { gross, net } = this.exposure();
    const reserved = this.reservedCash();
    const equity = this.totalEquity();
    let realized = 0;
    let unrealized = 0;
    for (const p of this.positions.values()) {
      realized += p.realized;
      unrealized += p.qty * p.mark - p.cost;
    }
    return {
      run_id: this.runId,
      as_of: isoUs(this.clock),
      cash: D(this.cash),
      total_equity: D(equity),
      positions,
      gross_exposure: D(gross),
      net_exposure: D(net),
      pending_orders: this.openOrders().map((o) => ({
        order_id: o.order.id,
        symbol: o.order.symbol,
        side: o.order.side,
        remaining_quantity: o.remaining,
        reserved_cash: D(o.reserved),
      })),
      reserved_cash: D(reserved),
      buying_power: D(this.cash - reserved),
      session_pnl: D(equity - this.sessionStartEquity),
      realized_pnl: D(realized),
      unrealized_pnl: D(unrealized),
    };
  }

  report(generatedAt: string): PerformanceReport {
    const first = this.equity[0];
    const last = this.equity[this.equity.length - 1];
    const returns: number[] = [];
    let prev = this.startingCash;
    for (const point of this.equity) {
      const v = M(point.equity);
      returns.push(prev > 0 ? v / prev - 1 : 0);
      prev = v;
    }
    const mean = returns.reduce((a, b) => a + b, 0) / Math.max(1, returns.length);
    const sd = Math.sqrt(returns.reduce((a, r) => a + (r - mean) ** 2, 0) / Math.max(1, returns.length - 1));
    const annual = Math.sqrt(252 * BARS_PER_SESSION);
    // Sharpe from session-close equity; bar-level Sharpe annualises into meaningless magnitudes.
    const closes = new Map<string, number>();
    for (const point of this.equity) closes.set(point.time.slice(0, 10), M(point.equity));
    const daily: number[] = [];
    let previousClose = this.startingCash;
    for (const close of closes.values()) {
      daily.push(close / previousClose - 1);
      previousClose = close;
    }
    const dMean = daily.reduce((a, b) => a + b, 0) / Math.max(1, daily.length);
    const dSd = Math.sqrt(daily.reduce((a, r) => a + (r - dMean) ** 2, 0) / Math.max(1, daily.length - 1));
    const pnls = this.trades.map((t) => M(t.pnl));
    const grossProfit = pnls.filter((v) => v > 0).reduce((a, b) => a + b, 0);
    const grossLoss = -pnls.filter((v) => v < 0).reduce((a, b) => a + b, 0);
    const ending = last ? M(last.equity) : this.startingCash;
    return {
      run_id: this.runId,
      generated_at: generatedAt,
      period_start: first?.time ?? this.config.period_start,
      period_end: last?.time ?? this.config.period_start,
      total_return: ending / this.startingCash - 1,
      volatility: sd * annual,
      max_drawdown: this.equity.reduce((a, p) => Math.min(a, p.drawdown), 0),
      ...(pnls.length === 0
        ? { profit_factor_status: "no_trades" as const }
        : grossLoss === 0
          ? { profit_factor_status: "no_losing_trades" as const }
          : { profit_factor: grossProfit / grossLoss }),
      trade_count: this.trades.length,
      ...(pnls.length > 0 ? { win_rate: pnls.filter((v) => v > 0).length / pnls.length } : {}),
      ...(daily.length >= 3 && dSd > 0 ? { sharpe: (dMean / dSd) * Math.sqrt(252) } : {}),
      ending_equity: D(ending),
      fees_paid: D(this.feesPaid),
    };
  }

  metrics(asOf: string): SystemMetrics {
    const t = this.telemetry;
    return {
      run_id: this.runId,
      as_of: asOf,
      total_events: t.totalEvents(),
      total_errors: [...t.stages.values()].reduce((a, s) => a + s.errors, 0),
      dropped_events: t.dropped,
      queue_capacity: 65_536,
      stages: STAGES.map((stage) => {
        const s = t.stages.get(stage)!;
        const sorted = [...s.samples].sort((a, b) => a - b);
        return {
          stage,
          events: s.events,
          errors: s.errors,
          latency_us: {
            count: s.events,
            min: s.events ? s.min : 0,
            p50: percentile(sorted, 0.5),
            p99: percentile(sorted, 0.99),
            max: s.max,
          },
        };
      }),
      history: t.history,
      latency_histogram: t.histogram.map((b) => ({ ...b })),
    };
  }

  orderList(): Order[] {
    return this.orders.map((o) => o.order);
  }
}
