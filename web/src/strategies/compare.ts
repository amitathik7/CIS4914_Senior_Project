// Compare: two completed, independent replays of one dataset, joined event by event. Nothing here runs the engine.
//
// Rules this file exists to enforce:
//   * The two runs must describe the SAME recorded events (same dataset bytes, same order, same rows). Events are joined by their
//     position in that recorded order (`index`, with the source line, symbol, time, type and prices verified equal), NEVER by
//     timestamp: equal timestamps across symbols stay distinct events. A bus sequence is per-run bookkeeping and is not compared.
//   * Each side keeps its own strategy state and its own result. A signal id counts 1, 2, 3... inside every run, so two sides share
//     raw ids; every signal is keyed by its side ("A/<run id>:<id>") and a raw id alone never joins anything.
//   * "Comparable" means BOTH sides evaluated the row. A row where either side was warming up, ignored or reported nothing is not an
//     evaluated no-signal result and never counts toward agreement (see compareSummary.ts).
//   * Recorded requests are described, never ranked: there is no score, no winner and no profit here.

import { LabError } from "../api/lab";
import { compareIds, lowerBound, nextSignal, resultOf, signalsOn, type ReplayModel } from "./cursor";
import type { EventResult, ReplayDoc, ReplayEvent, ReplaySignal } from "./replay";
import type { Snapshot } from "./snapshot";

export type Member = "A" | "B";
export const MEMBERS: readonly Member[] = ["A", "B"];
export const OTHER: Readonly<Record<Member, Member>> = { A: "B", B: "A" };

/** What a side did with a row, from its recorded verdict. A missing or unknown verdict is "unavailable", never "evaluated". */
export type Standing = "evaluated" | "warming_up" | "ignored" | "unavailable";
export const STANDING_TEXT: Readonly<Record<Standing, string>> = {
  evaluated: "evaluated", warming_up: "warming up", ignored: "ignored", unavailable: "no diagnostics",
};

export function standingOf(result: EventResult): Standing {
  if (!result.available) return "unavailable";
  return result.verdict === "evaluated" || result.verdict === "warming_up" || result.verdict === "ignored" ? result.verdict : "unavailable";
}

/** agree / differ: both sides evaluated the row and made the same / different requests. not_comparable: at least one side did not evaluate it. */
export type Relation = "agree" | "differ" | "not_comparable";

export interface EventRow {
  readonly index: number;
  readonly standing: Readonly<Record<Member, Standing>>;
  /** The request sides a member made on this row, sorted and joined: "" (none), "buy", "sell" or "buy+sell". */
  readonly sides: Readonly<Record<Member, string>>;
  readonly relation: Relation;
  /** The two sides recorded different requests on this row (whatever their standing). */
  readonly requestsDiffer: boolean;
  /** The two sides did not reach the same verdict on this row. */
  readonly standingsDiffer: boolean;
}

/** A signal request with the side whose run produced it. `key` is unique across both runs; the raw `signal.id` is not. */
export interface CompareSignal {
  readonly member: Member;
  readonly key: string;
  readonly signal: ReplaySignal;
}

export interface CompareModel {
  readonly members: Readonly<Record<Member, ReplayModel>>;
  /** The shared recorded sequence (side A's copy; every aligned field is verified equal to side B's). */
  readonly events: readonly ReplayEvent[];
  readonly total: number;
  readonly symbols: readonly string[];
  readonly rows: readonly EventRow[];
  /** Both sides' requests in recorded order (by event, then A before B, then id). */
  readonly signals: readonly CompareSignal[];
  readonly signalsByEvent: ReadonlyMap<number, readonly CompareSignal[]>;
  readonly navigation: Map<string, readonly number[]>;
  readonly ribbons: Map<string, readonly Relation[]>;
}

export const memberSignalKey = (member: Member, signal: ReplaySignal): string => `${member}/${signal.ref}`;

const LABEL_SKIP: readonly string[] = ["symbols", "strategy_id", "requested_quantity"];

/** "A: Moving-average crossover": the name used where space is tight. */
export const memberShort = (member: Member, title: string): string => `${member}: ${title}`;

/** "A: Moving-average crossover (short_window 2, long_window 3)": the exact settings sent, never a nickname. */
export function memberLabel(member: Member, title: string, params: readonly (readonly [string, string])[]): string {
  const shown = params.filter(([name]) => !LABEL_SKIP.includes(name)).map(([name, text]) => `${name} ${text}`).join(", ");
  return `${memberShort(member, title)}${shown ? ` (${shown})` : ""}`;
}

// ---- alignment -------------------------------------------------------------------------------------------------------

const ALIGNED_FIELDS = ["index", "sourceLine", "symbol", "time", "type", "price", "open", "high", "low", "volume"] as const;

const contextText = (doc: ReplayDoc, name: string): string | undefined => doc.context.find((c) => c.name === name)?.text;

/** Why two documents are not the same recorded run of the same data; empty when they are comparable. */
export function alignmentProblems(a: ReplayDoc, b: ReplayDoc): string[] {
  const problems: string[] = [];
  if (a.dataset.sha256 !== b.dataset.sha256) problems.push("the datasets differ (SHA-256)");
  for (const [key, name] of [["clock", "clock"], ["replay_order", "order"]] as const) {
    if (contextText(a, key) !== contextText(b, key)) problems.push(`the replay ${name} differs`);
  }
  if (a.busFault !== b.busFault) problems.push("the bus fault policy differs");
  if (a.events.length !== b.events.length) {
    problems.push(`the event counts differ (${a.events.length} and ${b.events.length})`);
    return problems;
  }
  for (let i = 0; i < a.events.length; i++) {
    const x = a.events[i]!;
    const y = b.events[i]!;
    const field = ALIGNED_FIELDS.find((f) => x[f] !== y[f]);
    if (field) {
      problems.push(`event ${i + 1} is not the same row in both runs (${field}: ${String(x[field])} and ${String(y[field])})`);
      break;
    }
  }
  return problems;
}

