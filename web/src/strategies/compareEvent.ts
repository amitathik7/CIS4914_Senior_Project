// One recorded event, side by side: what each configuration decided and why, field by field.
//
// Everything is read from each side's OWN result for the row (a side's document is never consulted for the other). "differs" is
// stated only where both sides reported the field; where only one did (an indicator that exists for one strategy kind but not the
// other) the row says so instead of pretending the two disagree.

import { unavailableText } from "./copy";
import { resultOf } from "./cursor";
import { ACTION_TEXT, windowText } from "./explain";
import { MEMBERS, STANDING_TEXT, standingOf, type CompareModel, type Member } from "./compare";
import type { EventResult } from "./replay";

export type FieldStatus = "same" | "differs" | "not_comparable";

export interface FieldRow {
  /** Which kind of difference this row can show: decision fields drive "Next difference"; the others explain it. */
  group: "decision" | "readiness" | "indicator" | "state";
  label: string;
  a: string;
  b: string;
  status: FieldStatus;
}

const NOT_REPORTED = "not reported by this strategy";

function indicatorText(result: EventResult, name: string): string | undefined {
  const value = result.indicators[name];
  if (value) return value.text;
  const why = result.unavailable[name];
  return why === undefined ? undefined : `unavailable: ${unavailableText(why)}`;
}

function row(group: FieldRow["group"], label: string, a: string | undefined, b: string | undefined): FieldRow {
  return { group, label, a: a ?? NOT_REPORTED, b: b ?? NOT_REPORTED, status: a === undefined || b === undefined ? "not_comparable" : a === b ? "same" : "differs" };
}

export const requestText = (sides: string): string =>
  sides === "" ? "No request" : sides.split("+").map((s) => ACTION_TEXT[s] ?? s).join(" + ");

/** The fields of one event for the inspector: decisions first, then readiness, indicators and state. */
export function compareEvent(model: CompareModel, eventIndex: number): FieldRow[] {
  const results = Object.fromEntries(MEMBERS.map((m) => [m, resultOf(model.members[m], model.members[m].events[eventIndex]!)])) as Record<Member, EventResult>;
  const { A: a, B: b } = results;
  const eventRow = model.rows[eventIndex]!;
  const out: FieldRow[] = [
    row("decision", "Verdict", a.available ? STANDING_TEXT[standingOf(a)] : "no diagnostics", b.available ? STANDING_TEXT[standingOf(b)] : "no diagnostics"),
    row("decision", "Reason code", a.available ? a.reason : undefined, b.available ? b.reason : undefined),
    row("decision", "Request", requestText(eventRow.sides.A), requestText(eventRow.sides.B)),
    row("readiness", "Window", a.available ? windowText(a) : undefined, b.available ? windowText(b) : undefined),
  ];
  const names = [...new Set([...Object.keys(a.indicators), ...Object.keys(a.unavailable), ...Object.keys(b.indicators), ...Object.keys(b.unavailable)])];
  for (const name of names) out.push(row("indicator", name, indicatorText(a, name), indicatorText(b, name)));
  const states = [...new Set([...Object.keys(a.states), ...Object.keys(b.states)])];
  for (const name of states) out.push(row("state", name.replace(/_/g, " "), a.states[name], b.states[name]));
  return out;
}

/** What the two sides recorded on this row, in words. A description, never a verdict on either configuration. */
export function agreementSentence(model: CompareModel, eventIndex: number): string {
  const row = model.rows[eventIndex]!;
  const { A, B } = row.sides;
  if (row.relation === "not_comparable") {
    const why = row.standing.A === row.standing.B ? `both configurations were ${STANDING_TEXT[row.standing.A]}` : `A was ${STANDING_TEXT[row.standing.A]} and B was ${STANDING_TEXT[row.standing.B]}`;
    const asks = A || B ? ` Requests: A ${requestText(A).toLowerCase()}; B ${requestText(B).toLowerCase()}.` : "";
    return `Not comparable: ${why}, so this row is left out of the agreement figures.${asks}`;
  }
  if (!A && !B) return "Both configurations evaluated this row and neither made a request: they agree.";
  if (A === B) return `Both configurations evaluated this row and made the same request (${requestText(A)}): they agree.`;
  if (A && B) return `Opposing or different requests on this row: A ${requestText(A)}, B ${requestText(B)}.`;
  return A ? `Only A made a request here (${requestText(A)}); B evaluated the row and made none.` : `Only B made a request here (${requestText(B)}); A evaluated the row and made none.`;
}
