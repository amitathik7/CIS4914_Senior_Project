import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LabError, type CheckResult, type ReplayResponse } from "../api/lab";
import { activeCheck, currentRequest, datasetIdentity, draftIssues, LabStore, resultStatus, runBlocker } from "./store";
import { builtin, deferred, FakeClient, MemoryStorage, OTHER_SHA, readyStatus, replayResponse, SHA, SPECS } from "./testClient";
import { PRESETS_KEY } from "./presets";

let client: FakeClient;
let storage: MemoryStorage;
let store: LabStore;

const connected = async (options: { debounce?: number } = {}) => {
  client = new FakeClient();
  storage = new MemoryStorage();
  store = new LabStore(client, { storage, now: () => "2026-10-09T12:00:00.000Z", checkDebounceMs: options.debounce ?? 0 });
  await store.connect();
  await vi.runAllTimersAsync();
};
const get = () => store.getState();
const flush = () => vi.runAllTimersAsync();

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe("connecting", () => {
  it("loads the engine's catalog and datasets and starts from the example for the default dataset", async () => {
    await connected();
    expect(get().service.phase).toBe("online");
    expect(get().catalog!.strategies.map((s) => s.kind)).toEqual(["sma_crossover", "mean_reversion", "ml"]);
    expect(get().draft.kind).toBe("sma_crossover");
    expect(get().draft.values).toMatchObject({ short_window: "2", long_window: "3", symbols: "AAPL" });
    expect(get().origin).toEqual({ label: "Example: Moving-average crossover on sma_crossover.csv", edited: false });
    expect(get().launches).toBe(0);
    expect(client.replays).toHaveLength(0); // connecting never replays
  });

  it("reports an unreachable service as offline, and Retry recovers without losing the draft", async () => {
    client = new FakeClient();
    client.statusResult = new LabError("service_unavailable", "not reachable");
    store = new LabStore(client, { storage: new MemoryStorage(), checkDebounceMs: 0 });
    await store.connect();
    expect(get().service.phase).toBe("offline");
    expect(runBlocker(get())).toMatch(/not running/);
    client.statusResult = readyStatus();
    await store.connect();
    await flush();
    expect(get().service.phase).toBe("online");
  });

  it("explains a service that is up but has no built engine", async () => {
    client = new FakeClient();
    client.statusResult = { gateway: { version: "0.1.0", api: "1" }, runner: { state: "missing", message: "not built", detail: "", setup: "cmake ..." } };
    store = new LabStore(client, { storage: new MemoryStorage() });
    await store.connect();
    expect(get().service.phase).toBe("no_runner");
    expect(runBlocker(get())).toMatch(/has not been built/);
  });
});

