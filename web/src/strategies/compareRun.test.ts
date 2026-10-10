import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LabError, type ReplayRequest, type ReplayResponse } from "../api/lab";
import { draftFromRequest } from "./backtestHandoff";
import { scopeText, provenanceJson } from "./compareExports";
import { compareBlocker, compareStatus } from "./compareState";
import type { CompareStore } from "./compareStore";
import type { LabStore } from "./store";
import { connectCompare, nineBars, sidesAnswer, windowOf } from "./testCompare";
import { deferred, OTHER_SHA, readyStatus, replayResponse, SHA, type FakeClient } from "./testClient";

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

describe("Run comparison", () => {
  it("launches exactly two independent replays over the same dataset, only when asked", async () => {
    await connected();
    differentB();
    await flush();
    expect(client.replays).toHaveLength(0);
    expect(await store.runComparison()).toBe(true);
    expect(client.replays).toHaveLength(2);
    expect(client.replays.map((r) => r.dataset)).toEqual([{ builtin: "sma_crossover" }, { builtin: "sma_crossover" }]);
    expect(client.replays.map(windowOf).sort()).toEqual(["2", "3"]);
    expect(get().launches).toBe(1);
    expect(get().run.phase).toBe("idle");
    expect(get().attempt).toMatchObject({ launch: 1, outcome: "complete" });
  });

  it("stores one immutable comparison with an identity for each configuration and for the pair", async () => {
    await connected();
    differentB();
    await store.runComparison();
    const done = get().comparison!;
    expect(done.id).toBe(1);
    expect(done.snapshots.A.request.params.find(([n]) => n === "short_window")![1]).toBe("2");
    expect(done.snapshots.B.request.params.find(([n]) => n === "short_window")![1]).toBe("3");
    expect(done.configIds.A).toMatch(/^[0-9a-f]{64}$/);
    expect(done.configIds.A).not.toBe(done.configIds.B);
    expect(done.comparisonId).toMatch(/^[0-9a-f]{64}$/);
    expect(done.datasetSha256).toBe(SHA);
    expect(done.model.rows).toHaveLength(9);
    expect(Object.isFrozen(done)).toBe(true);
    expect(get().cursor).toBe(0);
  });

  it("gives identical configurations identical config ids but still scopes every signal by side", async () => {
    await connected(); // A and B are copies of one another
    await store.runComparison();
    const done = get().comparison!;
    expect(done.configIds.A).toBe(done.configIds.B);
    expect(done.snapshots.A.doc.runId).toBe(done.snapshots.B.doc.runId); // the tool derives the id from the input
    const keys = done.model.signals.map((s) => s.key);
    expect(new Set(keys).size).toBe(keys.length);
    expect(keys.filter((k) => k.endsWith(":1")).sort()).toEqual(["A/0123456789abcdef:1", "B/0123456789abcdef:1"]);
  });

  it("stepping, filtering, tabs, difference mode, editing and downloads never start the engine", async () => {
    await connected();
    differentB();
    await store.runComparison();
    store.step("next"); store.step("previous"); store.step("next_signal"); store.step("next_difference"); store.step("reset");
    store.setCursor(5); store.setSymbol("AAPL"); store.setSymbol(null); store.setTab("events"); store.setTab("details"); store.setDiffMode("decisions");
    store.setValue("A", "short_window", "1"); store.setSymbols("AAPL"); store.selectBuiltin("mr_rearm"); store.copyAToB(); store.quickStart("crossover_variants");
    store.restoreDefaults("A"); store.selectKind("A", "mean_reversion");
    await flush();
    expect(client.replays).toHaveLength(2);
    expect(get().launches).toBe(1);
  });

  it("refuses a second submission while one is running (no duplicate launch)", async () => {
    await connected();
    const gates = [deferred<ReplayResponse>(), deferred<ReplayResponse>()];
    let n = 0;
    client.replayImpl = () => gates[n++]!.promise;
    const first = store.runComparison();
    expect(get().run.phase).toBe("running");
    expect(compareBlocker(get())).toBe("A comparison is already running.");
    expect(await store.runComparison()).toBe(false);
    expect(await store.runComparison()).toBe(false);
    expect(client.replays).toHaveLength(2); // one launch = two replays, however many clicks
    expect(get().launches).toBe(1);
    gates[0]!.resolve(sidesAnswer(client.replays[0]!));
    gates[1]!.resolve(sidesAnswer(client.replays[1]!));
    expect(await first).toBe(true);
  });

  it("shows each side's progress while the other is still running", async () => {
    await connected();
    differentB();
    const gate = deferred<ReplayResponse>();
    client.replayImpl = (request) => (windowOf(request) === "2" ? Promise.resolve(sidesAnswer(request)) : gate.promise);
    const pending = store.runComparison();
    await Promise.resolve();
    await Promise.resolve();
    expect(get().attempt!.members.A.phase).toBe("done");
    expect(get().attempt!.members.B.phase).toBe("running");
    expect(get().comparison).toBeUndefined(); // nothing is paired until both are in
    gate.resolve(sidesAnswer(client.replays[1]!));
    expect(await pending).toBe(true);
  });

  it("settings changed while a launch is in flight never reach its result: it keeps the settings and dataset it was sent, is stale against the editor, and exports what ran", async () => {
    await connected();
    differentB(); // A: windows 2 and 3 (the defaults of the fixture), B: windows 3 and 5
    const gates = [deferred<ReplayResponse>(), deferred<ReplayResponse>()];
    let n = 0;
    client.replayImpl = () => gates[n++]!.promise;
    const pending = store.runComparison();
    // The person keeps editing while both replays are out: both sides' parameters, and the dataset.
    store.setValue("A", "short_window", "1");
    store.setValue("B", "long_window", "9");
    store.selectBuiltin("mr_rearm");
    gates[0]!.resolve(sidesAnswer(client.replays[0]!));
    gates[1]!.resolve(sidesAnswer(client.replays[1]!));
    expect(await pending).toBe(true);

    const done = get().comparison!;
    const sent = (request: ReplayRequest) => Object.fromEntries(request.strategy.params);
    expect(done.snapshots.A.request.params).toEqual(client.replays[0]!.strategy.params);
    expect(done.snapshots.B.request.params).toEqual(client.replays[1]!.strategy.params);
    expect(sent(client.replays[0]!)).toMatchObject({ short_window: "2" });
    expect(sent(client.replays[1]!)).toMatchObject({ short_window: "3", long_window: "5" });
    expect(done.snapshots.A.request.dataset.name).toBe("sma_crossover.csv");
    expect(done.datasetSha256).toBe(SHA);
    // The editor now says something else, and the status says so rather than relabelling the result.
    expect(compareStatus(get())).toMatchObject({
      relation: "stale",
      reasons: { shared: ["Dataset: sma_crossover.csv → mr_rearm.csv"], A: ["Short window: 2 → 1"], B: ["Long window: 5 → 9"] },
    });
    // What is exported, and what the backtest handoff offers as "compared", is what ran.
    const json = JSON.parse(provenanceJson(done, scopeText(done, "full_run", done.model.total, null), { A: "A.json", B: "B.json" }));
    expect(json.members[0].parameters_as_passed).toEqual(done.snapshots.A.request.params.map(([name, value]) => ({ name, value })));
    expect(json.members[0].parameters_as_passed.find((p: { name: string }) => p.name === "short_window").value).toBe("2");
    expect(json.members[1].parameters_as_passed.find((p: { name: string }) => p.name === "long_window").value).toBe("5");
    expect(json.dataset.sha256).toBe(SHA);
    expect(json.dataset.name).toBe("sma_crossover.csv");
    expect(draftFromRequest(done.snapshots.A.request).values).toMatchObject({ short_window: "2" });
  });

  it("drops the answers of a cancelled launch, even when they arrive after a newer comparison", async () => {
    await connected();
    differentB();
    const old = [deferred<ReplayResponse>(), deferred<ReplayResponse>()];
    let n = 0;
    client.replayImpl = () => old[n++]!.promise;
    const first = store.runComparison();
    store.cancelRun();
    expect(get().run).toMatchObject({ phase: "idle", cancelled: true });
    expect(get().attempt).toBeUndefined();
    client.replayImpl = async (request) => sidesAnswer(request);
    expect(await store.runComparison()).toBe(true); // a newer comparison
    expect(get().comparison!.id).toBe(2);
    // The cancelled launch's answers finally arrive. They must not replace anything.
    old[0]!.resolve(sidesAnswer(client.replays[0]!));
    old[1]!.resolve(sidesAnswer(client.replays[1]!));
    expect(await first).toBe(false);
    await flush();
    expect(get().comparison!.id).toBe(2);
    expect(get().attempt).toMatchObject({ launch: 2, outcome: "complete" });
    expect(get().attempt!.members.A.snapshot).toBe(get().comparison!.snapshots.A); // the late answers did not touch the newer launch's record
    expect(get().attempt!.members.B.snapshot).toBe(get().comparison!.snapshots.B);
    expect(get().run.phase).toBe("idle");
  });

  it("a late answer of a cancelled launch does not disturb a newer launch that is still running", async () => {
    await connected();
    differentB();
    const old = [deferred<ReplayResponse>(), deferred<ReplayResponse>()];
    const fresh = [deferred<ReplayResponse>(), deferred<ReplayResponse>()];
    let n = 0;
    client.replayImpl = () => [...old, ...fresh][n++]!.promise;
    const first = store.runComparison();
    store.cancelRun();
    const second = store.runComparison();
    expect(get().run).toMatchObject({ phase: "running", token: 3 });
    old[0]!.reject(new LabError("timeout", "late"));
    old[1]!.resolve(sidesAnswer(client.replays[1]!));
    await first;
    await flush();
    // The newer launch is exactly as it was: running, nothing settled by the old answers.
    expect(get().run).toMatchObject({ phase: "running", token: 3 });
    expect(get().attempt).toMatchObject({ launch: 2, outcome: "running" });
    expect(get().attempt!.members.A.phase).toBe("running");
    expect(get().attempt!.members.B.phase).toBe("running");
    fresh[0]!.resolve(sidesAnswer(client.replays[2]!));
    fresh[1]!.resolve(sidesAnswer(client.replays[3]!));
    expect(await second).toBe(true);
    expect(get().comparison!.id).toBe(2);
  });

  it("a late failure of a cancelled launch does not mark the newer comparison failed", async () => {
    await connected();
    const late = deferred<ReplayResponse>();
    let n = 0;
    client.replayImpl = (request) => (n++ < 2 ? late.promise : Promise.resolve(sidesAnswer(request)));
    const first = store.runComparison();
    store.cancelRun();
    expect(await store.runComparison()).toBe(true);
    late.reject(new LabError("timeout", "too slow"));
    await first;
    await flush();
    expect(get().attempt).toMatchObject({ launch: 2, outcome: "complete" });
    expect(get().attempt!.members.A.error).toBeUndefined();
  });
});

