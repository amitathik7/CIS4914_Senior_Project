// The editable draft of one strategy configuration, its client-side checks, and the request it becomes.
//
// A draft is TEXT per parameter, exactly as typed: nothing is converted to a number on the way to the engine, so a value the
// engine would read exactly (a 19-digit quantity, 0.1 as a threshold) is never altered by JavaScript.
//
// What is checked here is only what the catalog itself declares mechanically (a parameter's type, whether it is required, the
// allowlist rules its constraint text states). Cross-field and range rules (short < long, lookback >= 2, entry > re-arm...)
// are NOT copied into the console: the real strategy constructors decide them (see store.ts, the "engine check") and the
// replay is refused at the gateway boundary otherwise.

import type { StrategyRequest } from "../api/lab";
import type { StrategySpec } from "./catalog";
import { paramCopy } from "./copy";

export interface StrategyDraft {
  readonly kind: string;
  readonly values: Readonly<Record<string, string>>;
}

export interface FieldIssue {
  name: string;
  message: string;
}

export interface DatasetIdentity {
  kind: "builtin" | "upload";
  name: string;
  sha256: string;
}

/** Everything that decides a replay's result: the strategy, its parameter text, and the exact dataset bytes. */
export interface RequestSnapshot {
  kind: string;
  params: [string, string][];
  dataset: DatasetIdentity;
}

export const defaultValues = (spec: StrategySpec): Record<string, string> =>
  Object.fromEntries(spec.params.map((p) => [p.name, p.defaultText ?? ""]));

export const newDraft = (spec: StrategySpec): StrategyDraft => ({ kind: spec.kind, values: defaultValues(spec) });

/** Spaces and tabs around each comma-separated entry are ignored; nothing else changes (case is kept, an empty entry stays empty). */
export const normalizeSymbols = (text: string): string => text.split(",").map((s) => s.replace(/^[ \t]+|[ \t]+$/g, "")).join(",");

export const symbolEntries = (text: string): string[] => (text.trim() === "" ? [] : normalizeSymbols(text).split(","));

export const withValue = (draft: StrategyDraft, name: string, text: string): StrategyDraft => ({ ...draft, values: { ...draft.values, [name]: text } });

export function sameDraft(a: StrategyDraft, b: StrategyDraft): boolean {
  const keys = new Set([...Object.keys(a.values), ...Object.keys(b.values)]);
  return a.kind === b.kind && [...keys].every((k) => a.values[k] === b.values[k]);
}

const UINT = /^\d+$/;
const DOUBLE = /^-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/;

export function validateDraft(spec: StrategySpec, draft: StrategyDraft): FieldIssue[] {
  const issues: FieldIssue[] = [];
  for (const p of spec.params) {
    const text = draft.values[p.name] ?? "";
    const add = (message: string) => issues.push({ name: p.name, message });
    if (text.trim() === "") {
      add(p.type === "string_list" ? "Enter at least one symbol." : p.defaultText ? `Enter a value, or restore the default (${p.defaultText}).` : "Enter a value.");
      continue;
    }
    if (p.type === "uint" && !UINT.test(text.trim())) add("Enter a whole number: digits only, no sign or decimals.");
    else if (p.type === "double" && !DOUBLE.test(text.trim())) add("Enter a finite number, like 2 or 0.5.");
    else if (p.type === "string_list") {
      const entries = symbolEntries(text);
      const seen = new Set<string>();
      if (entries.some((e) => e === "")) add("Remove the empty entry between commas.");
      for (const e of entries) {
        if (e !== "" && seen.has(e)) {
          add(`${e} is listed twice.`);
          break;
        }
        seen.add(e);
      }
    }
  }
  return issues;
}

/** The parameters in catalog order, as the text that will be sent. */
export function buildRequest(spec: StrategySpec, draft: StrategyDraft): StrategyRequest {
  return {
    kind: spec.kind,
    params: spec.params.map((p): [string, string] => {
      const text = draft.values[p.name] ?? "";
      return [p.name, p.type === "string_list" ? normalizeSymbols(text) : p.type === "string" ? text : text.trim()];
    }),
  };
}

export const requestKey = (r: RequestSnapshot): string => JSON.stringify([r.kind, r.params, r.dataset.kind, r.dataset.sha256]);

/** What differs between two requests, in words (for the "inputs changed" status). */
export function describeChanges(before: RequestSnapshot, after: RequestSnapshot, specs: (kind: string) => StrategySpec | undefined): string[] {
  const out: string[] = [];
  if (before.dataset.sha256 !== after.dataset.sha256) out.push(`Dataset: ${before.dataset.name} → ${after.dataset.name}`);
  if (before.kind !== after.kind) {
    out.push(`Strategy: ${specs(before.kind)?.title ?? before.kind} → ${specs(after.kind)?.title ?? after.kind}`);
    return out;
  }
  const was = new Map(before.params);
  for (const [name, text] of after.params) {
    if (was.get(name) !== text) out.push(`${paramCopy(name).label}: ${was.get(name) ?? "(not set)"} → ${text}`);
  }
  return out;
}

/** A compact one-line summary of a request, for the status strip and the preset list. */
export function summarize(spec: StrategySpec | undefined, params: readonly (readonly [string, string])[]): string {
  const shown = params.filter(([name]) => name !== "strategy_id" && name !== "requested_quantity");
  return shown.map(([name, text]) => `${paramCopy(name).label.toLowerCase()} ${text}`).join(", ") || (spec?.title ?? "");
}