describe("running a replay", () => {
  it("only runReplay launches the engine; stepping, filtering, tabs and editing never do", async () => {
    await connected();
    expect(await store.runReplay()).toBe(true);
    expect(get().launches).toBe(1);
    expect(client.replays).toHaveLength(1);
    expect(client.replays[0]).toEqual({
      strategy: { kind: "sma_crossover", params: [["strategy_id", "sma_crossover"], ["short_window", "2"], ["long_window", "3"], ["requested_quantity", "1"], ["symbols", "AAPL"]] },
      dataset: { builtin: "sma_crossover" },
    });
    store.step("next"); store.step("next"); store.step("next_signal"); store.step("previous"); store.step("reset");
    store.setCursor(5); store.setSymbol("AAPL"); store.setSymbol(null); store.setTab("diagnostics"); store.setTab("details");
    store.setValue("short_window", "1"); store.selectBuiltin("mr_rearm"); store.selectKind("mean_reversion"); store.restoreDefaults();
    await flush();
    expect(client.replays).toHaveLength(1);
    expect(get().launches).toBe(1);
  });

  it("stores an immutable snapshot with the request, dataset identity and the tool's exact text", async () => {
    await connected();
    await store.runReplay();
    const snap = get().snapshot!;
    expect(snap.request.dataset).toEqual({ kind: "builtin", name: "sma_crossover.csv", sha256: SHA });
    expect(snap.raw).toBe(replayResponse(client.replays[0]!).raw);
    expect(snap.model.signals.map((s) => [s.eventIndex, s.side])).toEqual([[4, "buy"], [7, "sell"]]);
    expect(Object.isFrozen(snap)).toBe(true);
    expect(Object.isFrozen(snap.request)).toBe(true);
    expect(Object.isFrozen(snap.doc.events)).toBe(true);
    expect(() => { (snap as { id: number }).id = 9; }).toThrow();
    expect(get().cursor).toBe(0); // a new result starts at position 0
  });

  it("marks the result stale when any input changes, and never relabels it", async () => {
    await connected();
    await store.runReplay();
    expect(resultStatus(get())).toMatchObject({ relation: "current", phase: "idle", reasons: [] });
    const before = get().snapshot!;
    store.setValue("long_window", "4");
    expect(resultStatus(get())).toMatchObject({ relation: "stale", reasons: ["Long window: 3 → 4"] });
    expect(get().snapshot).toBe(before); // untouched: same object, same request
    expect(get().snapshot!.request.params.find(([n]) => n === "long_window")![1]).toBe("3");
    store.setValue("long_window", "3");
    expect(resultStatus(get()).relation).toBe("current"); // back to the inputs that produced it
    store.selectBuiltin("mr_rearm");
    expect(resultStatus(get())).toMatchObject({ relation: "stale", reasons: ["Dataset: sma_crossover.csv → mr_rearm.csv"] });
  });

  it("a rebuilt engine makes the result stale", async () => {
    await connected();
    await store.runReplay();
    client.statusResult = readyStatus("d4".repeat(32));
    await store.pollStatus();
    expect(resultStatus(get())).toMatchObject({ relation: "stale", reasons: ["The replay engine was rebuilt since this result"] });
  });

  it("refuses a second submission while one is running (no duplicate launch)", async () => {
    await connected();
    const gate = deferred<ReplayResponse>();
    client.replayImpl = () => gate.promise;
    const first = store.runReplay();
    expect(get().run.phase).toBe("running");
    expect(runBlocker(get())).toMatch(/already running/);
    expect(await store.runReplay()).toBe(false);
    expect(client.replays).toHaveLength(1);
    gate.resolve(replayResponse(client.replays[0]!));
    expect(await first).toBe(true);
    expect(get().launches).toBe(1);
  });

  it("a cancelled run's late answer never replaces anything", async () => {
    await connected();
    const gate = deferred<ReplayResponse>();
    client.replayImpl = () => gate.promise;
    const first = store.runReplay();
    store.cancelRun();
    expect(get().run).toMatchObject({ phase: "idle", cancelled: true });
    gate.resolve(replayResponse(client.replays[0]!));
    expect(await first).toBe(false);
    expect(get().snapshot).toBeUndefined();
  });

  it("an older launch answering after a newer one is dropped, whichever way it ends", async () => {
    await connected();
    const slow = deferred<ReplayResponse>();
    const fast = deferred<ReplayResponse>();
    const gates = [slow, fast];
    client.replayImpl = () => gates.shift()!.promise;
    const a = store.runReplay();
    store.cancelRun();
    store.setValue("long_window", "4");
    const b = store.runReplay();
    fast.resolve(replayResponse(client.replays[1]!));
    expect(await b).toBe(true);
    const newer = get().snapshot!;
    expect(newer.request.params.find(([n]) => n === "long_window")![1]).toBe("4");
    slow.resolve(replayResponse(client.replays[0]!, SHA, SPECS.slice(0, 3)));
    expect(await a).toBe(false);
    expect(get().snapshot).toBe(newer);
    expect(get().run.phase).toBe("idle");
  });

  it("a stale failure cannot overwrite a newer success either", async () => {
    await connected();
    const slow = deferred<ReplayResponse>();
    client.replayImpl = () => slow.promise;
    const a = store.runReplay();
    store.cancelRun();
    client.replayImpl = async (r) => replayResponse(r);
    await store.runReplay();
    const newer = get().snapshot;
    slow.reject(new LabError("timeout", "too slow"));
    await a;
    expect(get().snapshot).toBe(newer);
    expect(get().run.phase).toBe("idle");
  });

  it("a failed run keeps the last good result and shows the engine's own message", async () => {
    await connected();
    await store.runReplay();
    const good = get().snapshot;
    client.replayImpl = async () => {
      throw new LabError("dataset_invalid", "dataset 'x.csv' is not valid", { problems: [{ message: "price 'abc' is not a number", line: 3, column: "price", where: null }] });
    };
    expect(await store.runReplay()).toBe(false);
    expect(get().run).toMatchObject({ phase: "failed" });
    expect(get().run.error!.problems[0]).toMatchObject({ line: 3, column: "price" });
    expect(get().snapshot).toBe(good);
    expect(resultStatus(get())).toMatchObject({ phase: "failed", relation: "current" });
    store.dismissRunError();
    expect(get().run.phase).toBe("idle");
  });

  it("an unreachable service during a run flips the service to offline", async () => {
    await connected();
    client.replayImpl = async () => { throw new LabError("service_unavailable", "gone"); };
    await store.runReplay();
    expect(get().service.phase).toBe("offline");
    expect(get().run.error!.unreachable).toBe(true);
  });

  it("refuses a result that does not describe the request (a different dataset)", async () => {
    await connected();
    client.replayImpl = async (r) => replayResponse(r, OTHER_SHA);
    expect(await store.runReplay()).toBe(false);
    expect(get().run.error!.kind).toBe("result_mismatch");
    expect(get().snapshot).toBeUndefined();
  });

  it("refuses a malformed document and says where", async () => {
    await connected();
    client.replayImpl = async (r) => ({ ...replayResponse(r), raw: replayResponse(r).raw.replace('"index":3', '"index":9') });
    await store.runReplay();
    expect(get().run.error!.kind).toBe("invalid_structure");
    expect(get().run.error!.message).toMatch(/events\[3\]\.index/);
  });
});

