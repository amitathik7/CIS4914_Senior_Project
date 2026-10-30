/// <reference types="node" />
// Tests against the REAL strategy_lab_replay executable (and, for the end-to-end block, the real Python gateway).
//
// They skip, with a reason shown in the run, when the executable has not been built (docs/STRATEGY_LAB.md section 4) or Python is
// not on PATH; a skipped run is not a pass. What they check is the console's READING of the real tool: that the lossless parser,
// the structural checks, the cursor model and the exports agree with the 13 hand-derived scenarios in
// tests/fixtures/strategy_lab/scenarios (the engine's own correctness is the C++ and Python suites' job), and that the gateway
// delivers the tool's output byte for byte.

import { spawn, spawnSync, type ChildProcess } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import net from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { HttpLabClient, LabError } from "../api/lab";
import { buildModel, resultOf } from "./cursor";
import { diagnosticsCsv, signalsCsv } from "./exports";
import { parseReplay } from "./replay";
import { LabStore, activeCheck, resultStatus } from "./store";

const ROOT = fileURLToPath(new URL("../../../", import.meta.url));
const FIXTURES = join(ROOT, "tests", "fixtures", "strategy_lab");
const NAME = process.platform === "win32" ? "strategy_lab_replay.exe" : "strategy_lab_replay";

function findRunner(): string | undefined {
  const fromEnv = process.env.STRATEGY_LAB_EXE;
  if (fromEnv) return existsSync(fromEnv) ? fromEnv : undefined;
  return ["Release", "RelWithDebInfo", "MinSizeRel", "Debug"].map((c) => join(ROOT, "out", "strategy_lab", "bin", c, NAME)).find(existsSync);
}

const exe = findRunner();
const pythonOk = (() => {
  const probe = spawnSync("python", ["--version"], { encoding: "utf8" });
  return probe.status === 0;
})();
if (!exe) console.warn("[real.test] strategy_lab_replay is not built: the real-engine tests are SKIPPED (build: docs/STRATEGY_LAB.md section 4).");

