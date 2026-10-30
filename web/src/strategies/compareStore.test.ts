import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LabError } from "../api/lab";
import { compareBlocker, memberCheck, memberIssues, memberRequest } from "./compareState";
import { CompareStore } from "./compareStore";
import { lengthenWindows, planQuickStart } from "./compareStarts";
import { LabStore } from "./store";
import { connectCompare, nineBars, windowOf } from "./testCompare";
import { builtin, catalogText, FakeClient, replayResponse } from "./testClient";
import { parseCatalog } from "./catalog";

let client: FakeClient;
let lab: LabStore;
let store: CompareStore;

const get = () => store.getState();
const flush = () => vi.runAllTimersAsync();

const connected = async () => {
  ({ client, lab, store } = await connectCompare());
};

/** B becomes the crossover with windows 3 and 5, so the two sides are different parameter sets of one strategy. */
const differentB = () => {
  store.setValue("B", "short_window", "3");
  store.setValue("B", "long_window", "5");
};

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe("starting Compare", () => {
  it("starts from the Configure draft: A is a copy of it, B a copy of A, and nothing runs", async () => {
    await connected();
    expect(get().initialized).toBe(true);
    expect(get().members.A.draft.values).toMatchObject({ short_window: "2", long_window: "3", symbols: "AAPL" });
    expect(get().members.B.draft).toEqual(get().members.A.draft);
    expect(get().members.B.draft).not.toBe(get().members.A.draft); // independent objects
    expect(get().members.B.origin).toEqual({ label: "Copied from A", edited: false });
    expect(get().dataset).toEqual({ kind: "builtin", key: "sma_crossover" });
    expect(get().launches).toBe(0);
    expect(client.replays).toHaveLength(0);
  });

  it("does not follow later edits to the Configure draft (the two views are independent)", async () => {
    await connected();
    lab.setValue("short_window", "9");
    expect(get().members.A.draft.values.short_window).toBe("2");
    expect(lab.getState().draft.values.short_window).toBe("9");
  });

  it("checks each side with the real strategy constructors, separately", async () => {
    await connected();
    client.checkImpl = async (request) => {
      if (request.params.some(([n, t]) => n === "short_window" && t === "9")) throw new LabError("runner_rejected", "short_window must be below long_window", { fields: ["short_window"] });
      return { ok: true, kind: request.kind, strategy_id: request.kind, window_size: 3, derived: {}, parameters: {}, warnings: [] };
    };
    store.setValue("B", "short_window", "9");
    await flush();
    expect(memberCheck(get(), "A").phase).toBe("ok");
    expect(memberCheck(get(), "B").phase).toBe("rejected");
    expect(compareBlocker(get())).toMatch(/rejected configuration B: short_window must be below long_window/);
    expect(await store.runComparison()).toBe(false);
    expect(client.replays).toHaveLength(0);
  });
});