describe("the engine check", () => {
  it("asks the real constructors and shows only the answer for the draft on screen", async () => {
    await connected({ debounce: 10 });
    expect(activeCheck(get())).toMatchObject({ phase: "ok" });
    const slow = deferred<CheckResult>();
    client.checkImpl = () => slow.promise;
    store.setValue("long_window", "4");
    await vi.advanceTimersByTimeAsync(20);
    store.setValue("long_window", "5");
    client.checkImpl = async (r) => ({ ok: true, kind: r.kind, strategy_id: "s", window_size: 5, derived: { later: true }, parameters: {}, warnings: [] });
    await vi.advanceTimersByTimeAsync(20);
    slow.resolve({ ok: true, kind: "sma_crossover", strategy_id: "s", window_size: 4, derived: { stale: true }, parameters: {}, warnings: [] });
    await flush();
    const check = activeCheck(get());
    expect(check.phase).toBe("ok");
    expect(check.phase === "ok" && check.result.derived).toEqual({ later: true });
  });

  it("debounces typing into one check", async () => {
    await connected({ debounce: 200 });
    const before = client.checks.length;
    for (const v of ["1", "12", "123"]) store.setValue("long_window", v);
    await vi.advanceTimersByTimeAsync(150);
    expect(client.checks.length).toBe(before);
    await vi.advanceTimersByTimeAsync(100);
    expect(client.checks.length).toBe(before + 1);
  });

  it("does not ask the engine about a draft that already fails the client checks", async () => {
    await connected();
    const before = client.checks.length;
    store.setValue("short_window", "abc");
    await flush();
    expect(client.checks.length).toBe(before);
    expect(draftIssues(get()).map((i) => i.name)).toEqual(["short_window"]);
    expect(runBlocker(get())).toMatch(/Fix the highlighted/);
    expect(await store.runReplay()).toBe(false);
  });

  it("surfaces the engine's rejection verbatim, blocks Run, and unblocks when fixed", async () => {
    await connected();
    client.checkImpl = async () => { throw new LabError("runner_rejected", "configuration error: long_window must be greater than short_window (got long_window 3, short_window 5)", { fields: ["long_window", "short_window"] }); };
    store.setValue("short_window", "5");
    await flush();
    const check = activeCheck(get());
    expect(check.phase === "rejected" && check.error.message).toMatch(/long_window must be greater than short_window/);
    expect(runBlocker(get())).toMatch(/engine rejected/);
    expect(await store.runReplay()).toBe(false);
    client.checkImpl = async (r) => ({ ok: true, kind: r.kind, strategy_id: "s", window_size: 3, derived: {}, parameters: {}, warnings: [] });
    store.setValue("short_window", "2");
    await flush();
    expect(runBlocker(get())).toBeUndefined();
  });

  it("says when the check could not be made because the service is unreachable", async () => {
    await connected();
    client.checkImpl = async () => { throw new LabError("service_unavailable", "gone"); };
    store.setValue("short_window", "3");
    await flush();
    expect(activeCheck(get()).phase).toBe("unavailable");
  });
});

