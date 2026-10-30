// Named configuration presets, kept in this browser's localStorage under the console's "te." prefix (like te.theme and
// te.demo.backtests). They never leave the machine and are not shared; the UI says so. Anything unreadable is dropped, and a
// browser without storage still works for the page session (the caller is told the save did not persist).

import type { BuiltinDataset } from "../api/lab";
import type { StrategySpec } from "./catalog";
import { defaultValues, type StrategyDraft } from "./draft";

export const PRESETS_KEY = "te.strategies.presets";
const MAX_PRESETS = 50;
const MAX_NAME = 60;
const MAX_TEXT = 4096;

export interface Preset {
  id: string;
  name: string;
  savedAt: string;
  draft: StrategyDraft;
}

export interface ExamplePreset {
  id: string;
  name: string;
  draft: StrategyDraft;
}

type ReadableStorage = Pick<Storage, "getItem">;
type WritableStorage = Pick<Storage, "setItem">;

function validDraft(raw: unknown): StrategyDraft | undefined {
  if (typeof raw !== "object" || raw === null) return undefined;
  const { kind, values } = raw as { kind?: unknown; values?: unknown };
  if (typeof kind !== "string" || typeof values !== "object" || values === null || Array.isArray(values)) return undefined;
  const entries = Object.entries(values as Record<string, unknown>);
  if (!entries.every(([, v]) => typeof v === "string" && v.length <= MAX_TEXT)) return undefined;
  return { kind, values: Object.fromEntries(entries) as Record<string, string> };
}

export function loadPresets(storage: ReadableStorage | undefined = safeStorage()): Preset[] {
  try {
    const parsed = JSON.parse(storage?.getItem(PRESETS_KEY) ?? "null") as { v?: unknown; presets?: unknown } | null;
    if (parsed?.v !== 1 || !Array.isArray(parsed.presets)) return [];
    const out: Preset[] = [];
    for (const item of parsed.presets as unknown[]) {
      if (typeof item !== "object" || item === null) continue; // one bad entry must not cost the person the rest
      const p = item as { id?: unknown; name?: unknown; savedAt?: unknown; draft?: unknown };
      const draft = validDraft(p.draft);
      if (typeof p.id === "string" && typeof p.name === "string" && p.name.length > 0 && p.name.length <= MAX_NAME && typeof p.savedAt === "string" && draft) {
        out.push({ id: p.id, name: p.name, savedAt: p.savedAt, draft });
      }
    }
    return out.slice(0, MAX_PRESETS);
  } catch {
    return [];
  }
}

/** True when the list was written; false when storage is unavailable or full (the presets then live for this page only). */
export function savePresets(presets: readonly Preset[], storage: WritableStorage | undefined = safeStorage()): boolean {
  try {
    if (!storage) return false;
    storage.setItem(PRESETS_KEY, JSON.stringify({ v: 1, presets }));
    return true;
  } catch {
    return false;
  }
}

function safeStorage(): Storage | undefined {
  try {
    return typeof localStorage === "undefined" ? undefined : localStorage;
  } catch {
    return undefined;
  }
}

export const presetId = (name: string, now: number): string => `${now.toString(36)}-${name.length.toString(36)}-${Math.floor(Math.random() * 1e6).toString(36)}`;

/** Example parameters the lab's own tests use for a fixture. Demonstrations, not recommendations. */
export function examplesFor(dataset: BuiltinDataset | undefined, specs: readonly StrategySpec[]): ExamplePreset[] {
  if (!dataset) return [];
  return specs.flatMap((spec) => {
    const values = dataset.presets[spec.kind];
    if (!spec.available || !values) return [];
    return [{ id: `example:${dataset.key}:${spec.kind}`, name: `${spec.title} on ${dataset.filename}`, draft: { kind: spec.kind, values: { ...defaultValues(spec), ...values } } }];
  });
}
