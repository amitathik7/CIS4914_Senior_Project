// Replay over a COMPLETED result. Nothing in here runs the engine.
//
// The cursor is a number of events revealed, 0..N, in the order the tool recorded them (file order: equal timestamps and
// interleaved symbols exactly as replayed). At cursor c, events 0..c-1 are the "visible prefix" and event c-1 is the selected
// event; c = 0 shows nothing. Everything shown as replay state (chart, markers, tables, counts, inspector) derives from the
// prefix only, so stepping back removes the future.
//
// A `symbol` argument is a DISPLAY filter: null means every symbol. A filter hides rows; it never moves the cursor and never
// changes a decision. Events and signals are joined by their integer indexes and ids, never by timestamp, so equal timestamps
// across symbols stay distinct.

import type { ReplayDoc, ReplayEvent, ReplaySignal, EventResult } from "./replay";

export interface ReplayModel {
  readonly doc: ReplayDoc;
  readonly slot: number;
  readonly strategyId: string;
  readonly kind: string;
  readonly events: readonly ReplayEvent[];
  readonly total: number;
  /** Symbols in the dataset's own order. */
  readonly symbols: readonly string[];
  readonly indexesBySymbol: ReadonlyMap<string, readonly number[]>;
  /** This strategy's signals in publish order. */
  readonly signals: readonly ReplaySignal[];
  readonly signalsByEvent: ReadonlyMap<number, readonly ReplaySignal[]>;
  /** Cursor positions (event index + 1) that follow a signal, for all symbols (key null) or one. */
  readonly signalPositions: ReadonlyMap<string | null, readonly number[]>;
  readonly tracks: Map<string, SymbolTrack>;
}

/** Order two non-negative integer ids written as text (no leading zeros): by length, then digit by digit. Exact for any int64. */
export const compareIds = (a: string, b: string): number => a.length - b.length || (a < b ? -1 : a > b ? 1 : 0);

export function buildModel(doc: ReplayDoc, strategyId?: string): ReplayModel {
  const slot = strategyId ? doc.strategies.findIndex((s) => s.strategyId === strategyId) : 0;
  if (slot < 0) throw new RangeError(`no strategy '${strategyId}' in this result`);
  const strategy = doc.strategies[slot]!;
  const indexesBySymbol = new Map<string, number[]>();
  for (const event of doc.events) {
    const list = indexesBySymbol.get(event.symbol);
    if (list) list.push(event.index);
    else indexesBySymbol.set(event.symbol, [event.index]);
  }
  const listed = doc.dataset.symbols.map((s) => s.symbol);
  const symbols = [...listed, ...[...indexesBySymbol.keys()].filter((s) => !listed.includes(s))];

  const signals = doc.signals.filter((s) => s.strategyId === strategy.strategyId).sort((a, b) => a.eventIndex - b.eventIndex || compareIds(a.id, b.id));
  const signalsByEvent = new Map<number, ReplaySignal[]>();
  for (const s of signals) {
    const list = signalsByEvent.get(s.eventIndex);
    if (list) list.push(s);
    else signalsByEvent.set(s.eventIndex, [s]);
  }
  const positions = (keep: (s: ReplaySignal) => boolean) => [...new Set(signals.filter(keep).map((s) => s.eventIndex + 1))].sort((a, b) => a - b);
  const signalPositions = new Map<string | null, readonly number[]>([[null, positions(() => true)]]);
  for (const symbol of symbols) signalPositions.set(symbol, positions((s) => s.symbol === symbol));

  return {
    doc, slot, strategyId: strategy.strategyId, kind: strategy.kind, events: doc.events, total: doc.events.length, symbols,
    indexesBySymbol, signals, signalsByEvent, signalPositions, tracks: new Map(),
  };
}

// ---- the cursor --------------------------------------------------------------------------------------------------

export const clamp = (model: ReplayModel, cursor: number): number => Math.max(0, Math.min(model.total, Math.trunc(cursor)));

