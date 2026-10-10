// A completed `strategy_lab.replay` document, read losslessly and checked before anything is drawn (docs/STRATEGY_LAB.md
// section 9). The same structural checks as the Streamlit lab's schema.py: indexes are 0..N-1, every signal points at the event
// that produced it, every id an event lists is a real signal, and the counts agree.
//
// Numeric rules (the project's int64 fixed-point contract, ADR 0002 / docs/STRATEGY_LAB.md):
//   price, open, high, low, limit price   exact decimal TEXT ("9223372036854.775807"); never a Number
//   volume, requested quantity            exact integer TEXT (int64 whole shares); never a Number
//   timestamps                            the tool's RFC 3339 UTC text with nine fractional digits; never a Date
//   index, source line, bus sequence     counts bounded by the dataset size: JavaScript integers, verified safe
//   signal id                             exact integer TEXT (a 64-bit identifier; compared as text, ordered by length then digits)
//   indicators (averages, z-scores)       derived doubles: { value, text } where text is exactly what the tool wrote
// A chart may turn a price into a Number to place a point (plotNumber in cursor.ts); nothing else may.

import { LabError } from "../api/lab";
import { decimalText, derivedNumber, integerText, isLNum, isObject, safeInteger, type LValue } from "../lib/losslessJson";
import { asArray, asBoolean, asCount, asObject, asString, invalid, need, readEnvelope } from "./wire";

export interface Entry {
  readonly name: string;
  readonly text: string;
}

export interface Indicator {
  readonly value: number;
  readonly text: string;
}

export interface WindowInfo {
  readonly fill?: number;
  readonly remaining?: number;
  readonly size: number;
}

export interface EventResult {
  readonly strategyId: string;
  readonly available: boolean;
  readonly unavailableReason?: string;
  readonly verdict?: string;
  readonly reason?: string;
  readonly action?: string;
  readonly window?: WindowInfo;
  readonly indicators: Readonly<Record<string, Indicator>>;
  readonly unavailable: Readonly<Record<string, string>>;
  readonly states: Readonly<Record<string, string>>;
  readonly signalIds: readonly string[];
}

export interface ReplayEvent {
  readonly index: number; // 0-based; the UI says "event index + 1"
  readonly sourceLine?: number;
  readonly symbol: string;
  readonly time: string;
  readonly type: string;
  readonly price?: string;
  readonly open?: string;
  readonly high?: string;
  readonly low?: string;
  readonly volume?: string;
  readonly busSequence: number;
  readonly results: readonly EventResult[];
}

export interface ReplaySignal {
  readonly id: string;
  readonly ref: string;
  readonly strategyId: string;
  readonly eventIndex: number;
  readonly busSequence?: number;
  readonly symbol: string;
  readonly side: string;
  readonly orderType?: string;
  readonly quantity?: string;
  readonly limitPrice?: string;
  readonly createdAt: string;
  readonly metadata: Readonly<Record<string, string>>;
}

export interface PublicationFailure {
  readonly signal: ReplaySignal;
  readonly kind: string;
  readonly message: string;
}

export interface SymbolSummary {
  readonly symbol: string;
  readonly barRows: number;
  readonly tradeRows: number;
  readonly firstTime: string;
  readonly lastTime: string;
}

export interface DatasetSummary {
  readonly name: string;
  readonly format: string;
  readonly sha256: string;
  readonly bytes: number;
  readonly rows: number;
  readonly firstTime: string;
  readonly lastTime: string;
  readonly symbols: readonly SymbolSummary[];
}

export interface StrategyRun {
  readonly kind: string;
  readonly strategyId: string;
  readonly windowSize: number;
  readonly parameters: readonly Entry[];
  /** Numeric parameters as doubles, for drawing thresholds only. */
  readonly parameterNumbers: Readonly<Record<string, number>>;
  readonly derived: readonly Entry[];
}

export interface RunWarning {
  readonly code: string;
  readonly message: string;
  readonly strategyId?: string;
}

export interface ReplayDoc {
  readonly schemaVersion: string;
  readonly provenance: readonly Entry[];
  readonly resultSha256?: string;
  readonly runId: string;
  readonly notice: string;
  readonly signalIdScope: string;
  readonly context: readonly Entry[];
  readonly dataset: DatasetSummary;
  readonly maxRows: number;
  readonly busFault: string;
  readonly strategies: readonly StrategyRun[];
  readonly warnings: readonly RunWarning[];
  readonly events: readonly ReplayEvent[];
  readonly signals: readonly ReplaySignal[];
  readonly publicationFailures: readonly PublicationFailure[];
  readonly engine: readonly Entry[];
  readonly summary: readonly Entry[];
}

type Obj = { [key: string]: LValue };

