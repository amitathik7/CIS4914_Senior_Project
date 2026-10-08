import type { EngineClient } from "../api/client";
import { EngineApiError } from "../api/client";
import type { BacktestRequest, RunConfig, RunDetail, RunEvent, RunStatus, RunSummary } from "../api/contract";
import { toMicros } from "../lib/decimal";
import { LAST_BACKTEST_SESSION, LIVE_SESSION, SYMBOLS, barAt, minutesBetween, nextMinute, sessionDates, sessionOpen } from "./market";
import { LIVE_CONFIG, PRESET_BACKTESTS, sma, DEFAULT_RISK, DEFAULT_SIMULATION } from "./presets";
import { Rng, hash32, runIdFor } from "./prng";
import { Simulation, isoUs } from "./simulation";

const STORAGE_KEY = "te.demo.backtests";
const BAR_INTERVAL_MS = 1500;
const CHUNK = 240; // timestamps replayed per macrotask

interface RunEntry {
  sim: Simulation;
  status: RunStatus;
  progress?: number;
  createdAt: string;
  error?: string;
  timers: number[];
}

const iso = (ms: number) => new Date(ms).toISOString();

function storedRequests(): BacktestRequest[] {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? (JSON.parse(raw) as BacktestRequest[]) : [];
  } catch {
    return [];
  }
}

function storeRequest(request: BacktestRequest) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify([...storedRequests(), request]));
  } catch {
    // Storage unavailable: the run still exists for this page session.
  }
}

function invalid(field: string, message: string): never {
  throw new EngineApiError(422, "invalid_request", message, field);
}

export function validateBacktest(request: BacktestRequest): void {
  if (!request.name.trim()) invalid("name", "Give the backtest a name.");
  const start = Date.parse(request.period_start);
  const end = Date.parse(request.period_end);
  const first = sessionOpen(sessionDates()[0]!);
  const last = sessionOpen(LAST_BACKTEST_SESSION) + 390 * 60_000;
  if (Number.isNaN(start) || Number.isNaN(end)) invalid("period", "Pick a start and end date.");
  if (end <= start) invalid("period", "The end date must be after the start date.");
  if (start < first || end > last) {
    invalid("period", `Recorded data covers ${sessionDates()[0]} to ${LAST_BACKTEST_SESSION}.`);
  }
  if (minutesBetween(start, end).length > 390 * 25) invalid("period", "Backtests are limited to 25 sessions.");
  if (request.strategies.length === 0) invalid("strategies", "Add at least one strategy.");
  const ids = new Set<string>();
  request.strategies.forEach((s, i) => {
    const at = `strategies.${i}`;
    if (!s.strategy_id.trim()) invalid(`${at}.strategy_id`, "Each strategy needs an id.");
    if (ids.has(s.strategy_id)) invalid(`${at}.strategy_id`, `Strategy id "${s.strategy_id}" is used twice.`);
    ids.add(s.strategy_id);
    if (s.symbols.length === 0) invalid(`${at}.symbols`, "Choose at least one symbol.");
    for (const sym of s.symbols) if (!SYMBOLS.includes(sym)) invalid(`${at}.symbols`, `No recorded data for ${sym}.`);
    if (!Number.isInteger(s.requested_quantity) || s.requested_quantity < 1) invalid(`${at}.requested_quantity`, "Quantity must be a whole number of shares, at least 1.");
    if (s.kind === "sma_crossover") {
      const short = s.short_window ?? 0;
      const long = s.long_window ?? 0;
      if (!Number.isInteger(short) || short < 1) invalid(`${at}.short_window`, "Short window must be at least 1.");
      if (!Number.isInteger(long) || long <= short) invalid(`${at}.long_window`, "Long window must be greater than the short window.");
    } else {
      const lookback = s.lookback ?? 0;
      const entry = s.entry_threshold ?? NaN;
      const rearm = s.rearm_threshold ?? NaN;
      if (!Number.isInteger(lookback) || lookback < 2) invalid(`${at}.lookback`, "Lookback must be at least 2.");
      if (!Number.isFinite(rearm) || rearm < 0) invalid(`${at}.rearm_threshold`, "Rearm threshold must be 0 or more.");
      if (!Number.isFinite(entry) || entry <= rearm) invalid(`${at}.entry_threshold`, "Entry threshold must be greater than the rearm threshold.");
    }
  });
  const decimalAtLeast = (field: string, text: string, min: bigint, message: string) => {
    let micros: bigint;
    try {
      micros = toMicros(text);
    } catch {
      invalid(field, "Enter a plain decimal number, like 25000 or 0.0035.");
    }
    if (micros < min) invalid(field, message);
  };
  const { risk, simulation: sim } = request;
  if (!Number.isInteger(risk.max_order_quantity) || risk.max_order_quantity < 1) invalid("risk.max_order_quantity", "Max order size must be a whole number of shares, at least 1.");
  decimalAtLeast("risk.max_position_notional", risk.max_position_notional, 1n, "Max position must be greater than zero.");
  decimalAtLeast("risk.max_gross_exposure", risk.max_gross_exposure, 1n, "Max gross exposure must be greater than zero.");
  decimalAtLeast("risk.max_daily_loss", risk.max_daily_loss, 1n, "Daily loss stop must be greater than zero.");
  if (!Number.isInteger(risk.max_open_orders) || risk.max_open_orders < 1) invalid("risk.max_open_orders", "Max open orders must be a whole number, at least 1.");
  decimalAtLeast("simulation.starting_cash", sim.starting_cash, 1n, "Starting cash must be greater than zero.");
  decimalAtLeast("simulation.fees.per_share", sim.fees.per_share, 0n, "Fees cannot be negative.");
  if (!Number.isFinite(sim.slippage_bps) || sim.slippage_bps < 0 || sim.slippage_bps > 500) invalid("simulation.slippage_bps", "Slippage must be between 0 and 500 basis points.");
  if (!(sim.participation_cap > 0 && sim.participation_cap <= 1)) invalid("simulation.participation_cap", "Participation must be above 0% and at most 100%.");
  if (!Number.isFinite(sim.latency_ms.signal_to_order) || sim.latency_ms.signal_to_order < 0) invalid("simulation.latency_ms.signal_to_order", "Latency must be 0 ms or more.");
  if (!Number.isFinite(sim.latency_ms.order_to_fill) || sim.latency_ms.order_to_fill < 0) invalid("simulation.latency_ms.order_to_fill", "Latency must be 0 ms or more.");
}