describe("one side fails", () => {
  const refused = new LabError("runner_rejected", "long_window must be greater than short_window", { fields: ["long_window"] });

  it("reports a partial result honestly: no comparison, the finished side described, the failed side explained", async () => {
    await connected();
    differentB();
    client.replayImpl = async (request) => {
      if (windowOf(request) === "3") throw refused;
      return sidesAnswer(request);
    };
    expect(await store.runComparison()).toBe(false);
    const attempt = get().attempt!;
    expect(attempt.outcome).toBe("partial");
    expect(attempt.members.A).toMatchObject({ phase: "done" });
    expect(attempt.members.A.snapshot!.doc.signals).toHaveLength(2);
    expect(attempt.members.B).toMatchObject({ phase: "failed" });
    expect(attempt.members.B.error!.message).toBe(refused.message);
    expect(get().comparison).toBeUndefined(); // the finished side is NOT presented as half a comparison
    expect(get().run.phase).toBe("idle");
    expect(get().launches).toBe(1);
  });

  it("reports both failing as a failure, naming each side", async () => {
    await connected();
    client.replayImpl = async () => { throw new LabError("timeout", "The replay took too long"); };
    expect(await store.runComparison()).toBe(false);
    expect(get().attempt!.outcome).toBe("failed");
    expect(get().attempt!.members.A.error!.kind).toBe("timeout");
    expect(get().attempt!.members.B.error!.kind).toBe("timeout");
  });

  it("keeps the last good comparison (marked stale) when a later launch fails, and lets the failure be dismissed", async () => {
    await connected();
    differentB();
    await store.runComparison();
    const good = get().comparison!;
    client.replayImpl = async () => { throw refused; };
    expect(await store.runComparison()).toBe(false);
    expect(get().comparison).toBe(good);
    expect(get().attempt!.outcome).toBe("failed");
    store.dismissAttempt();
    expect(get().attempt).toBeUndefined();
    expect(get().comparison).toBe(good);
  });

  it("will not pair two results that are not the same recorded events, and says why", async () => {
    await connected();
    differentB();
    client.replayImpl = async (request) => sidesAnswer(request, nineBars().a, nineBars().b.slice(0, 8)); // B's tool run saw only 8 rows
    expect(await store.runComparison()).toBe(false);
    expect(get().attempt).toMatchObject({ outcome: "mismatch" });
    expect(get().attempt!.mismatch).toMatchObject({ kind: "comparison_mismatch" });
    expect(get().attempt!.mismatch!.message).toMatch(/event counts differ \(9 and 8\)/);
    expect(get().comparison).toBeUndefined();
    expect(get().attempt!.members.A.phase).toBe("done");
    expect(get().attempt!.members.B.phase).toBe("done");
  });

  it("treats a result for different dataset bytes as that side failing", async () => {
    await connected();
    differentB();
    client.replayImpl = async (request) => {
      const answer = sidesAnswer(request);
      return windowOf(request) === "3" ? { ...answer, raw: replayResponse(request, OTHER_SHA, nineBars().b).raw } : answer;
    };
    expect(await store.runComparison()).toBe(false);
    expect(get().attempt!.outcome).toBe("partial");
    expect(get().attempt!.members.B.error).toMatchObject({ kind: "result_mismatch" });
  });
});

