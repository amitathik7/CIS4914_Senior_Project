import { describe, expect, it } from "vitest";
import { parseCatalog, specOf } from "./catalog";
import {
  buildRequest, defaultValues, describeChanges, newDraft, normalizeSymbols, requestKey, sameDraft, summarize, symbolEntries, validateDraft, withValue,
  type RequestSnapshot,
} from "./draft";
import { catalogText } from "./testClient";

const catalog = parseCatalog(catalogText);
const sma = specOf(catalog, "sma_crossover")!;
const mr = specOf(catalog, "mean_reversion")!;
const issuesOf = (draft = newDraft(sma)) => Object.fromEntries(validateDraft(sma, draft).map((i) => [i.name, i.message]));

describe("defaults come from the catalog", () => {
  it("fills every parameter the tool declares, and leaves the required allowlist empty", () => {
    expect(defaultValues(sma)).toEqual({ strategy_id: "sma_crossover", short_window: "5", long_window: "20", requested_quantity: "1", symbols: "" });
    expect(defaultValues(mr)).toMatchObject({ lookback: "20", entry_threshold: "2", rearm_threshold: "0.5" });
  });
});

describe("client checks (only what the catalog declares)", () => {
  const ok = withValue(newDraft(sma), "symbols", "AAPL");

  it("accepts the defaults plus a symbol", () => expect(validateDraft(sma, ok)).toEqual([]));

  it("requires the allowlist", () => expect(issuesOf().symbols).toBe("Enter at least one symbol."));

  it("a whole-number parameter takes digits only", () => {
    for (const bad of ["-1", "1.5", "1e3", "abc", "+2", "0x10"]) {
      expect(issuesOf(withValue(ok, "short_window", bad)).short_window, bad).toMatch(/digits only/);
    }
    for (const fine of ["1", " 12 ", "007", "9223372036854775807"]) expect(issuesOf(withValue(ok, "short_window", fine)).short_window, fine).toBeUndefined();
  });

  it("a real-number parameter must be a finite number", () => {
    const d = withValue(newDraft(mr), "symbols", "AAPL");
    for (const bad of ["NaN", "inf", "Infinity", "1,5", "--2", ""]) {
      expect(validateDraft(mr, withValue(d, "entry_threshold", bad)).some((i) => i.name === "entry_threshold"), bad).toBe(true);
    }
    for (const fine of ["2", "0.5", ".5", "-1", "1e2", "2."]) {
      expect(validateDraft(mr, withValue(d, "entry_threshold", fine)).some((i) => i.name === "entry_threshold"), fine).toBe(false);
    }
  });

  it("an empty value offers to restore the default", () => {
    expect(issuesOf(withValue(ok, "long_window", "")).long_window).toMatch(/restore the default \(20\)/);
  });

  it("allowlist: no empty entry, no repeats, case matters", () => {
    expect(issuesOf(withValue(ok, "symbols", "AAPL,,MSFT")).symbols).toMatch(/empty entry/);
    expect(issuesOf(withValue(ok, "symbols", "AAPL, AAPL")).symbols).toBe("AAPL is listed twice.");
    expect(issuesOf(withValue(ok, "symbols", "AAPL,aapl")).symbols).toBeUndefined();
  });

  it("does NOT reimplement cross-field rules: short >= long is left to the engine check", () => {
    const d = withValue(withValue(ok, "short_window", "30"), "long_window", "3");
    expect(validateDraft(sma, d)).toEqual([]);
  });
});

describe("the request is the text that was typed", () => {
  it("keeps every parameter as text, in catalog order, and never routes it through a number", () => {
    let d = withValue(newDraft(sma), "symbols", " AAPL ,\tMSFT ");
    d = withValue(d, "requested_quantity", "9223372036854775807");
    d = withValue(d, "short_window", " 2 ");
    expect(buildRequest(sma, d)).toEqual({
      kind: "sma_crossover",
      params: [["strategy_id", "sma_crossover"], ["short_window", "2"], ["long_window", "20"], ["requested_quantity", "9223372036854775807"], ["symbols", "AAPL,MSFT"]],
    });
    expect(Number("9223372036854775807").toString()).not.toBe("9223372036854775807"); // what a Number would have done to it
  });

  it("a threshold stays the text typed (0.1 is not 0.10000000000000001)", () => {
    const d = withValue(withValue(newDraft(mr), "symbols", "A"), "rearm_threshold", "0.1");
    expect(buildRequest(mr, d).params.find(([n]) => n === "rearm_threshold")![1]).toBe("0.1");
  });

  it("normalizes only the spaces and tabs around allowlist entries", () => {
    expect(normalizeSymbols(" a , B\t,C d")).toBe("a,B,C d");
    expect(symbolEntries("")).toEqual([]);
    expect(symbolEntries("A,,B")).toEqual(["A", "", "B"]);
  });
});

describe("what makes two requests the same", () => {
  const base: RequestSnapshot = { kind: "sma_crossover", params: [["short_window", "2"], ["long_window", "3"]], dataset: { kind: "builtin", name: "a.csv", sha256: "11" } };

  it("is the strategy, its parameter text and the dataset bytes (not the dataset name)", () => {
    expect(requestKey(base)).toBe(requestKey({ ...base, dataset: { ...base.dataset, name: "renamed.csv" } }));
    expect(requestKey(base)).not.toBe(requestKey({ ...base, dataset: { ...base.dataset, sha256: "22" } }));
    expect(requestKey(base)).not.toBe(requestKey({ ...base, params: [["short_window", "2"], ["long_window", "4"]] }));
    expect(requestKey(base)).not.toBe(requestKey({ ...base, kind: "mean_reversion" }));
  });

  it("describes what changed in words", () => {
    const after: RequestSnapshot = { ...base, params: [["short_window", "2"], ["long_window", "4"]], dataset: { kind: "upload", name: "b.csv", sha256: "22" } };
    expect(describeChanges(base, after, (k) => specOf(catalog, k))).toEqual(["Dataset: a.csv → b.csv", "Long window: 3 → 4"]);
    expect(describeChanges(base, { ...base, kind: "mean_reversion" }, (k) => specOf(catalog, k))).toEqual(["Strategy: Moving-average crossover → Mean reversion (rolling z-score)"]);
  });

  it("drafts compare by kind and every value", () => {
    const d = newDraft(sma);
    expect(sameDraft(d, { ...d })).toBe(true);
    expect(sameDraft(d, withValue(d, "short_window", "6"))).toBe(false);
    expect(sameDraft(d, newDraft(mr))).toBe(false);
  });

  it("summarizes a request without the id or quantity", () => {
    expect(summarize(sma, [["strategy_id", "x"], ["short_window", "2"], ["requested_quantity", "1"], ["symbols", "AAPL"]])).toBe("short window 2, symbols AAPL");
  });
});