export class MockEngineClient implements EngineClient {
  private readonly runs = new Map<string, RunEntry>();
  private readonly listeners = new Map<string, Set<(event: RunEvent) => void>>();
  private liveId = "";

  constructor() {
    PRESET_BACKTESTS.forEach((request, i) => this.startBacktest(request, iso(Date.parse("2026-10-05T15:12:00Z") + i * 7 * 60_000), true));
    this.addFailedRun();
    for (const request of storedRequests()) {
      try {
        validateBacktest(request);
        this.startBacktest(request, iso(Date.now()), true);
      } catch {
        // A stored request from an older build no longer validates; skip it.
      }
    }
    this.startLive();
  }

  // ---- run lifecycle ----

  private configFor(request: BacktestRequest): RunConfig {
    return { name: request.name, mode: "backtest", period_start: request.period_start, period_end: request.period_end, strategies: request.strategies, risk: request.risk, simulation: request.simulation };
  }

  private startBacktest(request: BacktestRequest, createdAt: string, synchronous: boolean): string {
    const config = this.configFor(request);
    let runId = runIdFor(JSON.stringify(config));
    if (this.runs.has(runId)) runId = runIdFor(JSON.stringify(config) + createdAt);
    const sim = new Simulation(runId, config);
    const entry: RunEntry = { sim, status: "queued", createdAt, progress: 0, timers: [] };
    this.runs.set(runId, entry);
    const minutes = minutesBetween(Date.parse(config.period_start), Date.parse(config.period_end!));
    const rng = new Rng(hash32(runId) ^ 0x5bd1e995);
    const base = Date.parse(createdAt);
    let index = 0;
    let chunkNo = 0;

    const runChunk = () => {
      entry.status = "running";
      const stop = Math.min(minutes.length, index + CHUNK);
      for (; index < stop; index++) {
        const t = minutes[index]!;
        sim.step(t, sim.symbols.map((s) => barAt(s, t)).filter((b) => b !== undefined));
      }
      sim.telemetry.history.push({
        time: iso(base + ++chunkNo * 40),
        events_per_sec: Math.round(rng.logNormal(186_000, 0.07)),
        queue_depth: Math.round(rng.logNormal(380, 0.5)),
        p99_us: sim.telemetry.p99EndToEnd(),
      });
      entry.progress = index / minutes.length;
      if (index >= minutes.length) {
        entry.status = "completed";
        entry.progress = undefined;
      }
      this.emit(runId, { type: "run", run: this.summary(runId) });
    };

    if (synchronous) {
      while (index < minutes.length) runChunk();
      if (minutes.length === 0) entry.status = "completed";
    } else {
      const tick = () => {
        if (entry.status === "stopped") return;
        runChunk();
        if (index < minutes.length) entry.timers.push(window.setTimeout(tick, 16));
      };
      entry.timers.push(window.setTimeout(tick, 250));
    }
    return runId;
  }