describe("stale results", () => {
  it("marks the comparison stale after any edit, per side, and never relabels it", async () => {
    await connected();
    differentB();
    await store.runComparison();
    expect(compareStatus(get())).toMatchObject({ relation: "current", reasons: { shared: [], A: [], B: [] } });
    const before = get().comparison!;
    store.setValue("B", "long_window", "6");
    expect(compareStatus(get())).toMatchObject({ relation: "stale", reasons: { shared: [], A: [], B: ["Long window: 5 → 6"] } });
    expect(get().comparison).toBe(before);
    expect(get().comparison!.snapshots.B.request.params.find(([n]) => n === "long_window")![1]).toBe("5");
    store.setValue("B", "long_window", "5");
    expect(compareStatus(get()).relation).toBe("current");
    store.setValue("A", "requested_quantity", "3");
    expect(compareStatus(get())).toMatchObject({ relation: "stale", reasons: { A: ["Shares per request: 1 → 3"], B: [] } });
  });

  it("reports a changed dataset once, as a shared reason", async () => {
    await connected();
    await store.runComparison();
    store.selectBuiltin("mr_rearm");
    expect(compareStatus(get())).toMatchObject({ relation: "stale", reasons: { shared: ["Dataset: sma_crossover.csv → mr_rearm.csv"], A: [], B: [] } });
    expect(get().comparison!.datasetSha256).toBe(SHA);
  });

  it("a rebuilt engine makes it stale", async () => {
    await connected();
    await store.runComparison();
    client.statusResult = readyStatus("d4".repeat(32));
    await lab.pollStatus();
    expect(compareStatus(get())).toMatchObject({ relation: "stale", reasons: { shared: ["The replay engine was rebuilt since this result"] } });
  });

  it("a changed shared allowlist makes both sides stale", async () => {
    await connected();
    await store.runComparison();
    store.setSymbols("AAPL,MSFT");
    const status = compareStatus(get());
    expect(status.relation).toBe("stale");
    expect(status.reasons.A).toEqual(["Symbols: AAPL → AAPL,MSFT"]);
    expect(status.reasons.B).toEqual(["Symbols: AAPL → AAPL,MSFT"]);
  });
});
