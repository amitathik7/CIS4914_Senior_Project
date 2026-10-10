import { describe, expect, it } from "vitest";
import { parseCatalog } from "./catalog";
import { examplesFor, loadPresets, PRESETS_KEY, savePresets, type Preset } from "./presets";
import { builtin, catalogText, MemoryStorage } from "./testClient";

const good: Preset = { id: "a", name: "fast", savedAt: "2026-10-09T12:00:00Z", draft: { kind: "sma_crossover", values: { short_window: "2" } } };

describe("preset storage", () => {
  it("round trips through localStorage under the console's te. prefix", () => {
    const storage = new MemoryStorage();
    expect(PRESETS_KEY.startsWith("te.")).toBe(true);
    expect(savePresets([good], storage)).toBe(true);
    expect(loadPresets(storage)).toEqual([good]);
  });

  it("drops whatever it cannot trust and never throws", () => {
    const storage = new MemoryStorage();
    storage.setItem(PRESETS_KEY, JSON.stringify({ v: 1, presets: [good, { id: 1 }, { ...good, name: "" }, { ...good, name: "x".repeat(61) }, { ...good, draft: { kind: "k", values: { a: 5 } } }, { ...good, id: "b", draft: { kind: "k", values: [] } }, null, "x"] }));
    expect(loadPresets(storage).map((p) => p.id)).toEqual(["a"]);
    for (const junk of ["not json", "null", '{"v":2,"presets":[]}', '{"v":1}', "[]"]) {
      storage.setItem(PRESETS_KEY, junk);
      expect(loadPresets(storage), junk).toEqual([]);
    }
    storage.fail = true;
    expect(loadPresets(storage)).toEqual([]);
    expect(loadPresets(undefined)).toEqual([]);
  });

  it("reports a failed save (storage disabled or full) instead of pretending", () => {
    const storage = new MemoryStorage();
    storage.fail = true;
    expect(savePresets([good], storage)).toBe(false);
    expect(savePresets([good], undefined)).toBe(false);
  });
});

describe("example presets", () => {
  const catalog = parseCatalog(catalogText);

  it("come from the dataset's own example parameters, are labelled by dataset, and skip an unavailable strategy", () => {
    const examples = examplesFor(builtin("sma_crossover"), catalog.strategies);
    expect(examples.map((e) => e.name)).toEqual(["Moving-average crossover on sma_crossover.csv", "Mean reversion (rolling z-score) on sma_crossover.csv"]);
    expect(examples[0]!.draft.values).toMatchObject({ short_window: "2", long_window: "3", symbols: "AAPL", requested_quantity: "1" });
    expect(examples[1]!.draft.values).toMatchObject({ lookback: "4", entry_threshold: "1.5", rearm_threshold: "0.5" }); // rearm: the catalog default
    expect(examplesFor(undefined, catalog.strategies)).toEqual([]);
  });
});