  private addFailedRun() {
    const request: BacktestRequest = {
      name: "NVDA crossover, September",
      period_start: "2026-09-01T13:30:00Z",
      period_end: "2026-09-30T20:00:00Z",
      strategies: [sma(["NVDA"], 8, 30, 150)],
      risk: DEFAULT_RISK,
      simulation: DEFAULT_SIMULATION,
    };
    const config = this.configFor(request);
    const runId = runIdFor(JSON.stringify(config) + "failed");
    const sim = new Simulation(runId, config);
    this.runs.set(runId, {
      sim,
      status: "failed",
      createdAt: "2026-10-06T18:41:00Z",
      error: "Dataset refused at line 8,214: NVDA bar at 2026-09-22T14:07:00Z is older than the previous NVDA bar. Nothing was replayed.",
      timers: [],
    });
  }

  private startLive() {
    const open = sessionOpen(LIVE_SESSION);
    const config: RunConfig = { ...LIVE_CONFIG, mode: "live", period_start: iso(open) };
    const runId = runIdFor(JSON.stringify(config));
    this.liveId = runId;
    const sim = new Simulation(runId, config);
    const entry: RunEntry = { sim, status: "running", createdAt: iso(open - 5 * 60_000), timers: [] };
    this.runs.set(runId, entry);
    const rng = new Rng(hash32(runId));

    let t: number | undefined = open;
    const advance = () => {
      if (t === undefined) return;
      sim.step(t, sim.symbols.map((s) => barAt(s, t!)).filter((b) => b !== undefined));
      t = nextMinute(t);
    };
    for (let i = 0; i < 90; i++) advance(); // the morning so far
    // The last two minutes of telemetry, so the charts are not empty on first load.
    const now = Date.now();
    for (let i = 120; i > 0; i--) {
      const prints = Math.round(rng.logNormal(170, 0.25));
      sim.telemetry.record("ingest", prints);
      sim.telemetry.history.push({
        time: iso(now - i * 1000),
        events_per_sec: prints + sim.symbols.length,
        queue_depth: Math.max(0, Math.round(rng.logNormal(3, 0.8)) - 1),
        p99_us: sim.telemetry.p99EndToEnd(),
      });
    }

    entry.timers.push(
      window.setInterval(() => {
        if (entry.status !== "running") return;
        if (t === undefined) {
          entry.status = "stopped";
          entry.error = "The demo calendar has ended.";
          this.emit(runId, { type: "run", run: this.summary(runId) });
          return;
        }
        const before = { signals: sim.signals.length, decisions: sim.decisions.length, fills: sim.fills.length };
        advance();
        for (const bars of sim.bars.values()) {
          const bar = bars[bars.length - 1];
          if (bar && Date.parse(bar.exchange_time) * 1000 === sim.clock) this.emit(runId, { type: "bar", bar });
        }
        for (const signal of sim.signals.slice(before.signals)) this.emit(runId, { type: "signal", signal });
        for (const decision of sim.decisions.slice(before.decisions)) this.emit(runId, { type: "risk", decision });
        for (const fill of sim.fills.slice(before.fills)) this.emit(runId, { type: "fill", fill });
        this.emit(runId, { type: "portfolio", snapshot: sim.snapshot() });
        this.emit(runId, { type: "run", run: this.summary(runId) });
      }, BAR_INTERVAL_MS),
      window.setInterval(() => {
        if (entry.status !== "running") return;
        // Trade prints between bars: counted by ingest, ignored by the bar-only strategies.
        const prints = Math.round(rng.logNormal(170, 0.25));
        sim.telemetry.record("ingest", prints);
        sim.telemetry.record("persistence", Math.round(prints / 8));
        const history = sim.telemetry.history;
        history.push({
          time: iso(Date.now()),
          events_per_sec: prints + sim.symbols.length,
          queue_depth: Math.max(0, Math.round(rng.logNormal(3, 0.8)) - 1),
          p99_us: sim.telemetry.p99EndToEnd(),
        });
        if (history.length > 300) history.splice(0, history.length - 300);
        this.emit(runId, { type: "metrics", metrics: sim.metrics(iso(Date.now())) });
      }, 1000),
    );
  }

