// "Use in new backtest": what may be carried from the Strategies draft to the existing New backtest form, and how.
//
// Nothing is started by carrying a configuration. The form opens pre-filled and the person still presses Run backtest. The
// parameter text is carried as typed (the form keeps its own fields as text and converts only when it submits), so anything the form
// would read differently is refused here, with the reason, instead of being rounded, rewritten or dropped on the way.
//
// What the destination is: the Console's Backtests run on whatever engine the Console is connected to, which today is its in-browser
// demo simulator over its own synthetic market data. They never replay the Strategies dataset, and the notes below say so at the
// control and again on the form.

import type { DataSource, StrategyConfig } from "../api/contract";
import type { StrategySpec } from "./catalog";
import { paramCopy } from "./copy";
import { symbolEntries, type RequestSnapshot, type StrategyDraft } from "./draft";

export type CarriedKind = "sma_crossover" | "mean_reversion";

export interface CarriedStrategy {
  kind: CarriedKind;
  strategy_id: string;
  symbols: string[];
  quantity: string;
  short_window?: string;
  long_window?: string;
  lookback?: string;
  entry?: string;
  rearm?: string;
}

/** The router state the New backtest page reads. */
export interface HandoffState {
  fromStrategies: CarriedStrategy;
  /** A one-line description of where it came from, shown on the form. */
  label: string;
  /** Which settings these are, in words: the editor as it is now, or a completed comparison's configuration. */
  basis?: string;
  /** The dataset the person was working with in Strategies. It is NOT replayed by the backtest; the form says so by name. */
  dataset?: string;
}

const KINDS: readonly string[] = ["sma_crossover", "mean_reversion"];

/** The strategy parameters the form has a field for, per kind. A parameter outside this list is refused, never dropped. */
const FORM_PARAMS: Readonly<Record<CarriedKind, readonly string[]>> = {
  sma_crossover: ["strategy_id", "symbols", "requested_quantity", "short_window", "long_window"],
  mean_reversion: ["strategy_id", "symbols", "requested_quantity", "lookback", "entry_threshold", "rearm_threshold"],
};

/** Whole numbers the form reads with Number(...). */
const WHOLE: Readonly<Record<CarriedKind, readonly string[]>> = {
  sma_crossover: ["requested_quantity", "short_window", "long_window"],
  mean_reversion: ["requested_quantity", "lookback"],
};

/** Decimals the form accepts: the same pattern as its own reader (pages/NewBacktest.tsx uses this constant), so there is one definition. */
export const PLAIN_DECIMAL = /^-?\d*\.?\d+$/;

const DECIMALS: Readonly<Record<CarriedKind, readonly string[]>> = { sma_crossover: [], mean_reversion: ["entry_threshold", "rearm_threshold"] };

export const isCarried = (value: unknown): value is HandoffState => {
  const s = (value as HandoffState | null | undefined)?.fromStrategies;
  return !!s && KINDS.includes(s.kind) && typeof s.strategy_id === "string" && Array.isArray(s.symbols) && typeof s.quantity === "string" && typeof (value as HandoffState).label === "string";
};

export function toCarried(draft: StrategyDraft): CarriedStrategy | undefined {
  if (!KINDS.includes(draft.kind)) return undefined;
  const v = draft.values;
  const text = (name: string) => (v[name] ?? "").trim();
  return {
    kind: draft.kind as CarriedKind,
    strategy_id: v.strategy_id ?? draft.kind,
    symbols: symbolEntries(v.symbols ?? ""),
    quantity: text("requested_quantity"),
    ...(draft.kind === "sma_crossover" ? { short_window: text("short_window"), long_window: text("long_window") } : { lookback: text("lookback"), entry: text("entry_threshold"), rearm: text("rearm_threshold") }),
  };
}

/** The settings a completed replay actually ran with, as a draft (the parameter text that was sent, verbatim). */
export const draftFromRequest = (request: RequestSnapshot): StrategyDraft => ({ kind: request.kind, values: Object.fromEntries(request.params) });

