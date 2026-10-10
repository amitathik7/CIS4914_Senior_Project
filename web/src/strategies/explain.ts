// "Why did this happen?" for one recorded event, in plain language.
//
// The sentence is built from what the tool recorded: its verdict and action, and the catalog's own wording for the reason code.
// Nothing here recomputes a decision. A bar that produced no request is silent: it is never described as a Hold signal.

import { reasonText, type StrategySpec } from "./catalog";
import { VERDICT_LABEL } from "./copy";
import type { EventResult, ReplayEvent, ReplaySignal } from "./replay";

export type Tone = "up" | "down" | "warn" | "accent" | undefined;

export interface Explanation {
  /** Short heading, e.g. "Buy request for AAPL". */
  title: string;
  tone: Tone;
  /** The badge for the verdict: Evaluated / Warming up / Ignored / Unavailable. */
  badge: { text: string; tone: Tone };
  /** Plain-language reason (the catalog's text for the recorded reason code). */
  why: string;
  /** Extra cautions that apply to this kind of event. */
  notes: string[];
}

export const ACTION_TEXT: Record<string, string> = {
  buy: "Buy request",
  sell: "Sell request",
  none: "No request",
};

/** "1 share", "100 shares": the quantity is exact text, so it is compared as text, never parsed. */
export const sharesText = (quantity: string): string => `${quantity} ${quantity === "1" ? "share" : "shares"}`;

export const SIDE_TEXT: Record<string, string> = { buy: "Buy request", sell: "Sell request" };

export function windowText(result: EventResult): string {
  const w = result.window;
  if (!w) return "not reported";
  if (w.fill === undefined) return `not tracked for this event (window size ${w.size})`;
  const left = w.remaining ?? 0;
  return `${w.fill} of ${w.size} accepted bars${left === 0 ? " (full)" : ` (${left} more needed)`}`;
}

export function explainEvent(
  spec: StrategySpec | undefined, event: ReplayEvent, result: EventResult, signals: readonly ReplaySignal[],
): Explanation {
  if (!result.available) {
    return {
      title: "No diagnostics for this event",
      tone: "warn",
      badge: { text: "Unavailable", tone: "warn" },
      why: `The strategy reported nothing for this row.${result.unavailableReason ? ` ${result.unavailableReason}` : ""}`,
      notes: ["Without diagnostics the console cannot say why a row was or was not acted on; any signal it produced is still listed below."],
    };
  }
  const meaning = reasonText(spec, result.reason ?? "") ?? `The tool recorded the reason code "${result.reason}".`;
  const verdict = result.verdict ?? "";
  const badgeText = VERDICT_LABEL[verdict] ?? verdict;

  if (result.action === "buy" || result.action === "sell") {
    const side = result.action === "buy" ? "Buy" : "Sell";
    const quantity = signals[0]?.quantity;
    return {
      title: `${side} request for ${event.symbol}${quantity ? ` (${sharesText(quantity)})` : ""}`,
      tone: result.action === "buy" ? "up" : "down",
      badge: { text: badgeText, tone: "accent" },
      why: meaning,
      notes: [
        "This is a request, not an order: risk and execution, which a signal replay does not run, decide whether anything is placed.",
        ...(result.action === "sell" ? ["A Sell request does not necessarily close a position: this strategy tracks none."] : []),
      ],
    };
  }
  if (verdict === "warming_up") {
    return { title: "Warming up: no decision yet", tone: "accent", badge: { text: badgeText, tone: undefined }, why: meaning, notes: [] };
  }
  if (verdict === "ignored") {
    return { title: `Ignored: this ${event.type} row changed nothing`, tone: "warn", badge: { text: badgeText, tone: "warn" }, why: meaning, notes: [] };
  }
  return {
    title: "Evaluated: no request",
    tone: undefined,
    badge: { text: badgeText, tone: undefined },
    why: meaning,
    notes: ["A silent bar is not a Hold signal: the strategy simply made no request on it."],
  };
}
