// The Strategies section's state: the editable draft, the chosen dataset, the engine check, the replay lifecycle and the one
// immutable snapshot of the last completed replay, plus the replay position. Framework-free (React reads it through
// useLabStore.ts), so every rule below is unit-tested with a fake client.
//
// Rules this file exists to enforce:
//   * Only runReplay() starts an engine run. Stepping, filtering, tab changes, downloads and editing never do (state.launches
//     counts real launches, and the tests assert it).
//   * A snapshot is never edited and never relabelled. Changing a setting makes it "stale" (resultStatus), nothing more.
//   * One replay at a time; a response that is not for the latest launch (cancelled, superseded) is dropped, as is an engine
//     check that is not for the latest request.
//   * "Engine check" is the real strategy constructors' verdict on the configuration (gateway /check): cross-field rules are
//     not duplicated in the console.

import { LabError, type BuiltinDataset, type CheckResult, type LabClient, type LabStatus, type ReplayRequest } from "../api/lab";
import { availableStrategies, parseCatalog, specOf, type Catalog, type StrategySpec } from "./catalog";
import { clamp, nextEvent, nextSignal, previousEvent } from "./cursor";
import {
  buildRequest, defaultValues, describeChanges, newDraft, requestKey, validateDraft, withValue, type DatasetIdentity, type FieldIssue,
  type RequestSnapshot, type StrategyDraft,
} from "./draft";
import { examplesFor, loadPresets, presetId, savePresets, type Preset } from "./presets";
import { makeSnapshot, type Snapshot } from "./snapshot";
import { DEFAULT_UPLOAD_LIMIT, describeUpload, precheckUpload, type UploadedFile } from "./uploads";

export type ServicePhase = "unknown" | "checking" | "online" | "no_runner" | "offline" | "error";
export interface ServiceState {
  phase: ServicePhase;
  status?: LabStatus;
  error?: LabError;
}

export type CheckState =
  | { phase: "idle" }
  | { phase: "checking"; key: string }
  | { phase: "ok"; key: string; result: CheckResult }
  | { phase: "rejected"; key: string; error: LabError }
  | { phase: "unavailable"; key: string; error: LabError };

export interface RunState {
  phase: "idle" | "running" | "failed";
  /** Moves on every launch and every cancel: a response carrying an older token is dropped. */
  token: number;
  error?: LabError;
  /** The file the failed launch was about (the tool calls every file it is given "dataset.csv"). */
  datasetName?: string;
  cancelled?: boolean;
}

export type DatasetSelection = { kind: "builtin"; key: string } | { kind: "upload" };
export type ResultTab = "signals" | "diagnostics" | "details";

export interface LabState {
  service: ServiceState;
  catalog?: Catalog;
  datasets: BuiltinDataset[];
  uploadLimit: number;
  draft: StrategyDraft;
  /** Where the draft came from, for a one-line caption; edited once the person changes it. */
  origin?: { label: string; edited: boolean };
  dataset: DatasetSelection;
  upload?: UploadedFile;
  uploadError?: string;
  check: CheckState;
  run: RunState;
  /** Engine launches in this browser tab (replays only; the engine check is separate). */
  launches: number;
  snapshot?: Snapshot;
  cursor: number;
  /** Display filter: a symbol, or null for all. Never an input to the strategy. */
  symbol: string | null;
  tab: ResultTab;
  presets: Preset[];
  presetsPersisted: boolean;
}

export interface StoreOptions {
  storage?: Storage;
  now?: () => string;
  checkDebounceMs?: number;
}

const EMPTY_DRAFT: StrategyDraft = { kind: "", values: {} };

// ---- pure selectors (the UI and the tests read state through these) ---------------------------------------------------

export function datasetIdentity(state: Pick<LabState, "dataset" | "datasets" | "upload">): DatasetIdentity | undefined {
  if (state.dataset.kind === "upload") return state.upload ? { kind: "upload", name: state.upload.name, sha256: state.upload.sha256 } : undefined;
  const key = state.dataset.key;
  const item = state.datasets.find((d) => d.key === key);
  return item?.available && item.sha256 ? { kind: "builtin", name: item.filename, sha256: item.sha256 } : undefined;
}