/** First index in a sorted array whose value is >= x. */
export function lowerBound(sorted: readonly number[], x: number): number {
  let lo = 0;
  let hi = sorted.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (sorted[mid]! < x) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

const matching = (model: ReplayModel, symbol: string | null): readonly number[] | undefined =>
  symbol === null ? undefined : model.indexesBySymbol.get(symbol) ?? [];

/** The position that reveals the next event of `symbol` (any symbol when null); undefined at the end. */
export function nextEvent(model: ReplayModel, cursor: number, symbol: string | null): number | undefined {
  const list = matching(model, symbol);
  if (!list) return cursor < model.total ? cursor + 1 : undefined;
  const k = lowerBound(list, cursor);
  return k < list.length ? list[k]! + 1 : undefined;
}

/** The position showing the previous event of `symbol`; 0 when none is left; undefined at 0. */
export function previousEvent(model: ReplayModel, cursor: number, symbol: string | null): number | undefined {
  if (cursor <= 0) return undefined;
  const list = matching(model, symbol);
  if (!list) return cursor - 1;
  const k = lowerBound(list, cursor - 1);
  return k > 0 ? list[k - 1]! + 1 : 0;
}

/** The position of the next event after the cursor that produced a signal for the filter. One stop per event. */
export function nextSignal(model: ReplayModel, cursor: number, symbol: string | null): number | undefined {
  const positions = model.signalPositions.get(symbol) ?? [];
  const k = lowerBound(positions, cursor + 1);
  return k < positions.length ? positions[k]! : undefined;
}

// ---- what the prefix shows ---------------------------------------------------------------------------------------------

export const selectedEvent = (model: ReplayModel, cursor: number): ReplayEvent | undefined =>
  cursor >= 1 && cursor <= model.total ? model.events[cursor - 1] : undefined;

export const resultOf = (model: ReplayModel, event: ReplayEvent): EventResult => event.results[model.slot]!;

export const signalsOn = (model: ReplayModel, eventIndex: number): readonly ReplaySignal[] => model.signalsByEvent.get(eventIndex) ?? [];

export const eventsThrough = (model: ReplayModel, cursor: number, symbol: string | null): ReplayEvent[] => {
  const prefix = model.events.slice(0, clamp(model, cursor));
  return symbol === null ? prefix : prefix.filter((e) => e.symbol === symbol);
};

export const signalsThrough = (model: ReplayModel, cursor: number, symbol: string | null): ReplaySignal[] =>
  model.signals.filter((s) => s.eventIndex < cursor && (symbol === null || s.symbol === symbol));

export interface PrefixCounts {
  events: number;
  bars: number;
  buys: number;
  sells: number;
  verdicts: Record<"ignored" | "warming_up" | "evaluated" | "unavailable", number>;
  reasons: Map<string, number>;
}

export function counts(model: ReplayModel, cursor: number, symbol: string | null): PrefixCounts {
  const out: PrefixCounts = { events: 0, bars: 0, buys: 0, sells: 0, verdicts: { ignored: 0, warming_up: 0, evaluated: 0, unavailable: 0 }, reasons: new Map() };
  for (const event of eventsThrough(model, cursor, symbol)) {
    out.events++;
    if (event.type === "bar") out.bars++;
    const result = resultOf(model, event);
    if (!result.available) {
      out.verdicts.unavailable++;
      continue;
    }
    const verdict = result.verdict as keyof PrefixCounts["verdicts"];
    out.verdicts[verdict in out.verdicts ? verdict : "unavailable"]++;
    out.reasons.set(result.reason ?? "unknown", (out.reasons.get(result.reason ?? "unknown") ?? 0) + 1);
  }
  for (const s of signalsThrough(model, cursor, symbol)) {
    if (s.side === "buy") out.buys++;
    else if (s.side === "sell") out.sells++;
  }
  return out;
}

// ---- one symbol's rows, ready for drawing -------------------------------------------------------------------------------

/** The prices drawn are plotting coordinates ONLY. This is the one place a price text becomes a Number; the exact text stays on the event. */
export const plotNumber = (text: string | undefined): number | null => {
  if (text === undefined) return null;
  const n = Number(text);
  return Number.isFinite(n) ? n : null;
};

export interface SymbolTrack {
  readonly symbol: string;
  /** Event indexes of this symbol's rows, in recorded order (all of them: the chart reads only the revealed prefix). */
  readonly indexes: readonly number[];
  readonly close: readonly (number | null)[];
  readonly types: readonly string[];
  readonly verdicts: readonly (string | undefined)[];
  readonly sides: readonly (string | null)[];
  readonly indicators: Readonly<Record<string, readonly (number | null)[]>>;
}

export function trackFor(model: ReplayModel, symbol: string, indicatorNames: readonly string[]): SymbolTrack {
  const key = `${symbol}\u0000${indicatorNames.join(",")}`;
  const cached = model.tracks.get(key);
  if (cached) return cached;
  const indexes = model.indexesBySymbol.get(symbol) ?? [];
  const indicators: Record<string, (number | null)[]> = Object.fromEntries(indicatorNames.map((n) => [n, [] as (number | null)[]]));
  const close: (number | null)[] = [];
  const types: string[] = [];
  const verdicts: (string | undefined)[] = [];
  const sides: (string | null)[] = [];
  for (const index of indexes) {
    const event = model.events[index]!;
    const result = resultOf(model, event);
    close.push(plotNumber(event.price));
    types.push(event.type);
    verdicts.push(result.available ? result.verdict : undefined);
    sides.push(signalsOn(model, index)[0]?.side ?? null);
    for (const name of indicatorNames) indicators[name]!.push(result.indicators[name]?.value ?? null); // a missing value is a gap, never 0
  }
  const track: SymbolTrack = { symbol, indexes, close, types, verdicts, sides, indicators };
  model.tracks.set(key, track);
  return track;
}

/** How many of a track's rows the cursor has revealed. */
export const revealed = (track: SymbolTrack, cursor: number): number => lowerBound(track.indexes, cursor);