const count = (o: Obj, key: string, path: string) => asCount(need(o, key, path), `${path}.${key}`);
const str = (o: Obj, key: string, path: string) => asString(need(o, key, path), `${path}.${key}`);

function optional<T>(o: Obj, key: string, path: string, read: (v: LValue, p: string) => T): T | undefined {
  return key in o ? read(o[key]!, `${path}.${key}`) : undefined;
}

const price = (v: LValue, path: string): string => {
  const text = decimalText(v);
  if (text === undefined) throw invalid(path, "a price must be an exact plain decimal with at most 6 places (no exponent).");
  return text;
};
const quantity = (v: LValue, path: string): string => {
  const text = integerText(v);
  if (text === undefined) throw invalid(path, "a quantity must be a whole number written exactly.");
  return text;
};
const idText = (v: LValue, path: string): string => {
  const text = integerText(v);
  if (text === undefined || text.startsWith("-")) throw invalid(path, "an identifier must be a non-negative whole number written exactly.");
  return text;
};
const smallInt = (v: LValue, path: string): number => {
  const n = safeInteger(v);
  if (n === undefined || n < 0) throw invalid(path, "expected a non-negative whole number that fits a JavaScript integer.");
  return n;
};

function scalarText(v: LValue): string {
  if (isLNum(v)) return v.text;
  if (typeof v === "string") return v;
  if (v === null) return "null";
  return String(v);
}

/** Every leaf of a nested value as "a.b.c" -> text, numbers kept as the tool wrote them. */
function flatten(v: LValue, prefix: string, out: Entry[]): Entry[] {
  if (isObject(v)) {
    for (const [k, inner] of Object.entries(v)) flatten(inner, prefix ? `${prefix}.${k}` : k, out);
  } else if (Array.isArray(v)) {
    out.push({ name: prefix, text: v.every((x) => !isObject(x) && !Array.isArray(x)) ? v.map(scalarText).join(", ") : `${v.length} items` });
  } else out.push({ name: prefix, text: scalarText(v) });
  return out;
}

function stringMap(v: LValue, path: string): Record<string, string> {
  const o = asObject(v, path);
  const out: Record<string, string> = {};
  for (const [k, inner] of Object.entries(o)) out[k] = asString(inner, `${path}.${k}`);
  return out;
}

function eventResult(raw: LValue, path: string): EventResult {
  const o = asObject(raw, path);
  const signalIds = asArray(need(o, "signal_ids", path), `${path}.signal_ids`).map((v, i) => idText(v, `${path}.signal_ids[${i}]`));
  const strategyId = str(o, "strategy_id", path);
  if (!asBoolean(need(o, "diagnostics_available", path), `${path}.diagnostics_available`)) {
    const why = optional(o, "diagnostics_unavailable_reason", path, asString);
    return { strategyId, available: false, ...(why !== undefined ? { unavailableReason: why } : {}), indicators: {}, unavailable: {}, states: {}, signalIds };
  }
  const w = asObject(need(o, "window", path), `${path}.window`);
  const wp = `${path}.window`;
  const fill = optional(w, "fill", wp, smallInt);
  const remaining = optional(w, "remaining", wp, smallInt);
  const indicators: Record<string, Indicator> = {};
  for (const [name, v] of Object.entries(asObject(need(o, "indicators", path), `${path}.indicators`))) {
    const d = derivedNumber(v);
    if (!d) throw invalid(`${path}.indicators.${name}`, "expected a finite number.");
    indicators[name] = d;
  }
  return {
    strategyId, available: true, verdict: str(o, "verdict", path), reason: str(o, "reason", path), action: str(o, "action", path),
    window: { ...(fill !== undefined ? { fill } : {}), ...(remaining !== undefined ? { remaining } : {}), size: count(w, "size", wp) },
    indicators, unavailable: stringMap(need(o, "unavailable", path), `${path}.unavailable`),
    states: stringMap(need(o, "states", path), `${path}.states`), signalIds,
  };
}

function replayEvent(raw: LValue, path: string, strategyCount: number): ReplayEvent {
  const o = asObject(raw, path);
  const results = asArray(need(o, "results", path), `${path}.results`).map((r, i) => eventResult(r, `${path}.results[${i}]`));
  if (results.length !== strategyCount) throw invalid(`${path}.results`, `expected ${strategyCount} entries (one per strategy), got ${results.length}.`);
  const sourceLine = optional(o, "source_line", path, smallInt);
  const p = optional(o, "price", path, price);
  const open = optional(o, "open", path, price);
  const high = optional(o, "high", path, price);
  const low = optional(o, "low", path, price);
  const volume = optional(o, "volume", path, quantity);
  return {
    index: count(o, "index", path), symbol: str(o, "symbol", path), time: str(o, "exchange_time", path), type: str(o, "type", path),
    busSequence: count(o, "bus_sequence", path), results,
    ...(sourceLine !== undefined ? { sourceLine } : {}), ...(p !== undefined ? { price: p } : {}),
    ...(open !== undefined ? { open } : {}), ...(high !== undefined ? { high } : {}), ...(low !== undefined ? { low } : {}),
    ...(volume !== undefined ? { volume } : {}),
  };
}