// ---- building --------------------------------------------------------------------------------------------------------

const sidesOn = (model: ReplayModel, index: number): string =>
  [...new Set(signalsOn(model, index).map((s) => s.side))].sort().join("+");

/** Pair two completed snapshots. Throws LabError(comparison_mismatch) when they do not describe the same recorded events. */
export function buildCompareModel(a: Snapshot, b: Snapshot): CompareModel {
  const problems = alignmentProblems(a.doc, b.doc);
  // Both replays are launched at the same moment, but the gateway looks the executable up for each, so a rebuild between the two
  // starts could pair two different engines under one identity (the comparison id names one executable).
  if (a.meta.runner.sha256 !== b.meta.runner.sha256) problems.push("the two replays ran on different replay executables (SHA-256)");
  if (problems.length) throw new LabError("comparison_mismatch", `The two runs cannot be compared: ${problems.join("; ")}.`);
  const members = { A: a.model, B: b.model } as const;
  const rows: EventRow[] = a.model.events.map((event, index) => {
    const standing = { A: standingOf(resultOf(members.A, event)), B: standingOf(resultOf(members.B, b.model.events[index]!)) };
    const sides = { A: sidesOn(members.A, index), B: sidesOn(members.B, index) };
    const requestsDiffer = sides.A !== sides.B;
    const relation: Relation = standing.A === "evaluated" && standing.B === "evaluated" ? (requestsDiffer ? "differ" : "agree") : "not_comparable";
    return { index, standing, sides, relation, requestsDiffer, standingsDiffer: standing.A !== standing.B };
  });
  const signals: CompareSignal[] = MEMBERS.flatMap((member) => members[member].signals.map((signal) => ({ member, key: memberSignalKey(member, signal), signal })));
  signals.sort((x, y) => x.signal.eventIndex - y.signal.eventIndex || MEMBERS.indexOf(x.member) - MEMBERS.indexOf(y.member) || compareIds(x.signal.id, y.signal.id));
  const signalsByEvent = new Map<number, CompareSignal[]>();
  for (const s of signals) {
    const list = signalsByEvent.get(s.signal.eventIndex);
    if (list) list.push(s);
    else signalsByEvent.set(s.signal.eventIndex, [s]);
  }
  return {
    members, events: a.model.events, total: a.model.total, symbols: a.model.symbols, rows, signals, signalsByEvent,
    navigation: new Map(), ribbons: new Map(),
  };
}

// ---- the shared cursor -----------------------------------------------------------------------------------------------

/** Which rows "Next difference" stops at. `requests`: the sides requested differently. `decisions`: also where they reached a different verdict. */
export type DiffMode = "requests" | "decisions";
export const DIFF_MODE_TEXT: Readonly<Record<DiffMode, string>> = {
  requests: "Requests differ",
  decisions: "Requests or verdicts differ",
};

const differs = (row: EventRow, mode: DiffMode): boolean => row.requestsDiffer || (mode === "decisions" && row.standingsDiffer);

/** Cursor positions (event index + 1) of the rows that count as a difference, optionally for one display symbol. Sorted. */
export function differencePositions(model: CompareModel, mode: DiffMode, symbol: string | null): readonly number[] {
  const key = `${mode}|${symbol === null ? "*" : `s:${symbol}`}`;
  const cached = model.navigation.get(key);
  if (cached) return cached;
  const source = symbol === null ? undefined : model.members.A.indexesBySymbol.get(symbol) ?? [];
  const found: number[] = [];
  if (source) {
    for (const index of source) if (differs(model.rows[index]!, mode)) found.push(index + 1);
  } else {
    for (const row of model.rows) if (differs(row, mode)) found.push(row.index + 1);
  }
  model.navigation.set(key, found);
  return found;
}

/** The next position after `cursor` that reveals a difference; undefined when none is left. */
export function nextDifference(model: CompareModel, cursor: number, symbol: string | null, mode: DiffMode): number | undefined {
  const positions = differencePositions(model, mode, symbol);
  const k = lowerBound(positions, cursor + 1);
  return k < positions.length ? positions[k] : undefined;
}

/** The next position where EITHER side made a request (for the display filter). */
export function nextSignalEither(model: CompareModel, cursor: number, symbol: string | null): number | undefined {
  const found = MEMBERS.map((m) => nextSignal(model.members[m], cursor, symbol)).filter((p): p is number => p !== undefined);
  return found.length ? Math.min(...found) : undefined;
}

/** One relation per row of a symbol, in recorded order, for the difference ribbon under the charts. */
export function ribbonFor(model: CompareModel, symbol: string): readonly Relation[] {
  const cached = model.ribbons.get(symbol);
  if (cached) return cached;
  const ribbon = (model.members.A.indexesBySymbol.get(symbol) ?? []).map((i) => model.rows[i]!.relation);
  model.ribbons.set(symbol, ribbon);
  return ribbon;
}

export const signalsAt = (model: CompareModel, eventIndex: number): readonly CompareSignal[] => model.signalsByEvent.get(eventIndex) ?? [];

/** Requests of both sides in the visible prefix (event index below the cursor), under the display filter. */
export const compareSignalsThrough = (model: CompareModel, cursor: number, symbol: string | null): CompareSignal[] =>
  model.signals.filter((s) => s.signal.eventIndex < cursor && (symbol === null || s.signal.symbol === symbol));