export function currentRequest(state: LabState): RequestSnapshot | undefined {
  const spec = specOf(state.catalog, state.draft.kind);
  const dataset = datasetIdentity(state);
  return spec && dataset ? { ...buildRequest(spec, state.draft), dataset } : undefined;
}

export function draftIssues(state: LabState): FieldIssue[] {
  const spec = specOf(state.catalog, state.draft.kind);
  return spec ? validateDraft(spec, state.draft) : [];
}

/** The check that belongs to the draft on screen (an older one is ignored). */
export function activeCheck(state: LabState): CheckState {
  const spec = specOf(state.catalog, state.draft.kind);
  if (!spec || state.check.phase === "idle") return { phase: "idle" };
  return state.check.key === JSON.stringify(buildRequest(spec, state.draft)) ? state.check : { phase: "idle" };
}

export interface ResultStatus {
  phase: "idle" | "running" | "failed";
  relation: "none" | "current" | "stale";
  reasons: string[];
  error?: LabError;
}

export function resultStatus(state: LabState): ResultStatus {
  const base = { phase: state.run.phase, error: state.run.error } as const;
  const snap = state.snapshot;
  if (!snap) return { ...base, relation: "none", reasons: [] };
  const request = currentRequest(state);
  const reasons = request ? describeChanges(snap.request, request, (kind) => specOf(state.catalog, kind)) : ["Settings are being edited"];
  const runner = state.service.status?.runner;
  if (runner?.state === "ready" && runner.sha256 !== snap.meta.runner.sha256) reasons.push("The replay engine was rebuilt since this result");
  const same = request !== undefined && requestKey(request) === snap.requestKey;
  return { ...base, relation: same && reasons.length === 0 ? "current" : "stale", reasons };
}

/** Why nothing can be replayed because of the service itself, in a sentence; undefined when it is online. */
export function serviceBlocker(service: ServiceState): string | undefined {
  switch (service.phase) {
    case "offline": return "The Strategy Lab service is not running, so nothing can be replayed. Start it (see the panel above), then retry.";
    case "no_runner": return "The service is running but the replay engine (strategy_lab_replay) has not been built. Build it as described above, then retry.";
    case "error": return "The service answered, but the console could not use its answer (see above).";
    case "online": return undefined;
    default: return "Connecting to the Strategy Lab service…";
  }
}

/** Why Run replay is unavailable right now, in a sentence; undefined when it can start. */
export function runBlocker(state: LabState): string | undefined {
  if (state.run.phase === "running") return "A replay is already running.";
  const service = serviceBlocker(state.service);
  if (service) return service;
  const spec = specOf(state.catalog, state.draft.kind);
  if (!spec) return "Choose a strategy.";
  if (!spec.available) return spec.unavailableReason || "This strategy is not available.";
  if (!datasetIdentity(state)) return state.dataset.kind === "upload" ? "Choose a CSV file." : "Choose a dataset.";
  if (draftIssues(state).length) return "Fix the highlighted settings first.";
  const check = activeCheck(state);
  if (check.phase === "rejected") return `The engine rejected this configuration: ${check.error.message}`;
  return undefined;
}

// ---- the store --------------------------------------------------------------------------------------------------------

export class LabStore {
  private state: LabState;
  private readonly listeners = new Set<() => void>();
  private runAbort?: AbortController;
  private checkTimer?: ReturnType<typeof setTimeout>;
  private checkSeq = 0;
  private connectSeq = 0;
  private initialized = false;
  private readonly now: () => string;
  private readonly debounceMs: number;
  private readonly storage?: Storage;

  constructor(private readonly client: LabClient, options: StoreOptions = {}) {
    this.now = options.now ?? (() => new Date().toISOString());
    this.debounceMs = options.checkDebounceMs ?? 350;
    this.storage = options.storage;
    this.state = {
      service: { phase: "unknown" }, datasets: [], uploadLimit: DEFAULT_UPLOAD_LIMIT, draft: EMPTY_DRAFT, dataset: { kind: "builtin", key: "" },
      check: { phase: "idle" }, run: { phase: "idle", token: 0 }, launches: 0, cursor: 0, symbol: null, tab: "signals",
      presets: loadPresets(options.storage), presetsPersisted: true,
    };
  }