function replaySignal(raw: LValue, path: string, runId: string, withSequence: boolean): ReplaySignal {
  const o = asObject(raw, path);
  const id = idText(need(o, "signal_id", path), `${path}.signal_id`);
  const ref = str(o, "signal_ref", path);
  if (ref !== `${runId}:${id}`) throw invalid(`${path}.signal_ref`, `'${ref}' is not '${runId}:${id}' (the run-scoped reference).`);
  const orderType = optional(o, "order_type", path, asString);
  const qty = optional(o, "requested_quantity", path, quantity);
  const limit = optional(o, "limit_price", path, price);
  return {
    id, ref, strategyId: str(o, "strategy_id", path), eventIndex: count(o, "event_index", path), symbol: str(o, "symbol", path),
    side: str(o, "side", path), createdAt: str(o, "created_at", path), metadata: stringMap(need(o, "metadata", path), `${path}.metadata`),
    ...(withSequence ? { busSequence: count(o, "bus_sequence", path) } : {}),
    ...(orderType !== undefined ? { orderType } : {}), ...(qty !== undefined ? { quantity: qty } : {}), ...(limit !== undefined ? { limitPrice: limit } : {}),
  };
}

function dataset(body: Obj): DatasetSummary {
  const path = "$.result.input.dataset";
  const input = asObject(need(body, "input", "$.result"), "$.result.input");
  const d = asObject(need(input, "dataset", "$.result.input"), path);
  return {
    name: str(d, "name", path), format: str(d, "format", path), sha256: str(d, "sha256", path), bytes: count(d, "bytes", path),
    rows: count(d, "rows", path), firstTime: str(d, "first_exchange_time", path), lastTime: str(d, "last_exchange_time", path),
    symbols: asArray(need(d, "symbols", path), `${path}.symbols`).map((raw, i): SymbolSummary => {
      const sp = `${path}.symbols[${i}]`;
      const s = asObject(raw, sp);
      return {
        symbol: str(s, "symbol", sp), barRows: count(s, "bar_rows", sp), tradeRows: count(s, "trade_rows", sp),
        firstTime: str(s, "first_exchange_time", sp), lastTime: str(s, "last_exchange_time", sp),
      };
    }),
  };
}

function configuration(body: Obj): { maxRows: number; busFault: string; strategies: StrategyRun[] } {
  const path = "$.result.configuration";
  const c = asObject(need(body, "configuration", "$.result"), path);
  const strategies = asArray(need(c, "strategies", path), `${path}.strategies`).map((raw, i): StrategyRun => {
    const sp = `${path}.strategies[${i}]`;
    const s = asObject(raw, sp);
    const parameters = asObject(need(s, "parameters", sp), `${sp}.parameters`);
    if ("requested_quantity" in parameters) quantity(parameters.requested_quantity!, `${sp}.parameters.requested_quantity`);
    const numbers: Record<string, number> = {};
    for (const [k, v] of Object.entries(parameters)) {
      const d = derivedNumber(v);
      if (d) numbers[k] = d.value;
    }
    return {
      kind: str(s, "kind", sp), strategyId: str(s, "strategy_id", sp), windowSize: count(s, "window_size", sp),
      parameters: flatten(parameters, "", []), parameterNumbers: numbers,
      derived: flatten(asObject(need(s, "derived", sp), `${sp}.derived`), "", []),
    };
  });
  if (strategies.length === 0) throw invalid(`${path}.strategies`, "no strategy was run.");
  const ids = strategies.map((s) => s.strategyId);
  if (new Set(ids).size !== ids.length) throw invalid(`${path}.strategies`, "a strategy_id appears twice.");
  return { maxRows: count(c, "max_rows", path), busFault: str(c, "bus_fault", path), strategies };
}