describe("two independent configurations over one shared dataset", () => {
  it("edits one side without touching the other, and shares only the dataset and the symbol allowlist", async () => {
    await connected();
    const aBefore = get().members.A.draft;
    differentB();
    expect(get().members.A.draft).toBe(aBefore);
    expect(get().members.B.draft.values).toMatchObject({ short_window: "3", long_window: "5" });
    expect(get().members.B.origin).toEqual({ label: "Copied from A", edited: true });
    store.setSymbols("AAPL,MSFT");
    expect(get().members.A.draft.values.symbols).toBe("AAPL,MSFT");
    expect(get().members.B.draft.values.symbols).toBe("AAPL,MSFT");
    store.setValue("A", "symbols", "MSFT"); // routed to the shared allowlist
    expect(get().members.B.draft.values.symbols).toBe("MSFT");
  });

  it("changes strategy kind per side, carrying only the shared allowlist and that side's own quantity", async () => {
    await connected();
    store.setValue("A", "requested_quantity", "7");
    store.selectKind("B", "mean_reversion");
    expect(get().members.B.draft.kind).toBe("mean_reversion");
    expect(get().members.B.draft.values).toMatchObject({ symbols: "AAPL", lookback: "20", requested_quantity: "1" });
    expect(get().members.A.draft.kind).toBe("sma_crossover");
    expect(get().members.A.draft.values.requested_quantity).toBe("7");
    store.selectKind("B", "ml"); // unavailable: ignored
    expect(get().members.B.draft.kind).toBe("mean_reversion");
  });

  it("Copy A to B copies the configuration, not the connection: later edits stay on one side", async () => {
    await connected();
    store.setValue("A", "short_window", "4");
    store.setValue("A", "long_window", "8");
    store.selectKind("B", "mean_reversion");
    store.copyAToB();
    expect(get().members.B.draft).toEqual(get().members.A.draft);
    expect(get().members.B.origin).toEqual({ label: "Copied from A", edited: false });
    store.setValue("B", "short_window", "5");
    expect(get().members.A.draft.values.short_window).toBe("4");
  });

  it("loads a preset into one side only, and keeps the shared allowlist", async () => {
    await connected();
    store.setSymbols("AAPL,MSFT");
    lab.selectBuiltin("mr_rearm");
    expect(store.savePreset("A", "mine")).toMatchObject({ ok: true });
    const id = lab.getState().presets[0]!.id;
    store.setValue("A", "short_window", "1");
    expect(store.loadPreset("B", id)).toBe(true);
    expect(get().members.B.draft.values).toMatchObject({ short_window: "2", symbols: "AAPL,MSFT" });
    expect(get().members.B.origin).toEqual({ label: "Preset: mine", edited: false });
    expect(get().members.A.draft.values.short_window).toBe("1");
    expect(store.loadPreset("A", "no-such-id")).toBe(false);
  });

  it("adopts a loaded allowlist when there is none yet, for both sides", async () => {
    await connected();
    store.setSymbols("");
    store.loadDraft("A", { kind: "sma_crossover", values: { symbols: "NVDA", short_window: "2", long_window: "3", requested_quantity: "1", strategy_id: "x" } }, "test");
    expect(get().members.A.draft.values.symbols).toBe("NVDA");
    expect(get().members.B.draft.values.symbols).toBe("NVDA");
    expect(get().members.A.origin).toEqual({ label: "test", edited: false });
  });
});

describe("quick starts", () => {
  it("fills crossover versus mean reversion from the dataset's examples, with one shared allowlist; nothing runs", async () => {
    await connected();
    expect(store.quickStart("crossover_vs_reversion")).toEqual({ ok: true });
    expect(get().members.A.draft).toMatchObject({ kind: "sma_crossover", values: { short_window: "2", long_window: "3", symbols: "AAPL" } });
    expect(get().members.B.draft).toMatchObject({ kind: "mean_reversion", values: { lookback: "4", entry_threshold: "1.5", symbols: "AAPL" } });
    expect(get().members.A.origin?.label).toBe("Quick start: crossover");
    expect(get().members.B.origin?.label).toBe("Quick start: mean reversion");
    expect(client.replays).toHaveLength(0);
  });

  it("fills two crossover variants: the same strategy with both windows lengthened", async () => {
    await connected();
    expect(store.quickStart("crossover_variants")).toEqual({ ok: true });
    expect(get().members.A.draft.values).toMatchObject({ short_window: "2", long_window: "3" });
    expect(get().members.B.draft).toMatchObject({ kind: "sma_crossover", values: { short_window: "3", long_window: "5" } });
    await flush();
    expect(memberIssues(get(), "B")).toEqual([]);
    expect(memberCheck(get(), "B").phase).toBe("ok");
  });

  it("keeps an allowlist already chosen, and lengthens windows exactly (beyond 2^53)", () => {
    const sma = parseCatalog(catalogText).strategies[0]!;
    const big = lengthenWindows({ kind: "sma_crossover", values: { short_window: "9007199254740993", long_window: "9223372036854775805" } })!;
    expect(big.values).toMatchObject({ short_window: "9007199254740994", long_window: "9223372036854775807" });
    expect(lengthenWindows({ kind: "sma_crossover", values: { short_window: "x", long_window: "3" } })).toBeUndefined();
    expect(lengthenWindows({ kind: "mean_reversion", values: { lookback: "3" } })).toBeUndefined();
    expect(sma.kind).toBe("sma_crossover");
  });

  it("says why a quick start cannot be offered", () => {
    const catalog = parseCatalog(catalogText.replace(/"kind":"mean_reversion"[^]*?"available":true/, (m) => m.replace('"available":true', '"available":false,"unavailable_reason":"x"')));
    expect(planQuickStart("crossover_vs_reversion", catalog, builtin("d"))).toEqual({ error: "The engine does not offer the mean reversion strategy." });
    expect(planQuickStart("crossover_variants", undefined, undefined)).toEqual({ error: "The engine does not offer the moving-average crossover strategy." });
  });
});

