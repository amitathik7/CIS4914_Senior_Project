/// <reference types="node" />
// Compare and Validation against the REAL strategy_lab_replay executable, through the real Python gateway and the console's own
// HttpLabClient, stores and reader. They skip, with a reason, when the executable is not built or Python is not on PATH (a skipped run
// is not a pass). Expectations come from arithmetic done by hand in this file or from the hand-derived scenario files in
// tests/fixtures/strategy_lab/scenarios; nothing is copied from a run of the tool.

import { spawn, spawnSync, type ChildProcess } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import net from "node:net";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { HttpLabClient } from "../api/lab";
import { buildCompareModel, differencePositions } from "./compare";
import { compareEvent } from "./compareEvent";
import { bundleZip, signalsCsv } from "./compareExports";
import { compareStatus } from "./compareState";
import { CompareStore } from "./compareStore";
import { agreementPercent, summarizeFull } from "./compareSummary";
import { LabStore } from "./store";
import { ValidationStore, validationStatus } from "./validationStore";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const FIXTURES = join(ROOT, "tests", "fixtures", "strategy_lab");
const NAME = process.platform === "win32" ? "strategy_lab_replay.exe" : "strategy_lab_replay";

function findRunner(): string | undefined {
  const fromEnv = process.env.STRATEGY_LAB_EXE;
  if (fromEnv) return existsSync(fromEnv) ? fromEnv : undefined;
  return ["Release", "RelWithDebInfo", "MinSizeRel", "Debug"].map((c) => join(ROOT, "out", "strategy_lab", "bin", c, NAME)).find(existsSync);
}

const exe = findRunner();
const pythonOk = spawnSync("python", ["--version"], { encoding: "utf8" }).status === 0;
if (!exe) console.warn("[compare.real.test] strategy_lab_replay is not built: the real-engine Compare tests are SKIPPED (docs/STRATEGY_LAB.md section 4).");

async function freePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const { port } = server.address() as net.AddressInfo;
      server.close(() => resolve(port));
    });
  });
}

interface Scenario {
  dataset: { path: string };
  strategies: { kind: string; params: Record<string, string> }[];
  expect: { signals?: { strategy: string; event_index: number; symbol: string; side: string; signal_id: number }[] };
}
const scenario = (file: string): Scenario => JSON.parse(readFileSync(join(FIXTURES, "scenarios", file), "utf8")) as Scenario;

type Config = { kind: string; values: Record<string, string> };
const sma = (short: string, long: string, symbols = "AAPL", id = `sma_${short}_${long}`): Config => ({
  kind: "sma_crossover", values: { strategy_id: id, short_window: short, long_window: long, requested_quantity: "1", symbols },
});
const reversion = (lookback: string, entry: string, rearm: string, symbols = "AAPL", id = `mr_${lookback}`): Config => ({
  kind: "mean_reversion", values: { strategy_id: id, lookback, entry_threshold: entry, rearm_threshold: rearm, requested_quantity: "1", symbols },
});