  getState = (): LabState => this.state;

  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener);
    return () => void this.listeners.delete(listener);
  };

  private set(patch: Partial<LabState>): void {
    this.state = { ...this.state, ...patch };
    for (const listener of [...this.listeners]) listener();
  }

  // ---- the service, the catalog, the datasets ------------------------------------------------------------------------

  /** Reach the gateway and load what the engine says it can do. Safe to call again (Retry); the draft is kept. */
  connect = async (): Promise<void> => {
    const seq = ++this.connectSeq;
    this.set({ service: { phase: "checking" } });
    try {
      const status = await this.client.status();
      if (seq !== this.connectSeq) return;
      if (status.runner.state !== "ready") {
        this.set({ service: { phase: "no_runner", status } });
        return;
      }
      const [catalogText, listing] = await Promise.all([this.client.catalog(), this.client.datasets()]);
      if (seq !== this.connectSeq) return;
      const catalog = parseCatalog(catalogText);
      this.set({ service: { phase: "online", status }, catalog, datasets: listing.datasets, uploadLimit: listing.upload_limit_bytes });
      this.initialize();
      this.scheduleCheck(true);
    } catch (error) {
      if (seq !== this.connectSeq) return;
      const e = toLabError(error);
      this.set({ service: { phase: e.unreachable ? "offline" : "error", error: e } });
    }
  };

  /** Refresh only the runner identity (so a rebuilt engine marks the result stale). Failures are quiet. */
  pollStatus = async (): Promise<void> => {
    if (this.state.service.phase !== "online") return;
    try {
      const status = await this.client.status();
      if (this.state.service.phase === "online") this.set({ service: { phase: "online", status } });
    } catch (error) {
      if (toLabError(error).unreachable) this.set({ service: { phase: "offline", error: toLabError(error) } });
    }
  };

  private initialize(): void {
    if (this.initialized) return;
    this.initialized = true;
    const catalog = this.state.catalog!;
    const dataset = this.state.datasets.find((d) => d.available && d.key === "sma_crossover") ?? this.state.datasets.find((d) => d.available);
    const spec = availableStrategies(catalog).find((s) => s.kind === "sma_crossover") ?? availableStrategies(catalog)[0];
    if (!spec) return;
    const example = examplesFor(dataset, [spec])[0];
    this.set({
      dataset: { kind: "builtin", key: dataset?.key ?? "" },
      draft: example?.draft ?? newDraft(spec),
      origin: example ? { label: `Example: ${example.name}`, edited: false } : undefined,
    });
  }

  // ---- the draft -------------------------------------------------------------------------------------------------------

  selectKind = (kind: string): void => {
    const spec = specOf(this.state.catalog, kind);
    if (!spec?.available || kind === this.state.draft.kind) return;
    const fresh = newDraft(spec);
    const carried = Object.fromEntries(["symbols", "requested_quantity"].filter((n) => n in fresh.values && this.state.draft.values[n]).map((n) => [n, this.state.draft.values[n]!]));
    this.set({ draft: { kind, values: { ...fresh.values, ...carried } }, origin: undefined });
    this.scheduleCheck();
  };

  setValue = (name: string, text: string): void => {
    this.set({ draft: withValue(this.state.draft, name, text), origin: this.state.origin && { ...this.state.origin, edited: true } });
    this.scheduleCheck();
  };

  /** The engine's defaults for every parameter. The symbols allowlist (which has no default) is kept. */
  restoreDefaults = (): void => {
    const spec = specOf(this.state.catalog, this.state.draft.kind);
    if (!spec) return;
    const keep = this.state.draft.values.symbols;
    this.set({ draft: { kind: spec.kind, values: { ...defaultValues(spec), ...(keep !== undefined ? { symbols: keep } : {}) } }, origin: undefined });
    this.scheduleCheck();
  };

  /** Replace the draft with a saved preset, an example or a recorded run's settings: always an explicit action. */
  loadDraft = (draft: StrategyDraft, label: string): boolean => {
    const spec = specOf(this.state.catalog, draft.kind);
    if (!spec?.available) return false;
    const base = defaultValues(spec);
    const values = Object.fromEntries(Object.keys(base).map((k) => [k, draft.values[k] ?? base[k]!]));
    this.set({ draft: { kind: spec.kind, values }, origin: { label, edited: false } });
    this.scheduleCheck();
    return true;
  };

  // ---- the dataset ---------------------------------------------------------------------------------------------------

  selectBuiltin = (key: string): void => this.set({ dataset: { kind: "builtin", key }, uploadError: undefined });

  selectUpload = (): void => {
    if (this.state.upload) this.set({ dataset: { kind: "upload" }, uploadError: undefined });
  };

  uploadBytes = async (name: string, bytes: Uint8Array): Promise<boolean> => {
    const problem = precheckUpload(name, bytes, this.state.uploadLimit);
    if (problem) {
      this.set({ uploadError: problem });
      return false;
    }
    try {
      this.set({ upload: await describeUpload(name, bytes), dataset: { kind: "upload" }, uploadError: undefined });
      return true;
    } catch {
      this.set({ uploadError: "This browser could not fingerprint the file (it needs a secure context such as http://127.0.0.1)." });
      return false;
    }
  };

  uploadFile = async (file: File): Promise<boolean> => {
    if (file.size > this.state.uploadLimit) {
      // Refused before the bytes are even read into memory.
      this.set({ uploadError: `'${file.name}' is ${(file.size / 1048576).toFixed(1)} MiB; the limit is ${Math.floor(this.state.uploadLimit / 1048576)} MiB.` });
      return false;
    }
    return this.uploadBytes(file.name, new Uint8Array(await file.arrayBuffer()));
  };

  removeUpload = (): void => {
    const fallback = this.state.datasets.find((d) => d.available)?.key ?? "";
    this.set({ upload: undefined, uploadError: undefined, dataset: this.state.dataset.kind === "upload" ? { kind: "builtin", key: fallback } : this.state.dataset });
  };

  // ---- the engine check -----------------------------------------------------------------------------------------------

  /** Ask the real strategy constructors about the draft (debounced), unless the draft already fails the client checks. */
  private scheduleCheck(immediate = false): void {
    if (this.checkTimer) clearTimeout(this.checkTimer);
    const spec = specOf(this.state.catalog, this.state.draft.kind);
    const seq = ++this.checkSeq;
    if (!spec || validateDraft(spec, this.state.draft).length) {
      this.set({ check: { phase: "idle" } });
      return;
    }
    const request = buildRequest(spec, this.state.draft);
    const key = JSON.stringify(request);
    if (this.state.service.phase !== "online") {
      this.set({ check: { phase: "unavailable", key, error: this.state.service.error ?? new LabError("service_unavailable", "The Strategy Lab service is not reachable.") } });
      return;
    }
    this.set({ check: { phase: "checking", key } });
    this.checkTimer = setTimeout(async () => {
      try {
        const result = await this.client.check(request);
        if (seq === this.checkSeq) this.set({ check: { phase: "ok", key, result } });
      } catch (error) {
        if (seq !== this.checkSeq) return;
        const e = toLabError(error);
        this.set({ check: e.kind === "runner_rejected" ? { phase: "rejected", key, error: e } : { phase: "unavailable", key, error: e } });
      }
    }, immediate ? 0 : this.debounceMs);
  }

  // ---- the replay ------------------------------------------------------------------------------------------------------

  /** THE only place an engine replay starts. Returns true when a new result was stored. */
  runReplay = async (): Promise<boolean> => {
    const s = this.state;
    if (runBlocker(s)) return false;
    const spec = specOf(s.catalog, s.draft.kind)!;
    const request = currentRequest(s)!;
    const dataset: ReplayRequest["dataset"] = s.dataset.kind === "upload" ? { name: s.upload!.name, csv_base64: s.upload!.base64 } : { builtin: s.dataset.key };
    const token = s.run.token + 1;
    const launch = s.launches + 1;
    const controller = new AbortController();
    this.runAbort = controller;
    this.set({ run: { phase: "running", token }, launches: launch });
    try {
      const response = await this.client.replay({ strategy: { kind: request.kind, params: request.params }, dataset }, controller.signal);
      if (this.state.run.token !== token) return false; // cancelled or superseded: never replaces a newer state
      const snapshot = makeSnapshot(launch, request, spec.title, response, this.now());
      this.set({ snapshot, run: { phase: "idle", token }, cursor: 0, symbol: null });
      return true;
    } catch (error) {
      if (this.state.run.token !== token) return false;
      const e = toLabError(error);
      this.set({ run: { phase: "failed", token, error: e, datasetName: request.dataset.name }, ...(e.unreachable ? { service: { phase: "offline" as const, error: e } } : {}) });
      return false;
    } finally {
      if (this.runAbort === controller) this.runAbort = undefined;
    }
  };

  cancelRun = (): void => {
    if (this.state.run.phase !== "running") return;
    this.runAbort?.abort();
    this.set({ run: { phase: "idle", token: this.state.run.token + 1, cancelled: true } });
  };

  dismissRunError = (): void => {
    if (this.state.run.phase === "failed") this.set({ run: { phase: "idle", token: this.state.run.token } });
  };

  // ---- the replay position (never starts the engine) ------------------------------------------------------------------

  setCursor = (cursor: number): void => {
    const snap = this.state.snapshot;
    if (snap) this.set({ cursor: clamp(snap.model, cursor) });
  };

  step = (direction: "next" | "previous" | "next_signal" | "reset"): void => {
    const { snapshot: snap, cursor, symbol } = this.state;
    if (!snap) return;
    const target =
      direction === "reset" ? 0
      : direction === "next" ? nextEvent(snap.model, cursor, symbol)
      : direction === "previous" ? previousEvent(snap.model, cursor, symbol)
      : nextSignal(snap.model, cursor, symbol);
    if (target !== undefined) this.set({ cursor: target });
  };

  setSymbol = (symbol: string | null): void => this.set({ symbol });
  setTab = (tab: ResultTab): void => this.set({ tab });

  // ---- presets ---------------------------------------------------------------------------------------------------------

  savePreset = (name: string): { ok: boolean; persisted: boolean; message?: string } => this.savePresetDraft(name, this.state.draft);

  /** Save any draft as a named preset (Compare saves the A or B configuration through here, so one list serves both views). */
  savePresetDraft = (name: string, draft: StrategyDraft): { ok: boolean; persisted: boolean; message?: string } => {
    const clean = name.trim();
    if (!clean || clean.length > 60) return { ok: false, persisted: false, message: "Give the preset a name of 1 to 60 characters." };
    const entry: Preset = { id: presetId(clean, Date.parse(this.now()) || 0), name: clean, savedAt: this.now(), draft };
    const others = this.state.presets.filter((p) => p.name !== clean);
    const presets = [entry, ...others].slice(0, 50);
    const persisted = savePresets(presets, this.storage);
    this.set({ presets, presetsPersisted: persisted });
    return { ok: true, persisted };
  };

  deletePreset = (id: string): void => {
    const presets = this.state.presets.filter((p) => p.id !== id);
    this.set({ presets, presetsPersisted: savePresets(presets, this.storage) });
  };

  loadPreset = (id: string): boolean => {
    const preset = this.state.presets.find((p) => p.id === id);
    return preset ? this.loadDraft(preset.draft, `Preset: ${preset.name}`) : false;
  };
}

export function toLabError(error: unknown): LabError {
  if (error instanceof LabError) return error;
  if (error instanceof DOMException && error.name === "AbortError") return new LabError("cancelled", "The request was cancelled.");
  return new LabError("internal", error instanceof Error ? error.message : String(error));
}

export const specFor = (state: LabState): StrategySpec | undefined => specOf(state.catalog, state.draft.kind);
