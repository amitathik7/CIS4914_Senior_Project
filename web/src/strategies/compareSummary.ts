// Signal-level summaries of a comparison: counts per side, and how often the two sides agreed.
//
// DEFINITIONS (shown in the UI next to every figure, because a ratio without its denominator misleads):
//   comparable event  a recorded row that BOTH sides evaluated. A row where either side was warming up, ignored or reported no
//                     diagnostics is not comparable: "nothing happened yet" is not the same thing as "evaluated, no request".
//   agree             comparable, and both sides made the same request: both none, both Buy, or both Sell.
//   differ            comparable, and the requests are different (one side asked and the other did not, or Buy against Sell).
// Counts are counts of recorded requests, rows and verdicts. They are not a measure of profitability or of quality.

import { counts, lowerBound, type PrefixCounts } from "./cursor";
import { MEMBERS, STANDING_TEXT, type CompareModel, type EventRow, type Member } from "./compare";

export interface Agreement {
  /** Rows both sides evaluated: the denominator of every agreement figure. */
  comparable: number;
  agree: number;
  differ: number;
  /** Rows at least one side did not evaluate. */
  notComparable: number;
  /** Why those rows were left out, most common first. */
  excluded: { label: string; count: number }[];
  /** Rows (of any standing) where the two sides requested differently. */
  requestDifferences: number;
  /** Rows where the two sides did not reach the same verdict. */
  verdictDifferences: number;
}

export interface CompareSummary {
  /** Rows covered (the visible prefix under the display filter, or the whole run). */
  events: number;
  members: Readonly<Record<Member, PrefixCounts>>;
  agreement: Agreement;
}

function excludedLabel(row: EventRow): string {
  const a = STANDING_TEXT[row.standing.A];
  const b = STANDING_TEXT[row.standing.B];
  return row.standing.A === row.standing.B ? `both ${a}` : `A ${a} · B ${b}`;
}

function agreementOver(rows: Iterable<EventRow>): Agreement {
  const out: Agreement = { comparable: 0, agree: 0, differ: 0, notComparable: 0, excluded: [], requestDifferences: 0, verdictDifferences: 0 };
  const why = new Map<string, number>();
  for (const row of rows) {
    if (row.requestsDiffer) out.requestDifferences++;
    if (row.standingsDiffer) out.verdictDifferences++;
    if (row.relation === "not_comparable") {
      out.notComparable++;
      const label = excludedLabel(row);
      why.set(label, (why.get(label) ?? 0) + 1);
    } else {
      out.comparable++;
      if (row.relation === "agree") out.agree++;
      else out.differ++;
    }
  }
  out.excluded = [...why.entries()].map(([label, count]) => ({ label, count })).sort((x, y) => y.count - x.count || (x.label < y.label ? -1 : 1));
  return out;
}

/** The rows the visible prefix covers under the display filter, in recorded order. */
export function rowsThrough(model: CompareModel, cursor: number, symbol: string | null): EventRow[] {
  const end = Math.max(0, Math.min(model.total, Math.trunc(cursor)));
  if (symbol === null) return model.rows.slice(0, end);
  const indexes = model.members.A.indexesBySymbol.get(symbol) ?? [];
  return indexes.slice(0, lowerBound(indexes, end)).map((i) => model.rows[i]!);
}

/** "Replay so far": events 1..cursor under the display filter. Both sides read the same rows. */
export function summarize(model: CompareModel, cursor: number, symbol: string | null): CompareSummary {
  const members = Object.fromEntries(MEMBERS.map((m) => [m, counts(model.members[m], cursor, symbol)])) as Record<Member, PrefixCounts>;
  return { events: members.A.events, members, agreement: agreementOver(rowsThrough(model, cursor, symbol)) };
}

/** "Full run": every recorded event of every symbol, whatever the replay position or filter. */
export const summarizeFull = (model: CompareModel): CompareSummary => summarize(model, model.total, null);

/** "83.3%": integer arithmetic only. Undefined when nothing was comparable (a ratio of zero events says nothing). */
export function agreementPercent(agree: number, comparable: number): string | undefined {
  if (comparable <= 0) return undefined;
  const tenths = Math.round((agree * 1000) / comparable);
  return `${Math.floor(tenths / 10)}.${tenths % 10}%`;
}

export const AGREEMENT_DEFINITION =
  "Comparable events are rows that both configurations evaluated; rows where either was warming up, ignored or reported nothing are left out. " +
  "A comparable row agrees when both made the same request (none, Buy or Buy, Sell or Sell).";

export const COUNTS_ARE_NOT_PERFORMANCE =
  "These are counts of recorded requests, rows and verdicts. They say nothing about profit, loss or quality: no order was placed or filled.";

// ---- readiness ------------------------------------------------------------------------------------------------------

export interface Readiness {
  symbol: string;
  state: "ready" | "warming_up" | "not_tracked" | "no_rows";
  fill?: number;
  size?: number;
  /** The event number (1-based) whose recorded window is quoted. */
  asOfEvent?: number;
}

/**
 * Per symbol: is this side's recorded window full, as of the latest revealed row of that symbol that reports one?
 * Read from the recorded window only. It is NOT inferred from requests: a ready strategy may well request nothing.
 */
export function readiness(model: CompareModel, member: Member, cursor: number, symbol: string | null): Readiness[] {
  const side = model.members[member];
  const shown = symbol === null ? side.symbols : [symbol];
  return shown.map((sym): Readiness => {
    const indexes = side.indexesBySymbol.get(sym) ?? [];
    const revealed = lowerBound(indexes, cursor);
    if (revealed === 0) return { symbol: sym, state: "no_rows" };
    for (let k = revealed - 1; k >= 0; k--) {
      const event = side.events[indexes[k]!]!;
      const window = event.results[side.slot]!.window;
      if (window?.fill !== undefined) {
        return { symbol: sym, state: (window.remaining ?? 0) === 0 ? "ready" : "warming_up", fill: window.fill, size: window.size, asOfEvent: event.index + 1 };
      }
    }
    return { symbol: sym, state: "not_tracked", asOfEvent: indexes[revealed - 1]! + 1 };
  });
}

export const READINESS_TEXT: Readonly<Record<Readiness["state"], string>> = {
  ready: "Ready",
  warming_up: "Warming up",
  not_tracked: "No bar has reached the strategy",
  no_rows: "No rows revealed yet",
};

export function readinessText(r: Readiness): string {
  const base = READINESS_TEXT[r.state];
  const progress = r.fill !== undefined ? `: ${r.fill} of ${r.size} accepted bars` : "";
  return `${base}${progress}${r.asOfEvent !== undefined && r.state !== "no_rows" ? ` (as of event ${r.asOfEvent})` : ""}`;
}