describe.skipIf(!exe || !pythonOk)("Compare through the real gateway and engine", () => {
  let child: ChildProcess | undefined;
  let base = "";

  beforeAll(async () => {
    const port = await freePort();
    child = spawn("python", ["lab_gateway.py", "--port", String(port), "--exe", exe!], { cwd: join(ROOT, "python", "strategy_lab"), stdio: "ignore" });
    base = `http://127.0.0.1:${port}/lab`;
    for (let i = 0; i < 100; i++) {
      try {
        if ((await fetch(`${base}/v1/status`)).ok) return;
      } catch {
        // not listening yet
      }
      await new Promise((r) => setTimeout(r, 100));
    }
    throw new Error("the gateway did not start");
  }, 20_000);

  afterAll(() => {
    child?.kill();
  });

  /** Connected stores with side A and B loaded from `a` and `b`, over the built-in dataset `dataset`. Nothing has run. */
  async function ready(dataset: string, a: Config, b: Config) {
    const client = new HttpLabClient(base);
    const lab = new LabStore(client, { storage: undefined, checkDebounceMs: 0 });
    const compare = new CompareStore(client, lab, { checkDebounceMs: 0 });
    await lab.connect();
    compare.selectBuiltin(dataset);
    compare.setSymbols(a.values.symbols!);
    expect(compare.loadDraft("A", { kind: a.kind, values: a.values }, "test A")).toBe(true);
    expect(compare.loadDraft("B", { kind: b.kind, values: b.values }, "test B")).toBe(true);
    return { compare, lab, client };
  }

  it("two parameter sets of one strategy on the documented fixture: hand-derived requests, agreement and its denominator", async () => {
    // closes 3 2 1 2 3 4 3 2 1. SMA 2/3 asks Buy at event 5 and Sell at event 8 (docs). SMA 3/5, by hand: from event 5 SMA3 2 vs SMA5 2.2
    // (baseline, below), event 6 SMA3 3 vs SMA5 2.4 (crosses above: Buy), events 7 and 8 stay above, event 9 SMA3 2 vs SMA5 2.6 (Sell).
    const { compare } = await ready("sma_crossover", sma("2", "3"), sma("3", "5"));
    expect(await compare.runComparison()).toBe(true);
    const done = compare.getState().comparison!;
    const requests = (m: "A" | "B") => done.model.members[m].signals.map((s) => [s.eventIndex + 1, s.side, s.id]);
    expect(requests("A")).toEqual([[5, "buy", "1"], [8, "sell", "2"]]);
    expect(requests("B")).toEqual([[6, "buy", "1"], [9, "sell", "2"]]);

    // Two independent engine runs: ids count 1, 2 in each, so every signal is keyed by its side.
    expect(done.model.signals.map((s) => s.key.replace(/:.*$/, "")).length).toBe(4);
    expect(new Set(done.model.signals.map((s) => s.key)).size).toBe(4);
    expect(done.snapshots.A.doc.runId).not.toBe(done.snapshots.B.doc.runId);

    // Rows 1-2: both warming up. Rows 3-4: only A has its window (3 bars). Rows 5-9: both evaluate, and only row 7 has no request on either side.
    const full = summarizeFull(done.model);
    expect(full.agreement).toMatchObject({ comparable: 5, agree: 1, differ: 4, notComparable: 4, requestDifferences: 4, verdictDifferences: 2 });
    expect(full.agreement.excluded).toEqual([{ label: "A evaluated · B warming up", count: 2 }, { label: "both warming up", count: 2 }]);
    expect(agreementPercent(1, 5)).toBe("20.0%");
    expect(differencePositions(done.model, "requests", null)).toEqual([5, 6, 8, 9]);
    expect(compareStatus(compare.getState()).relation).toBe("current");
    expect(compare.getState().launches).toBe(1);
  });

  it("exports both runs byte for byte, and each one's result digest still verifies", async () => {
    const { compare } = await ready("sma_crossover", sma("2", "3"), sma("3", "5"));
    await compare.runComparison();
    const done = compare.getState().comparison!;
    for (const m of ["A", "B"] as const) {
      const snap = done.snapshots[m];
      const start = snap.raw.indexOf('"result":') + '"result":'.length;
      expect(createHash("sha256").update(snap.raw.slice(start, snap.raw.lastIndexOf("}")), "utf8").digest("hex")).toBe(snap.doc.resultSha256);
    }
    const zip = bundleZip(done);
    const text = new TextDecoder().decode(zip);
    expect(text).toContain(done.snapshots.A.raw); // stored, not compressed: the tool's own bytes are in the archive as they came
    expect(text).toContain(done.snapshots.B.raw);
    expect(Array.from(bundleZip(done))).toEqual(Array.from(zip));
    expect(signalsCsv(done, "full_run", done.model.total, null).split("\r\n").filter(Boolean)).toHaveLength(1 + 4);
  });

  it("two different strategies on two interleaved symbols: each side's requests are the hand-derived scenario's, row by row", async () => {
    const s = scenario("10_interleaved_symbols_two_strategies.json");
    const [smaConf, mrConf] = s.strategies;
    const { compare } = await ready("two_symbols_interleaved", { kind: smaConf!.kind, values: { requested_quantity: "1", ...smaConf!.params } }, { kind: mrConf!.kind, values: { requested_quantity: "1", ...mrConf!.params } });
    expect(await compare.runComparison()).toBe(true);
    const done = compare.getState().comparison!;
    const wanted = (strategy: string) => (s.expect.signals ?? []).filter((x) => x.strategy === strategy).map((x) => [x.event_index, x.symbol, x.side]);
    const got = (m: "A" | "B") => done.model.members[m].signals.map((x) => [x.eventIndex, x.symbol, x.side]);
    expect(got("A")).toEqual(wanted("sma_2_3"));
    expect(got("B")).toEqual(wanted("mr_4"));
    expect(done.model.symbols).toEqual(["AAPL", "MSFT"]);
    // Opposing requests on the very same row (event 8, AAPL: A Buy, B Sell) are a difference, found by position.
    expect(done.model.rows[8]!.sides).toEqual({ A: "buy", B: "sell" });
    expect(done.model.rows[8]!.relation).toBe("differ");
    expect(done.model.rows[7]!.sides).toEqual({ A: "", B: "buy" });
    // The display filter keeps only that symbol's rows, in recorded order, without moving anything.
    const msft = differencePositions(done.model, "requests", "MSFT");
    expect(msft.every((p) => done.model.events[p - 1]!.symbol === "MSFT")).toBe(true);
    expect(msft).toEqual([8, 12, 14]); // A and B disagree on MSFT at events 7 (B only), 11 (B only) and 13 (A Buy, B Sell), 0-based
    // Each side keeps its own diagnostics: indicators of the other strategy are "not reported", never zero.
    const rows = compareEvent(done.model, 12);
    expect(rows.find((r) => r.label === "short_sma")).toMatchObject({ b: "not reported by this strategy", status: "not_comparable" });
    expect(rows.find((r) => r.label === "z_score")).toMatchObject({ a: "not reported by this strategy" });
    expect(compare.getState().launches).toBe(1);
  });

  it("equal timestamps on two symbols stay two events, and the sides are joined by position", async () => {
    const s = scenario("11_equal_time_aapl_first.json");
    const conf = s.strategies[0]!;
    const { compare } = await ready("tie_aapl_first", { kind: conf.kind, values: { requested_quantity: "1", ...conf.params } }, sma("1", "3", "AAPL,MSFT"));
    await compare.runComparison();
    const done = compare.getState().comparison!;
    const { events, rows } = done.model;
    const tied = events.filter((e, i) => events.findIndex((x) => x.time === e.time) !== i);
    expect(tied.length).toBeGreaterThan(0); // the fixture really has rows sharing a timestamp
    for (const e of tied) {
      const twin = events.find((x) => x.time === e.time && x.index !== e.index)!;
      expect(twin.symbol).not.toBe(e.symbol);
    }
    expect(rows.map((r) => r.index)).toEqual(events.map((e) => e.index));
    expect(done.model.members.A.signals.map((x) => [x.eventIndex, x.symbol, x.side])).toEqual((s.expect.signals ?? []).map((x) => [x.event_index, x.symbol, x.side]));
  });

  it("refuses to pair runs over different dataset bytes", async () => {
    const first = await ready("tie_aapl_first", sma("1", "2", "AAPL,MSFT"), sma("1", "3", "AAPL,MSFT"));
    await first.compare.runComparison();
    const one = first.compare.getState().comparison!;
    first.compare.selectBuiltin("tie_msft_first");
    await first.compare.runComparison();
    const two = first.compare.getState().comparison!;
    expect(() => buildCompareModel(one.snapshots.A, two.snapshots.A)).toThrowError(/datasets differ/);
  });

  it("insufficient warm-up on both sides leaves nothing comparable, and says so instead of a ratio", async () => {
    const { compare } = await ready("sma_crossover", sma("5", "20"), sma("6", "20"));
    await compare.runComparison();
    const full = summarizeFull(compare.getState().comparison!.model);
    expect(full.agreement).toMatchObject({ comparable: 0, agree: 0, differ: 0, notComparable: 9 });
    expect(full.agreement.excluded).toEqual([{ label: "both warming up", count: 9 }]);
    expect(compare.getState().comparison!.model.signals).toEqual([]);
  });

  it("constant-price windows: no requests, z-scores unavailable (never zero), rows still comparable", async () => {
    const { compare } = await ready("constant_price", sma("5", "20"), reversion("20", "2", "0.5"));
    await compare.runComparison();
    const done = compare.getState().comparison!;
    expect(done.model.signals).toEqual([]);
    const mr = done.model.members.B;
    const evaluated = mr.events.filter((e) => e.results[0]!.verdict === "evaluated");
    expect(evaluated.length).toBeGreaterThan(0);
    for (const e of evaluated) {
      expect(e.results[0]!.indicators.z_score).toBeUndefined();
      expect(e.results[0]!.unavailable.z_score).toBe("constant_window");
    }
    expect(summarizeFull(done.model).agreement.differ).toBe(0);
  });

  it("an invalid CSV fails both sides with the tool's own line and column, and produces no comparison", async () => {
    const { compare } = await ready("sma_crossover", sma("2", "3"), sma("3", "5"));
    await compare.uploadBytes("bad.csv", readFileSync(join(FIXTURES, "invalid", "bad_price_text.csv")));
    expect(await compare.runComparison()).toBe(false);
    const attempt = compare.getState().attempt!;
    expect(attempt.outcome).toBe("failed");
    for (const m of ["A", "B"] as const) {
      expect(attempt.members[m].error!.kind).toBe("dataset_invalid");
      expect(attempt.members[m].error!.problems[0]).toMatchObject({ column: "price" });
    }
    expect(compare.getState().comparison).toBeUndefined();
  });

  it("a rejected configuration on one side is the engine's message verbatim, and nothing is replayed for either", async () => {
    const { compare } = await ready("sma_crossover", sma("2", "3"), sma("3", "5"));
    compare.setValue("B", "short_window", "5");
    compare.setValue("B", "long_window", "3");
    await new Promise((r) => setTimeout(r, 400));
    expect(await compare.runComparison()).toBe(false);
    expect(compare.getState().launches).toBe(0);
  });
});