/** Why the draft cannot be carried to the backtest form, in a sentence; undefined when it can. */
export function handoffBlocker(draft: StrategyDraft, spec: StrategySpec | undefined, issues: number, rejected: string | undefined, coverage: readonly string[] | undefined): string | undefined {
  if (!spec) return "Choose a strategy first.";
  if (!KINDS.includes(draft.kind)) return `The backtest form supports only Moving-average crossover and Mean reversion, not ${spec.title}.`;
  if (issues > 0) return "Fix the highlighted settings first.";
  if (rejected) return `The engine rejected this configuration: ${rejected}`;
  const kind = draft.kind as CarriedKind;
  const unsupported = spec.params.map((p) => p.name).filter((name) => !FORM_PARAMS[kind].includes(name));
  if (unsupported.length) return `The backtest form has no field for ${unsupported.map((n) => paramCopy(n).label).join(", ")}, so this configuration cannot be carried over without losing it.`;
  for (const name of WHOLE[kind]) {
    if (!Number.isSafeInteger(Number(draft.values[name] ?? ""))) {
      return `The backtest request carries ${paramCopy(name).label} as a JavaScript number, which is exact only up to 9,007,199,254,740,991. Use a smaller value for a backtest.`;
    }
  }
  for (const name of DECIMALS[kind]) {
    const text = (draft.values[name] ?? "").trim();
    if (!PLAIN_DECIMAL.test(text)) return `The backtest form reads plain decimals such as 2 or 0.5, not “${text}” (${paramCopy(name).label}). Rewrite it that way first.`;
  }
  const id = draft.values.strategy_id ?? "";
  if (id !== id.trim()) return "The backtest form trims spaces around the strategy id, which would change it. Remove them first.";
  const missing = symbolEntries(draft.values.symbols ?? "").filter((s) => coverage && !coverage.includes(s));
  if (missing.length) return `The Console has no recorded market data for ${missing.join(", ")}; a backtest can replay only ${coverage!.join(", ")}.`;
  return undefined;
}

// ---- what the person is told, at the control and on the form ---------------------------------------------------------------

const subject = (dataset: string | undefined): string => (dataset ? `“${dataset}”` : "the dataset used in Strategies");

/** Where the form leads: which engine and whose market data it uses. Never claims the Strategies dataset is replayed. */
export function destinationText(source: DataSource | undefined, dataset?: string): string {
  const apart = `It does not replay ${subject(dataset)}, so what it reports cannot be tied to the signals seen in Strategies.`;
  if (source === "demo") return `The New backtest form runs on the console's in-browser demo simulator over its own synthetic market data (Demo data). ${apart}`;
  if (source === "engine") return `The New backtest form is sent to the engine API this console is connected to, which replays its own recorded market data. ${apart}`;
  return `The New backtest form runs on whatever engine this console is connected to (its in-browser demo simulator when none is configured), over that engine's own market data. ${apart}`;
}

/** What travels with the button and what does not. */
export const CARRIES_TEXT = "Carried: this strategy's kind, id, symbols, shares per signal and its parameters.";
export const NOT_CARRIED_TEXT = "Not carried: the dataset and its time range, the replay settings and the other configuration. The form keeps its own period, capital, fees, slippage, latency and risk limits (its defaults), so review them.";

/** A recorded Console run strategy as a draft (the reverse direction: always an explicit "Load as draft"). */
export function draftFromRecorded(spec: StrategySpec, config: StrategyConfig, defaults: Record<string, string>): StrategyDraft {
  const values: Record<string, string> = { ...defaults, strategy_id: config.strategy_id, symbols: config.symbols.join(","), requested_quantity: String(config.requested_quantity) };
  for (const [name, value] of [["short_window", config.short_window], ["long_window", config.long_window], ["lookback", config.lookback], ["entry_threshold", config.entry_threshold], ["rearm_threshold", config.rearm_threshold]] as const) {
    if (value !== undefined && name in defaults) values[name] = String(value);
  }
  return { kind: spec.kind, values };
}