/** Run the tool the way the bridge does: one UTF-8 response file, a private directory, the dataset copied to dataset.csv. */
function runTool(strategies: { kind: string; params: Record<string, string> }[], dataset: Buffer | string, compact = true) {
  const dir = mkdtempSync(join(tmpdir(), "te-real-"));
  try {
    const args = ["run", "--dataset", "dataset.csv", ...strategies.flatMap((s) => ["--strategy", s.kind, ...Object.entries(s.params).flatMap(([k, v]) => ["--param", `${k}=${v}`])]), "--max-rows", "20000", ...(compact ? [] : ["--pretty"])];
    writeFileSync(join(dir, "args.txt"), args.join("\n") + "\n", "utf8");
    writeFileSync(join(dir, "dataset.csv"), dataset);
    const result = spawnSync(exe!, ["@args.txt"], { cwd: dir, maxBuffer: 256 * 1024 * 1024 });
    return { code: result.status, stdout: result.stdout.toString("utf8"), stderr: result.stderr.toString("utf8") };
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

interface Scenario {
  id: string;
  dataset: { path: string };
  strategies: { kind: string; params: Record<string, string> }[];
  expect: {
    outcome: string;
    warnings?: string[];
    signals?: { strategy: string; event_index: number; symbol: string; side: string; signal_id: number }[];
    events?: Record<string, unknown>[];
  };
}

const scenarios: Scenario[] = readdirSync(join(FIXTURES, "scenarios"))
  .filter((f) => f.endsWith(".json"))
  .map((f) => JSON.parse(readFileSync(join(FIXTURES, "scenarios", f), "utf8")) as Scenario)
  .filter((s) => s.expect.outcome === "result");

const rational = (text: string): number => {
  const [a, b] = text.split("/");
  return Number(a) / Number(b ?? 1);
};

describe.skipIf(!exe)("the console reads the real tool's output as the hand-derived scenarios expect", () => {
  it("found the 13 result scenarios", () => expect(scenarios).toHaveLength(13));

  it.each(scenarios.map((s) => [s.id, s] as const))("%s", (_id, scenario) => {
    const run = runTool(scenario.strategies, readFileSync(join(FIXTURES, scenario.dataset.path)));
    expect(run.code).toBe(0);
    const doc = parseReplay(run.stdout);
    expect(doc.warnings.map((w) => w.code).sort()).toEqual([...(scenario.expect.warnings ?? [])].sort());

    // The signals, in publish order, with their run-scoped ids.
    const got = doc.signals.map((s) => ({ strategy: s.strategyId, event_index: s.eventIndex, symbol: s.symbol, side: s.side, signal_id: Number(s.id) }));
    expect(got).toEqual(scenario.expect.signals);

    // Every event expectation, per strategy, through the cursor model.
    for (const strategy of scenario.strategies) {
      const model = buildModel(doc, strategy.params.strategy_id);
      for (const want of (scenario.expect.events ?? []).filter((e) => e.strategy === strategy.params.strategy_id)) {
        const range = (want.event_range as [number, number] | undefined) ?? [want.event_index as number, want.event_index as number];
        for (let i = range[0]; i <= range[1]; i++) {
          const event = model.events[i]!;
          const result = resultOf(model, event);
          const where = `${scenario.id} ${strategy.params.strategy_id} event ${i}`;
          if ("verdict" in want) expect(result.verdict, where).toBe(want.verdict);
          if ("reason" in want) expect(result.reason, where).toBe(want.reason);
          if ("action" in want) expect(result.action, where).toBe(want.action);
          if ("symbol" in want) expect(event.symbol, where).toBe(want.symbol);
          if ("exchange_time" in want) expect(event.time, where).toBe(want.exchange_time);
          if ("type" in want) expect(event.type, where).toBe(want.type);
          if ("window_fill" in want) expect(result.window?.fill, where).toBe(want.window_fill);
          if ("window_size" in want) expect(result.window?.size, where).toBe(want.window_size);
          for (const [name, reason] of Object.entries((want.unavailable as Record<string, string> | undefined) ?? {})) {
            expect(result.unavailable[name], `${where} ${name}`).toBe(reason);
            expect(result.indicators[name], `${where} ${name} must not exist when unavailable`).toBeUndefined(); // never zero
          }
          for (const [name, spec] of Object.entries((want.indicators as Record<string, { exact?: string; approx?: string }> | undefined) ?? {})) {
            const value = result.indicators[name]?.value;
            expect(value, `${where} ${name}`).toBeDefined();
            expect(Math.abs(value! - (spec.exact !== undefined ? rational(spec.exact) : Number(spec.approx))), `${where} ${name}`).toBeLessThan(1e-9);
          }
        }
      }
    }
  });
});

describe.skipIf(!exe)("exact numbers through the real tool", () => {
  it("the INT64-edge prices of precision_edge.csv come back digit for digit", () => {
    const csv = readFileSync(join(FIXTURES, "datasets", "precision_edge.csv"), "utf8");
    const run = runTool([{ kind: "mean_reversion", params: { lookback: "2", symbols: "AAPL" } }], csv);
    const doc = parseReplay(run.stdout);
    expect(doc.events.map((e) => e.price)).toEqual(["0.1", "0.3", "0.000001", "123456789.123456", "1.25", "9223372036854.775807"]);
  });

  it("an int64 volume and a six-place price survive the whole trip, export included", () => {
    const csv = "symbol,exchange_time,type,price,open,high,low,volume\n" +
      "AAPL,2026-01-05T14:30:00.123456789Z,bar,9223372036854.775807,9223372036854.775806,9223372036854.775807,9223372036854.775805,9223372036854775807\n" +
      "AAPL,2026-01-05T14:30:00.123456790Z,bar,0.000001,0.000001,0.000002,0.000001,0\n";
    const run = runTool([{ kind: "mean_reversion", params: { lookback: "2", symbols: "AAPL" } }], csv);
    expect(run.code).toBe(0);
    const doc = parseReplay(run.stdout);
    expect(doc.events[0]).toMatchObject({ price: "9223372036854.775807", volume: "9223372036854775807", time: "2026-01-05T14:30:00.123456789Z" });
    expect(doc.events[1]).toMatchObject({ price: "0.000001", volume: "0", time: "2026-01-05T14:30:00.123456790Z" }); // 1 ns later: kept apart
    const text = diagnosticsCsv(buildModel(doc), undefined, "full_run", 0, null);
    expect(text).toContain("9223372036854.775807");
    expect(text).toContain("2026-01-05T14:30:00.123456790Z");
    expect(signalsCsv(buildModel(doc), "full_run", 0, null)).not.toContain("\u0000");
  });
});

// ---- end to end: HttpLabClient -> real gateway -> real tool --------------------------------------------------------------

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

describe.skipIf(!exe || !pythonOk)("end to end through the real gateway", () => {
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

  const connected = async () => {
    const store = new LabStore(new HttpLabClient(base), { storage: undefined, checkDebounceMs: 0 });
    await store.connect();
    return store;
  };

  it("serves the tool's own catalog and the fixtures", async () => {
    const store = await connected();
    const state = store.getState();
    expect(state.service.phase).toBe("online");
    const sma = state.catalog!.strategies.find((s) => s.kind === "sma_crossover")!;
    expect(sma.params.map((p) => [p.name, p.defaultText])).toEqual([["strategy_id", "sma_crossover"], ["short_window", "5"], ["long_window", "20"], ["requested_quantity", "1"], ["symbols", undefined]]);
    expect(state.catalog!.strategies.find((s) => s.kind === "ml")).toMatchObject({ available: false });
    expect(state.datasets).toHaveLength(11);
    expect(state.datasets.find((d) => d.key === "sma_crossover")).toMatchObject({ sha256: createHash("sha256").update(readFileSync(join(FIXTURES, "datasets", "sma_crossover.csv"))).digest("hex") });
  });

  it("replays the documented fixture: Buy at event 5, Sell at event 8, and the tool's bytes are intact", async () => {
    const store = await connected();
    expect(await store.runReplay()).toBe(true);
    const snap = store.getState().snapshot!;
    expect(snap.model.signals.map((s) => [s.eventIndex + 1, s.side, s.id])).toEqual([[5, "buy", "1"], [8, "sell", "2"]]);
    expect(store.getState().launches).toBe(1);
    // provenance.result_sha256 is the SHA-256 of the compact `result` bytes: it only matches if nothing in transit changed a byte.
    const start = snap.raw.indexOf('"result":') + '"result":'.length;
    const result = snap.raw.slice(start, snap.raw.lastIndexOf("}"));
    expect(createHash("sha256").update(result, "utf8").digest("hex")).toBe(snap.doc.resultSha256);
    expect(snap.raw.endsWith("}\n")).toBe(true);
    expect(resultStatus(store.getState())).toMatchObject({ phase: "idle", relation: "current" });
  });

  it("the engine check returns the real constructors' words, and a derived fact", async () => {
    const store = await connected();
    store.setValue("short_window", "5");
    store.setValue("long_window", "3");
    await new Promise((r) => setTimeout(r, 400));
    const bad = activeCheck(store.getState());
    expect(bad.phase).toBe("rejected");
    expect(bad.phase === "rejected" && bad.error.message).toMatch(/long_window must be greater than short_window/);
    expect(bad.phase === "rejected" && bad.error.fields).toEqual(["long_window", "short_window"]);
    store.setValue("long_window", "20");
    await new Promise((r) => setTimeout(r, 400));
    const good = activeCheck(store.getState());
    expect(good.phase === "ok" && good.result.derived.earliest_signal_accepted_bar).toBe(21);
  });

  it("an uploaded file goes through the tool's own parser: exact int64 values back, and refusals with line and column", async () => {
    const store = await connected();
    const csv = "symbol,exchange_time,type,price,volume\nAAPL,2026-01-05T14:30:00Z,bar,9223372036854.775807,9223372036854775807\nAAPL,2026-01-05T14:31:00Z,bar,1,5\n";
    await store.uploadBytes("edge.csv", new TextEncoder().encode(csv));
    store.loadDraft({ kind: "mean_reversion", values: { symbols: "AAPL", lookback: "2" } }, "test");
    expect(await store.runReplay()).toBe(true);
    const doc = store.getState().snapshot!.doc;
    expect(doc.events[0]).toMatchObject({ price: "9223372036854.775807", volume: "9223372036854775807" });
    expect(doc.dataset.name).toBe("dataset.csv"); // the tool reports its own copy's name; the console shows the real file name
    expect(store.getState().snapshot!.meta.dataset).toMatchObject({ kind: "upload", name: "edge.csv", synthetic: false });

    await store.uploadBytes("bad.csv", readFileSync(join(FIXTURES, "invalid", "bad_price_text.csv")));
    expect(await store.runReplay()).toBe(false);
    const failure = store.getState().run.error!;
    expect(failure.kind).toBe("dataset_invalid");
    expect(failure.problems[0]).toMatchObject({ column: "price" });
    expect(failure.problems[0]!.line).toBeGreaterThan(0);
    expect(store.getState().snapshot!.doc.events[0]!.volume).toBe("9223372036854775807"); // the earlier good result is untouched
  });

  it("reports an unreachable service as such", async () => {
    const dead = new HttpLabClient(`http://127.0.0.1:${await freePort()}/lab`);
    await expect(dead.status()).rejects.toMatchObject({ kind: "service_unavailable" });
    await expect(dead.status()).rejects.toBeInstanceOf(LabError);
  });

  it("refuses a request the console should never send", async () => {
    const client = new HttpLabClient(base);
    await expect(client.replay({ strategy: { kind: "sma_crossover", params: [["bogus", "1"]] }, dataset: { builtin: "sma_crossover" } })).rejects.toMatchObject({ kind: "bad_request" });
    await expect(client.replay({ strategy: { kind: "ml", params: [] }, dataset: { builtin: "sma_crossover" } })).rejects.toMatchObject({ kind: "bad_request" });
  });
});