describe("blockers", () => {
  it("explains why it cannot run: invalid parameters, no service, no runner", async () => {
    await connected();
    store.setValue("A", "short_window", "two");
    expect(memberIssues(get(), "A")).toEqual([{ name: "short_window", message: "Enter a whole number: digits only, no sign or decimals." }]);
    expect(compareBlocker(get())).toBe("Fix the highlighted settings in A first.");
    store.setValue("A", "short_window", "2");
    store.setValue("B", "long_window", "");
    expect(compareBlocker(get())).toBe("Fix the highlighted settings in B first.");
    store.setValue("B", "long_window", "3");
    expect(compareBlocker(get())).toBeUndefined();

    client.statusResult = { gateway: { version: "0.1.0", api: "1" }, runner: { state: "missing", message: "not built", detail: "", setup: "cmake" } };
    await lab.connect();
    expect(compareBlocker(get())).toMatch(/has not been built/);
    client.statusResult = new LabError("service_unavailable", "down");
    await lab.connect();
    expect(compareBlocker(get())).toMatch(/not running/);
    expect(await store.runComparison()).toBe(false);
    expect(client.replays).toHaveLength(0);
  });

  it("needs a dataset", async () => {
    await connected();
    store.selectBuiltin("nope");
    expect(compareBlocker(get())).toBe("Choose a dataset.");
  });

  it("builds each side's request from its own draft and the shared dataset identity", async () => {
    await connected();
    differentB();
    expect(memberRequest(get(), "A")!.dataset).toEqual(memberRequest(get(), "B")!.dataset);
    expect(memberRequest(get(), "A")!.params).not.toEqual(memberRequest(get(), "B")!.params);
  });
});

describe("an uploaded CSV is the shared dataset", () => {
  it("sends the same bytes for both sides", async () => {
    await connected();
    const bytes = new TextEncoder().encode("symbol,exchange_time,type,price\nAAPL,2026-01-05T14:30:00Z,bar,1\n");
    expect(await store.uploadBytes("mine.csv", bytes)).toBe(true);
    expect(get().dataset).toEqual({ kind: "upload" });
    client.replayImpl = async (request) => replayResponse(request, get().upload!.sha256, windowOf(request) === "2" ? nineBars().a : nineBars().b);
    expect(await store.runComparison()).toBe(true);
    const sent = client.replays.map((r) => r.dataset);
    expect(sent[0]).toEqual(sent[1]);
    expect(sent[0]).toMatchObject({ name: "mine.csv" });
    expect(get().comparison!.snapshots.A.request.dataset.kind).toBe("upload");
    store.removeUpload();
    expect(get().dataset).toEqual({ kind: "builtin", key: "sma_crossover" });
    expect(await store.uploadBytes("empty.csv", new Uint8Array())).toBe(false);
    expect(get().uploadError).toMatch(/is empty/);
  });
});

describe("the synchronized cursor", () => {
  it("moves one position for both sides, follows the display filter, and clamps", async () => {
    await connected();
    differentB();
    await store.runComparison();
    store.step("next"); store.step("next");
    expect(get().cursor).toBe(2);
    store.step("previous");
    expect(get().cursor).toBe(1);
    store.step("next_signal"); // A's first Buy is on event 5
    expect(get().cursor).toBe(5);
    store.step("next_signal"); // B's Buy on event 6
    expect(get().cursor).toBe(6);
    store.setCursor(99);
    expect(get().cursor).toBe(9);
    store.setCursor(-4);
    expect(get().cursor).toBe(0);
    store.setCursor(3);
    store.step("reset");
    expect(get().cursor).toBe(0);
  });

  it("jumps to the next difference in the chosen mode", async () => {
    await connected();
    differentB();
    await store.runComparison();
    store.step("next_difference");
    expect(get().cursor).toBe(5); // requests differ first at event 5
    store.step("next_difference");
    expect(get().cursor).toBe(6);
    store.setCursor(0);
    store.setDiffMode("decisions");
    store.step("next_difference");
    expect(get().cursor).toBe(3); // A is ready (evaluated) while B is still warming up
    store.setSymbol("MSFT");
    store.step("next_difference");
    expect(get().cursor).toBe(3); // nothing for MSFT: stays
  });

  it("starts at position 0 for every new comparison and does nothing without one", async () => {
    await connected();
    store.step("next"); store.setCursor(4);
    expect(get().cursor).toBe(0);
    await store.runComparison();
    store.setCursor(7);
    await store.runComparison();
    expect(get().cursor).toBe(0);
    expect(get().symbol).toBeNull();
  });
});