describe("choosing the strategy and the dataset", () => {
  it("switching strategy loads that strategy's defaults but keeps the allowlist and quantity", async () => {
    await connected();
    store.setValue("requested_quantity", "50");
    store.selectKind("mean_reversion");
    expect(get().draft).toEqual({ kind: "mean_reversion", values: { strategy_id: "mean_reversion", lookback: "20", entry_threshold: "2", rearm_threshold: "0.5", requested_quantity: "50", symbols: "AAPL" } });
    expect(get().origin).toBeUndefined();
  });

  it("Custom ML is unavailable and cannot be selected", async () => {
    await connected();
    store.selectKind("ml");
    expect(get().draft.kind).toBe("sma_crossover");
  });

  it("Restore defaults resets every parameter except the allowlist", async () => {
    await connected();
    store.setValue("short_window", "9");
    store.setValue("symbols", "AAPL,MSFT");
    store.restoreDefaults();
    expect(get().draft.values).toMatchObject({ short_window: "5", long_window: "20", symbols: "AAPL,MSFT" });
  });

  it("an upload is fingerprinted in the browser, identified by its bytes, and sent as base64", async () => {
    await connected();
    const bytes = new TextEncoder().encode("symbol,exchange_time,type,price\nAAPL,2026-01-05T14:30:00Z,bar,1\n");
    expect(await store.uploadBytes("mine.csv", bytes)).toBe(true);
    const up = get().upload!;
    expect(up).toMatchObject({ name: "mine.csv", size: bytes.length, symbols: ["AAPL"] });
    expect(up.sha256).toMatch(/^[0-9a-f]{64}$/);
    expect(datasetIdentity(get())).toEqual({ kind: "upload", name: "mine.csv", sha256: up.sha256 });
    client.replayImpl = async (r) => replayResponse(r, up.sha256);
    expect(await store.runReplay()).toBe(true);
    expect(client.replays[0]!.dataset).toEqual({ name: "mine.csv", csv_base64: up.base64 });
    expect(atob(up.base64)).toBe("symbol,exchange_time,type,price\nAAPL,2026-01-05T14:30:00Z,bar,1\n");
    // The same bytes under another name are the same inputs; different bytes are not.
    const request = currentRequest(get())!;
    expect(request.dataset.sha256).toBe(up.sha256);
  });

  it("refuses an empty file, a binary file and an oversized file before anything is sent", async () => {
    await connected();
    expect(await store.uploadBytes("e.csv", new Uint8Array(0))).toBe(false);
    expect(get().uploadError).toMatch(/is empty/);
    expect(await store.uploadBytes("b.csv", new Uint8Array([97, 44, 0, 1]))).toBe(false);
    expect(get().uploadError).toMatch(/NUL bytes/);
    const big = { name: "big.csv", size: 9 * 1024 * 1024, arrayBuffer: async () => { throw new Error("must not be read"); } } as unknown as File;
    expect(await store.uploadFile(big)).toBe(false);
    expect(get().uploadError).toMatch(/limit is 8 MiB/);
    expect(get().dataset).toEqual({ kind: "builtin", key: "sma_crossover" }); // the previous choice stands
    expect(client.replays).toHaveLength(0);
  });

  it("removing the upload returns to a built-in dataset", async () => {
    await connected();
    await store.uploadBytes("mine.csv", new TextEncoder().encode("a,b\n1,2\n"));
    store.removeUpload();
    expect(get().upload).toBeUndefined();
    expect(get().dataset.kind).toBe("builtin");
  });

  it("a built-in dataset the service cannot read is not runnable", async () => {
    client = new FakeClient();
    client.datasetsList = [builtin("sma_crossover", { available: false, sha256: undefined, message: "missing" })];
    store = new LabStore(client, { storage: new MemoryStorage() });
    await store.connect();
    expect(runBlocker(get())).toMatch(/Choose a dataset/);
  });
});