describe.skipIf(!exe || !pythonOk)("Validation through the real gateway", () => {
  let child: ChildProcess | undefined;
  let base = "";

  beforeAll(async () => {
    const port = await freePort();
    child = spawn("python", ["lab_gateway.py", "--port", String(port), "--exe", exe!], { cwd: join(ROOT, "python", "strategy_lab"), stdio: "ignore" });
    base = `http://127.0.0.1:${port}/lab`;
    for (let i = 0; i < 100; i++) {
      try {
        if ((await fetch(`${base}/v1/status`)).ok) return;
      } catch {
        // not listening yet
      }
      await new Promise((r) => setTimeout(r, 100));
    }
    throw new Error("the gateway did not start");
  }, 20_000);

  afterAll(() => {
    child?.kill();
  });

  it("runs only on request, names the executable and files it ran, and counts what is on disk", async () => {
    const client = new HttpLabClient(base);
    const lab = new LabStore(client, { storage: undefined, checkDebounceMs: 0 });
    const store = new ValidationStore(client, lab);
    await lab.connect();
    await store.loadInfo();
    const files = readdirSync(join(FIXTURES, "scenarios")).filter((f) => f.endsWith(".json"));
    expect(store.getState().info!.scenarios).toHaveLength(files.length);
    expect(store.getState().launches).toBe(0);
    expect(store.getState().result).toBeUndefined();

    expect(await store.run()).toBe(true);
    const { report } = store.getState().result!;
    expect(report.summary).toMatchObject({ scenarios: files.length, passed: files.length, failed: 0, error: 0 });
    expect(report.executable.sha256).toBe(createHash("sha256").update(readFileSync(exe!)).digest("hex"));
    expect(report.scenarioFiles.identity).toBe(store.getState().info!.identity);
    expect(report.scenarios.every((s) => s.executableSha256 === report.executable.sha256 && /^[0-9a-f]{64}$/.test(s.datasetSha256GivenToTool))).toBe(true);
    expect(validationStatus(store.getState()).relation).toBe("current");
    expect(store.getState().launches).toBe(1);

    // The opt-in demonstration fails on purpose and carries expected-against-actual differences.
    store.setIncludeDemonstration(true);
    await store.run();
    const demo = store.getState().result!.report.scenarios.filter((s) => s.demonstration);
    expect(demo).toHaveLength(1);
    expect(demo[0]!.status).toBe("failed");
    expect(demo[0]!.mismatches.every((m) => m.where && m.expected && m.actual)).toBe(true);
    expect(store.getState().result!.report.summary.failed).toBe(0);
  }, 60_000);
});
