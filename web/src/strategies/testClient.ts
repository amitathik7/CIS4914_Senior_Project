// Test helper: a controllable stand-in for the gateway. Each replay() call returns a promise the test settles by hand, so races
// (cancel, duplicate submit, a late answer for an older launch) can be staged exactly.

import type { BuiltinDataset, CheckResult, LabClient, LabStatus, ReplayRequest, ReplayResponse, StrategyRequest, ValidationRequest } from "../api/lab";
import { LabError } from "../api/lab";
import { makeDoc, type Spec } from "./testDoc";

export const SHA = "a1".repeat(32);
export const OTHER_SHA = "b2".repeat(32);

const param = (name: string, type: string, def: string | number | undefined, constraint: string, required = false) =>
  JSON.stringify({ name, type, required, ...(def !== undefined ? { default: def } : {}), constraint }).replace(/"default":"(\d+(?:\.\d+)?)"/, (_m, n: string) => `"default":${n}`);

const reasons = [
  ["warming_up", "warming_up", "Fewer than long_window bars have been accepted for this symbol, so no averages exist yet."],
  ["same_side", "evaluated", "The short average is still on the same side of the long average; a crossover signals once."],
  ["crossover_buy", "evaluated", "The short average crossed above the long one: a Buy request was emitted."],
  ["crossover_sell", "evaluated", "The short average crossed below the long one: a Sell request was emitted."],
  ["not_a_bar", "ignored", "The event is not a bar (a trade, quote or status); only bars carry a close."],
].map(([code, verdict, text]) => JSON.stringify({ code, verdict, text }));

const strategy = (kind: string, title: string, params: string[], indicators: string[], available = true) =>
  `{"kind":"${kind}","title":"${title}","summary":"summary of ${kind}","docs":"docs/${kind}.md","available":${available},` +
  (available ? "" : '"unavailable_reason":"Not implemented: the custom ML strategy has not been started.",') +
  `"parameters":[${params.join(",")}],"signal_metadata_keys":["trigger"],"decision_reasons":[${available ? reasons.join(",") : ""}],` +
  `"indicators":${JSON.stringify(indicators)},"states":[]}`;

const SYMBOLS = param("symbols", "string_list", undefined, "Comma-separated allowlist.", true);

export const catalogText =
  `{"schema":"strategy_lab.catalog","schema_version":"1.0","catalog":{"tool":"strategy_lab_replay","project_version":"0.0.1",` +
  `"limits":{"default_max_rows":50000,"hard_max_rows":200000,"max_strategies":16},"dataset_format":{"id":"strategy_lab_bars_csv/1","columns":[]},"bus_faults":[],"strategies":[` +
  strategy("sma_crossover", "Moving-average crossover", [
    param("strategy_id", "string", "sma_crossover", "Not empty."), param("short_window", "uint", "5", "At least 1 and below long_window."),
    param("long_window", "uint", "20", "Greater than short_window."), param("requested_quantity", "uint", "1", "A positive whole number."), SYMBOLS,
  ], ["short_sma", "long_sma", "short_minus_long"]) + "," +
  strategy("mean_reversion", "Mean reversion (rolling z-score)", [
    param("strategy_id", "string", "mean_reversion", "Not empty."), param("lookback", "uint", "20", "At least 2."),
    param("entry_threshold", "double", "2", "Greater than rearm_threshold."), param("rearm_threshold", "double", "0.5", "At least 0."),
    param("requested_quantity", "uint", "1", "A positive whole number."), SYMBOLS,
  ], ["mean", "standard_deviation", "z_score"]) + "," +
  strategy("ml", "Custom ML strategy", [], [], false) +
  "]}}\n";

export const builtin = (key: string, extra: Partial<BuiltinDataset> = {}): BuiltinDataset => ({
  key, label: `${key} demo`, description: "", filename: `${key}.csv`, synthetic: true, provenance: "Synthetic fixture", available: true,
  bytes: 320, sha256: SHA, rows: 9, symbols: ["AAPL"],
  presets: { sma_crossover: { short_window: "2", long_window: "3", symbols: "AAPL" }, mean_reversion: { lookback: "4", entry_threshold: "1.5", symbols: "AAPL" } },
  ...extra,
});

export const readyStatus = (sha = "c3".repeat(32)): LabStatus => ({
  gateway: { version: "0.1.0", api: "1" },
  runner: { state: "ready", name: "strategy_lab_replay.exe", size: 1, sha256: sha, tool: "strategy_lab_replay", project_version: "0.0.1", schema_version: "1.0", default_max_rows: 50000, hard_max_rows: 200000, max_rows_used: 20000 },
});

export interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (error: unknown) => void;
}
export function deferred<T>(): Deferred<T> {
  let resolve!: (v: T) => void;
  let reject!: (e: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

/** A replay answer whose document has two signals (Buy at event 4, Sell at event 7) for one strategy, framed as the gateway would. */
export const SPECS: Spec[] = Array.from({ length: 9 }, (_, i) => ({
  symbol: "AAPL", minute: i, price: String(i + 1),
  ...(i === 4 ? { signals: [[0, "1", "buy"]] as [number, string, string][], reason: "crossover_buy" } : {}),
  ...(i === 7 ? { signals: [[0, "2", "sell"]] as [number, string, string][], reason: "crossover_sell" } : {}),
}));

export function replayResponse(request: ReplayRequest, sha = SHA, specs: Spec[] = SPECS): ReplayResponse {
  return {
    meta: {
      gateway: { version: "0.1.0", api: "1" }, runner: { name: "strategy_lab_replay.exe", size: 1, sha256: "c3".repeat(32) },
      dataset: { kind: "builtin" in request.dataset ? "builtin" : "upload", name: "builtin" in request.dataset ? `${request.dataset.builtin}.csv` : request.dataset.name, sha256: sha, bytes: 320, synthetic: "builtin" in request.dataset, provenance: "test" },
      request: { kind: request.strategy.kind, params: request.strategy.params, max_rows: 20000 },
      process: { exit_code: 0, duration_ms: 12, args_file: "run\n", stderr: "", stderr_truncated: false },
    },
    raw: makeDoc(specs, { datasetSha: sha, kind: request.strategy.kind }),
  };
}

export class FakeClient implements LabClient {
  statusResult: LabStatus | LabError = readyStatus();
  datasetsList: BuiltinDataset[] = [builtin("sma_crossover"), builtin("mr_rearm", { sha256: OTHER_SHA })];
  checkImpl: (request: StrategyRequest) => Promise<CheckResult> = async (request) => ({
    ok: true, kind: request.kind, strategy_id: request.kind, window_size: 3, derived: { earliest_signal_accepted_bar: 4 }, parameters: {}, warnings: [],
  });
  replayImpl: (request: ReplayRequest, signal?: AbortSignal) => Promise<ReplayResponse> = async (request) => replayResponse(request);
  validationInfoImpl: () => Promise<string> = async () => "{}";
  validateImpl: (request: ValidationRequest, signal?: AbortSignal) => Promise<string> = async () => "{}";
  readonly replays: ReplayRequest[] = [];
  readonly checks: StrategyRequest[] = [];
  readonly validations: ValidationRequest[] = [];
  statusCalls = 0;

  status = async (): Promise<LabStatus> => {
    this.statusCalls++;
    if (this.statusResult instanceof LabError) throw this.statusResult;
    return this.statusResult;
  };
  catalog = async (): Promise<string> => catalogText;
  datasets = async () => ({ datasets: this.datasetsList, upload_limit_bytes: 8 * 1024 * 1024 });
  check = (request: StrategyRequest): Promise<CheckResult> => {
    this.checks.push(request);
    return this.checkImpl(request);
  };
  replay = (request: ReplayRequest, signal?: AbortSignal): Promise<ReplayResponse> => {
    this.replays.push(request);
    return this.replayImpl(request, signal);
  };
  validationInfo = (): Promise<string> => this.validationInfoImpl();
  validate = (request: ValidationRequest, signal?: AbortSignal): Promise<string> => {
    this.validations.push(request);
    return this.validateImpl(request, signal);
  };
}

/** A Storage stand-in that can be told to fail, like a browser with storage disabled or full. */
export class MemoryStorage implements Storage {
  private data = new Map<string, string>();
  fail = false;
  get length() {
    return this.data.size;
  }
  clear() {
    this.data.clear();
  }
  getItem(key: string) {
    if (this.fail) throw new Error("storage disabled");
    return this.data.get(key) ?? null;
  }
  key(i: number) {
    return [...this.data.keys()][i] ?? null;
  }
  removeItem(key: string) {
    this.data.delete(key);
  }
  setItem(key: string, value: string) {
    if (this.fail) throw new Error("quota exceeded");
    this.data.set(key, value);
  }
}
