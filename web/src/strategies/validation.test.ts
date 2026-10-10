import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { LabError } from "../api/lab";
import { LabStore } from "./store";
import { deferred, FakeClient, MemoryStorage, readyStatus } from "./testClient";
import { EXE_ID, FILES_ID, infoText, reportText } from "./testValidation";
import { parseValidationInfo, parseValidationReport } from "./validation";
import { ValidationStore, validationBlocker, validationStatus } from "./validationStore";

let client: FakeClient;
let lab: LabStore;
let store: ValidationStore;
const get = () => store.getState();
const flush = () => vi.runAllTimersAsync();

const connected = async () => {
  client = new FakeClient();
  client.validationInfoImpl = async () => infoText();
  client.validateImpl = async () => reportText();
  lab = new LabStore(client, { storage: new MemoryStorage(), checkDebounceMs: 0 });
  store = new ValidationStore(client, lab, { now: () => "2026-10-09T12:00:05.000Z" });
  await lab.connect();
  await flush();
};

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe("reading the gateway's documents", () => {
  it("reads the scenario list; the count is the files', never a constant", () => {
    const info = parseValidationInfo(infoText(["a", "b", "c", "d", "e"]));
    expect(info.scenarios.map((s) => s.id)).toEqual(["a", "b", "c", "d", "e"]);
    expect(info.files).toHaveLength(5);
    expect(info.identity).toBe(FILES_ID);
    expect(info.scenarios[0]).toMatchObject({ title: "Title of a", datasetPath: "datasets/a.csv", strategies: [{ kind: "sma_crossover", strategyId: "sma_crossover" }] });
    expect(parseValidationInfo(infoText([])).scenarios).toEqual([]);
  });

  it("reads a report with its identities, timestamps and the expected-against-actual differences", () => {
    const report = parseValidationReport(reportText([
      { id: "alpha" },
      { id: "beta", status: "failed", mismatches: [{ where: "signal 1 event_index", expected: "4", actual: "5", note: "event_index counts from 0" }] },
      { id: "gamma", status: "error", runId: null, error: { kind: "timeout", title: "The replay timed out", message: "took too long", exit_code: null } },
    ], { number: 3 }));
    expect(report.number).toBe(3);
    expect(report.startedAt).toBe("2026-10-09T12:00:00Z");
    expect(report.finishedAt).toBe("2026-10-09T12:00:02Z");
    expect(report.summary).toEqual({ scenarios: 3, passed: 1, failed: 1, error: 1, notRun: 0, overall: "errors: some checks could not be carried out" });
    expect(report.executable).toMatchObject({ sha256: EXE_ID, fileName: "strategy_lab_replay.exe", tool: "strategy_lab_replay", compiler: "MSVC" });
    expect(report.scenarioFiles.identity).toBe(FILES_ID);
    const [alpha, beta, gamma] = report.scenarios;
    expect(alpha).toMatchObject({ status: "passed", runId: "0123456789abcdef", datasetSha256GivenToTool: "ab".repeat(32), executableSha256: EXE_ID, durationS: "0.125" });
    expect(beta!.mismatches).toEqual([{ where: "signal 1 event_index", expected: "4", actual: "5", note: "event_index counts from 0" }]);
    expect(gamma).toMatchObject({ status: "error", error: { kind: "timeout", message: "took too long" } });
    expect(gamma!.runId).toBeUndefined(); // null is "not applicable", not an empty id
    expect(gamma!.resultSha256).toBeUndefined();
    expect(beta!.configuration[0]).toEqual({ kind: "sma_crossover", strategyId: "sma_crossover", parameters: [["short_window", "2"], ["long_window", "3"], ["symbols", "AAPL"]] });
  });

  it("keeps the opt-in demonstration apart from the totals", () => {
    const report = parseValidationReport(reportText([{ id: "alpha" }, { id: "demo", demonstration: true, status: "failed", mismatches: [{ where: "w", expected: "e", actual: "a" }] }], { include: true }));
    expect(report.summary.scenarios).toBe(1);
    expect(report.summary.failed).toBe(0);
    expect(report.demonstration).toMatchObject({ included: true, results: [{ id: "demo", status: "failed", failedAsDesigned: true }] });
    expect(report.scenarios.find((s) => s.demonstration)!.id).toBe("demo");
  });

  it("refuses a document that is not what it claims, naming the problem", () => {
    expect(() => parseValidationReport("not json")).toThrowError(LabError);
    expect(() => parseValidationReport(JSON.stringify({ report: "something_else" }))).toThrow(/Expected a strategy_lab.validation_report/);
    expect(() => parseValidationReport(reportText().replace('"report_version":"1.0"', '"report_version":"2.0"'))).toThrow(/version 2\.0/);
    expect(() => parseValidationReport(reportText().replace('"status":"passed"', '"status":"fine"'))).toThrow(/'fine' is not passed, failed, error or not_run/);
    expect(() => parseValidationReport(reportText().replace(/"checks_made":5/, '"checks_made":-1'))).toThrow(/checks_made/);
    expect(() => parseValidationInfo("[]")).toThrow(/expected an object/);
  });
});