  private summary(runId: string): RunSummary {
    const entry = this.get(runId);
    const { sim } = entry;
    const c = sim.config;
    return {
      run_id: runId,
      name: c.name,
      mode: c.mode,
      status: entry.status,
      ...(entry.progress !== undefined && entry.status !== "completed" ? { progress: entry.progress } : {}),
      created_at: entry.createdAt,
      period_start: c.period_start,
      ...(c.period_end ? { period_end: c.period_end } : {}),
      ...(sim.clock ? { clock: isoUs(sim.clock) } : {}),
      symbols: sim.symbols,
      strategies: c.strategies.map((s) => s.strategy_id),
      ...(entry.status === "completed" || (entry.status === "running" && c.mode === "live") ? { report: sim.report(isoUs(sim.clock)) } : {}),
      ...(entry.error ? { error: entry.error } : {}),
    };
  }

  private get(runId: string): RunEntry {
    const entry = this.runs.get(runId);
    if (!entry) throw new EngineApiError(404, "run_not_found", `No run with id ${runId}.`);
    return entry;
  }

  private emit(runId: string, event: RunEvent) {
    for (const listener of this.listeners.get(runId) ?? []) listener(event);
  }

  private respond<T>(value: () => T): Promise<T> {
    return new Promise((resolve, reject) => {
      queueMicrotask(() => {
        try {
          resolve(structuredClone(value()));
        } catch (e) {
          reject(e);
        }
      });
    });
  }

  // ---- EngineClient ----

  info = () =>
    this.respond(() => ({
      project_version: "0.1.0",
      data_source: "demo" as const,
      api_version: "1" as const,
      market_data: { symbols: SYMBOLS, first_session: sessionDates()[0]!, last_session: LAST_BACKTEST_SESSION },
    }));

  listRuns = () =>
    this.respond(() =>
      [...this.runs.keys()]
        .map((id) => this.summary(id))
        .sort((a, b) => (a.mode === b.mode ? b.created_at.localeCompare(a.created_at) : a.mode === "live" ? -1 : 1)),
    );

  getRun = (runId: string) =>
    this.respond<RunDetail>(() => {
      const { sim } = this.get(runId);
      const k = sim.killSwitch;
      return {
        ...this.summary(runId),
        config: sim.config,
        kill_switch: { engaged: k.engaged, ...(k.changedAt ? { changed_at: isoUs(k.changedAt) } : {}), ...(k.reason ? { reason: k.reason } : {}) },
      };
    });

  createBacktest = (request: BacktestRequest) =>
    this.respond(() => {
      validateBacktest(request);
      storeRequest(request);
      return this.summary(this.startBacktest(request, iso(Date.now()), false));
    });

  stopRun = (runId: string) =>
    this.respond(() => {
      const entry = this.get(runId);
      if (entry.status === "running" || entry.status === "queued") {
        entry.status = "stopped";
        entry.timers.forEach((t) => clearTimeout(t));
        this.emit(runId, { type: "run", run: this.summary(runId) });
      }
      return this.summary(runId);
    });

  setKillSwitch = async (runId: string, engaged: boolean, reason?: string) => {
    this.get(runId).sim.setKillSwitch(engaged, reason);
    this.emit(runId, { type: "run", run: this.summary(runId) });
    return this.getRun(runId);
  };

  portfolio = (id: string) => this.respond(() => this.get(id).sim.snapshot());
  equity = (id: string) => this.respond(() => this.get(id).sim.equity);
  bars = (id: string, symbol: string) => this.respond(() => this.get(id).sim.bars.get(symbol) ?? []);
  signals = (id: string) => this.respond(() => this.get(id).sim.signals);
  riskDecisions = (id: string) => this.respond(() => this.get(id).sim.decisions);
  orders = (id: string) => this.respond(() => this.get(id).sim.orderList());
  fills = (id: string) => this.respond(() => this.get(id).sim.fills);
  trades = (id: string) => this.respond(() => this.get(id).sim.trades);
  metrics = (id: string) => this.respond(() => this.get(id).sim.metrics(iso(Date.now())));

  subscribe(runId: string, onEvent: (event: RunEvent) => void) {
    const set = this.listeners.get(runId) ?? new Set();
    this.listeners.set(runId, set);
    set.add(onEvent);
    return () => {
      set.delete(onEvent);
    };
  }

  get liveRunId() {
    return this.liveId;
  }
}