describe("the replay position", () => {
  it("steps within the recorded order and honours the display filter without moving the cursor", async () => {
    await connected();
    await store.runReplay();
    store.step("next_signal");
    expect(get().cursor).toBe(5);
    store.step("next_signal");
    expect(get().cursor).toBe(8);
    store.step("next_signal");
    expect(get().cursor).toBe(8); // no further signal: boundary
    store.setSymbol("AAPL");
    expect(get().cursor).toBe(8); // a filter hides rows; it never moves the cursor
    store.step("previous");
    expect(get().cursor).toBe(7);
    store.step("reset");
    expect(get().cursor).toBe(0);
    store.step("previous");
    expect(get().cursor).toBe(0);
    store.setCursor(99);
    expect(get().cursor).toBe(9);
    store.step("next");
    expect(get().cursor).toBe(9);
  });

  it("a new result starts at the beginning with no filter", async () => {
    await connected();
    await store.runReplay();
    store.setCursor(6);
    store.setSymbol("AAPL");
    await store.runReplay();
    expect(get()).toMatchObject({ cursor: 0, symbol: null });
    expect(get().snapshot!.id).toBe(2);
  });
});

describe("presets", () => {
  it("are saved by name in this browser, loaded explicitly, replaced by name and deleted", async () => {
    await connected();
    store.setValue("short_window", "3");
    expect(store.savePreset("  fast cross ")).toEqual({ ok: true, persisted: true });
    expect(JSON.parse(storage.getItem(PRESETS_KEY)!)).toMatchObject({ v: 1, presets: [{ name: "fast cross", savedAt: "2026-10-09T12:00:00.000Z" }] });
    store.setValue("short_window", "4");
    store.savePreset("fast cross");
    expect(get().presets).toHaveLength(1); // same name replaces
    expect(get().presets[0]!.draft.values.short_window).toBe("4");
    store.restoreDefaults();
    expect(store.loadPreset(get().presets[0]!.id)).toBe(true);
    expect(get().draft.values.short_window).toBe("4");
    expect(get().origin).toEqual({ label: "Preset: fast cross", edited: false });
    store.deletePreset(get().presets[0]!.id);
    expect(get().presets).toEqual([]);
    expect(JSON.parse(storage.getItem(PRESETS_KEY)!).presets).toEqual([]);
  });

  it("survive a reload of the page", async () => {
    await connected();
    store.savePreset("keep me");
    const again = new LabStore(client, { storage });
    expect(again.getState().presets.map((p) => p.name)).toEqual(["keep me"]);
  });

  it("say so when the browser cannot persist them, and still work for the page session", async () => {
    await connected();
    storage.fail = true;
    expect(store.savePreset("temp")).toEqual({ ok: true, persisted: false });
    expect(get().presetsPersisted).toBe(false);
    expect(get().presets.map((p) => p.name)).toEqual(["temp"]);
  });

  it("reject an empty or over-long name", async () => {
    await connected();
    expect(store.savePreset("   ").ok).toBe(false);
    expect(store.savePreset("x".repeat(61)).ok).toBe(false);
  });

  it("a preset for a strategy the engine does not offer is not loaded", async () => {
    await connected();
    expect(store.loadDraft({ kind: "ml", values: {} }, "x")).toBe(false);
    expect(store.loadDraft({ kind: "nope", values: {} }, "x")).toBe(false);
  });
});