export function parseReplay(text: string): ReplayDoc {
  const root = readEnvelope(text, "strategy_lab.replay");
  const provenance = asObject(need(root, "provenance", "$"), "$.provenance");
  const body = asObject(need(root, "result", "$"), "$.result");
  const run = asObject(need(body, "run", "$.result"), "$.result.run");
  const runId = str(run, "run_id", "$.result.run");
  const mode = str(run, "mode", "$.result.run");
  if (mode !== "signal_replay") throw new LabError("unsupported_schema", `$.result.run.mode: '${mode}' is not 'signal_replay'.`);

  const ds = dataset(body);
  const cfg = configuration(body);
  const events = asArray(need(body, "events", "$.result"), "$.result.events").map((e, i) => replayEvent(e, `$.result.events[${i}]`, cfg.strategies.length));
  const signals = asArray(need(body, "signals", "$.result"), "$.result.signals").map((s, i) => replaySignal(s, `$.result.signals[${i}]`, runId, true));
  const publicationFailures = asArray(need(body, "publication_failures", "$.result"), "$.result.publication_failures").map((raw, i): PublicationFailure => {
    const fp = `$.result.publication_failures[${i}]`;
    const f = asObject(raw, fp);
    return { signal: replaySignal(f, fp, runId, false), kind: str(f, "kind", fp), message: str(f, "message", fp) };
  });
  const warnings = asArray(need(body, "warnings", "$.result"), "$.result.warnings").map((raw, i): RunWarning => {
    const wp = `$.result.warnings[${i}]`;
    const w = asObject(raw, wp);
    const strategyId = optional(w, "strategy_id", wp, asString);
    return { code: str(w, "code", wp), message: str(w, "message", wp), ...(strategyId !== undefined ? { strategyId } : {}) };
  });
  const summary = asObject(need(body, "summary", "$.result"), "$.result.summary");
  checkLinks(events, signals, cfg.strategies, summary, ds);

  const sha = provenance.result_sha256;
  return {
    schemaVersion: asString(root.schema_version!, "$.schema_version"),
    provenance: flatten(provenance, "", []), ...(typeof sha === "string" ? { resultSha256: sha } : {}),
    runId, notice: str(run, "notice", "$.result.run"), signalIdScope: str(run, "signal_id_scope", "$.result.run"),
    context: Object.entries(stringMap(need(run, "context", "$.result.run"), "$.result.run.context")).map(([name, text]) => ({ name, text })),
    dataset: ds, maxRows: cfg.maxRows, busFault: cfg.busFault, strategies: cfg.strategies, warnings, events, signals, publicationFailures,
    engine: flatten(asObject(need(body, "engine", "$.result"), "$.result.engine"), "", []), summary: flatten(summary, "", []),
  };
}

/** Every join the replay relies on, proved once. Throws invalid_structure on any disagreement. */
function checkLinks(events: ReplayEvent[], signals: ReplaySignal[], strategies: StrategyRun[], summary: Obj, ds: DatasetSummary): void {
  const slots = new Map(strategies.map((s, i) => [s.strategyId, i]));
  events.forEach((event, position) => {
    if (event.index !== position) throw invalid(`$.result.events[${position}].index`, `expected ${position}, got ${event.index}.`);
    event.results.forEach((r, slot) => {
      if (r.strategyId !== strategies[slot]!.strategyId) {
        throw invalid(`$.result.events[${position}].results[${slot}]`, `strategy '${r.strategyId}' does not match configuration '${strategies[slot]!.strategyId}'.`);
      }
    });
  });
  if (events.length !== ds.rows) throw invalid("$.result.events", `the result has ${events.length} events but the dataset reports ${ds.rows} rows.`);

  const seen = new Set<string>();
  signals.forEach((signal, position) => {
    const where = `$.result.signals[${position}]`;
    const slot = slots.get(signal.strategyId);
    if (slot === undefined) throw invalid(`${where}.strategy_id`, `unknown strategy '${signal.strategyId}'.`);
    const event = events[signal.eventIndex];
    if (!event) throw invalid(`${where}.event_index`, `${signal.eventIndex} is past the last event.`);
    const key = `${signal.strategyId}\u0000${signal.id}`;
    if (seen.has(key)) throw invalid(where, `signal ${signal.id} appears twice.`);
    seen.add(key);
    if (event.symbol !== signal.symbol) throw invalid(`${where}.symbol`, `'${signal.symbol}' differs from the symbol of event ${signal.eventIndex} ('${event.symbol}').`);
    if (!event.results[slot]!.signalIds.includes(signal.id)) throw invalid(where, `event ${signal.eventIndex} does not list signal ${signal.id}.`);
  });

  let listed = 0;
  for (const event of events) {
    for (const result of event.results) {
      for (const id of result.signalIds) {
        listed++;
        if (!seen.has(`${result.strategyId}\u0000${id}`)) throw invalid("$.result.events", `event ${event.index} lists signal ${id}, which is not in the signals array.`);
      }
    }
  }
  if (listed !== signals.length) throw invalid("$.result.signals", `events list ${listed} signals but the signals array holds ${signals.length}.`);
  for (const [name, expected] of [["events", events.length], ["signals", signals.length]] as const) {
    const reported = summary[name];
    const n = reported === undefined ? undefined : safeInteger(reported);
    if (n !== undefined && n !== expected) throw invalid(`$.result.summary.${name}`, `is ${n} but the document holds ${expected}.`);
  }
}