describe("validation runs only on request", () => {
  it("opening the page and reading the file list start nothing", async () => {
    await connected();
    await store.loadInfo();
    await store.loadInfo();
    store.setIncludeDemonstration(true);
    store.setIncludeDemonstration(false);
    await flush();
    expect(client.validations).toHaveLength(0);
    expect(get().launches).toBe(0);
    expect(get().result).toBeUndefined();
    expect(get().info!.scenarios).toHaveLength(3);
  });

  it("runs once per click, passing the demonstration choice, and stores the report with the answer's exact text", async () => {
    await connected();
    await store.loadInfo();
    store.setIncludeDemonstration(true);
    expect(await store.run()).toBe(true);
    expect(client.validations).toEqual([{ include_demonstration: true }]);
    expect(get().launches).toBe(1);
    expect(get().result!.raw).toBe(reportText());
    expect(get().result!.receivedAt).toBe("2026-10-09T12:00:05.000Z");
    expect(Object.isFrozen(get().result)).toBe(true);
  });

  it("refuses a second validation while one is running, and drops the answer of a cancelled one", async () => {
    await connected();
    const first = deferred<string>();
    client.validateImpl = () => first.promise;
    const running = store.run();
    expect(validationBlocker(get())).toBe("A validation is already running.");
    expect(await store.run()).toBe(false);
    expect(client.validations).toHaveLength(1);
    store.cancel();
    expect(get().run).toMatchObject({ phase: "idle", cancelled: true });
    client.validateImpl = async () => reportText([{ id: "newer" }], { number: 2 });
    expect(await store.run()).toBe(true);
    first.resolve(reportText([{ id: "old" }], { number: 1 })); // the cancelled run's answer arrives late
    expect(await running).toBe(false);
    expect(get().result!.report.scenarios.map((s) => s.id)).toEqual(["newer"]);
    expect(get().result!.launch).toBe(2);
  });

  it("keeps the previous result when a later validation fails, and says why", async () => {
    await connected();
    await store.run();
    const good = get().result;
    client.validateImpl = async () => { throw new LabError("executable_missing", "strategy_lab_replay has not been built."); };
    expect(await store.run()).toBe(false);
    expect(get().result).toBe(good);
    expect(get().run).toMatchObject({ phase: "failed" });
    expect(get().run.error!.kind).toBe("executable_missing");
    store.dismissError();
    expect(get().run.phase).toBe("idle");
  });

  it("treats an unreadable answer as a failure, not a pass", async () => {
    await connected();
    client.validateImpl = async () => '{"report":"strategy_lab.validation_report"}';
    expect(await store.run()).toBe(false);
    expect(get().run.phase).toBe("failed");
    expect(get().result).toBeUndefined();
  });

  it("cannot run without a service or an engine", async () => {
    await connected();
    client.statusResult = new LabError("service_unavailable", "down");
    await lab.connect();
    expect(validationBlocker(get())).toMatch(/not running/);
    expect(await store.run()).toBe(false);
    client.statusResult = { gateway: { version: "0.1.0", api: "1" }, runner: { state: "missing", message: "not built", detail: "", setup: "cmake" } };
    await lab.connect();
    expect(validationBlocker(get())).toMatch(/has not been built/);
    expect(client.validations).toHaveLength(0);
  });

  it("reports an unreadable scenario list and does not offer to run on top of it", async () => {
    await connected();
    client.validationInfoImpl = async () => { throw new LabError("scenario_invalid", "No scenario files (*.json) were found."); };
    await store.loadInfo();
    expect(get().infoPhase).toBe("failed");
    expect(get().infoError!.message).toMatch(/No scenario files/);
    expect(validationBlocker(get())).toMatch(/scenario files could not be read/);
  });
});

describe("a result belongs to the build and the files it ran", () => {
  it("is current while the engine and the scenario files are the ones it used", async () => {
    await connected();
    await store.loadInfo();
    expect(validationStatus(get())).toEqual({ relation: "none", reasons: [] });
    await store.run();
    expect(validationStatus(get())).toEqual({ relation: "current", reasons: [] });
  });

  it("is marked as not describing a rebuilt engine, and is never relabelled", async () => {
    await connected();
    await store.loadInfo();
    await store.run();
    const before = get().result;
    client.statusResult = readyStatus("d4".repeat(32));
    await lab.pollStatus();
    const status = validationStatus(get());
    expect(status.relation).toBe("stale");
    expect(status.reasons).toEqual(["The replay engine was rebuilt since this validation, so it does not describe the build in use now."]);
    expect(get().result).toBe(before);
    expect(get().result!.report.executable.sha256).toBe(EXE_ID);
  });

  it("is marked when the scenario files changed", async () => {
    await connected();
    await store.loadInfo();
    await store.run();
    client.validationInfoImpl = async () => infoText(["alpha", "beta", "gamma", "delta"], "e5".repeat(32));
    await store.loadInfo();
    expect(validationStatus(get())).toMatchObject({ relation: "stale", reasons: ["The scenario files changed since this validation, so it does not describe the files in use now."] });
  });

  it("does not claim to be current before it can tell", async () => {
    await connected();
    await store.run(); // the scenario list was never read
    expect(validationStatus(get()).relation).toBe("unknown");
  });
});
