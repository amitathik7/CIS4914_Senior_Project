// Quick starts for Compare: two ready configurations to put in A and B. They only FILL the editors; nothing runs.
// Parameter values come from the chosen dataset's own example presets (the lab's tests use them) or, failing that, the engine's
// defaults. They are demonstrations, not recommendations.

import type { BuiltinDataset } from "../api/lab";
import { specOf, type Catalog, type StrategySpec } from "./catalog";
import { defaultValues, type StrategyDraft } from "./draft";

export type QuickStartId = "crossover_vs_reversion" | "crossover_variants";

export const QUICK_STARTS: readonly { id: QuickStartId; title: string; summary: string }[] = [
  { id: "crossover_vs_reversion", title: "Crossover vs mean reversion", summary: "A: moving-average crossover. B: mean reversion. Two different strategies on the same bars." },
  { id: "crossover_variants", title: "Two crossover variants", summary: "A: a crossover. B: the same crossover with both windows lengthened. One strategy, two parameter sets." },
];

export interface QuickStartPlan {
  A: StrategyDraft;
  B: StrategyDraft;
  labelA: string;
  labelB: string;
}

const exampleFor = (spec: StrategySpec, dataset: BuiltinDataset | undefined): StrategyDraft => ({
  kind: spec.kind,
  values: { ...defaultValues(spec), ...(dataset?.presets[spec.kind] ?? {}) },
});

const WHOLE = /^\d+$/;

/** The same crossover with the short window one bar longer and the long window two bars longer (valid whenever the original is). */
export function lengthenWindows(draft: StrategyDraft): StrategyDraft | undefined {
  const short = (draft.values.short_window ?? "").trim();
  const long = (draft.values.long_window ?? "").trim();
  if (draft.kind !== "sma_crossover" || !WHOLE.test(short) || !WHOLE.test(long)) return undefined;
  return { ...draft, values: { ...draft.values, short_window: (BigInt(short) + 1n).toString(), long_window: (BigInt(long) + 2n).toString() } };
}

/** The two drafts for a quick start, or the reason it cannot be offered (a strategy the engine does not list). */
export function planQuickStart(id: QuickStartId, catalog: Catalog | undefined, dataset: BuiltinDataset | undefined): QuickStartPlan | { error: string } {
  const sma = specOf(catalog, "sma_crossover");
  if (!sma?.available) return { error: "The engine does not offer the moving-average crossover strategy." };
  const a = exampleFor(sma, dataset);
  if (id === "crossover_variants") {
    const b = lengthenWindows(a);
    return b ? { A: a, B: b, labelA: "Quick start: crossover", labelB: "Quick start: crossover, windows lengthened" } : { error: "The crossover windows are not whole numbers, so no variant can be derived." };
  }
  const mr = specOf(catalog, "mean_reversion");
  if (!mr?.available) return { error: "The engine does not offer the mean reversion strategy." };
  return { A: a, B: exampleFor(mr, dataset), labelA: "Quick start: crossover", labelB: "Quick start: mean reversion" };
}
