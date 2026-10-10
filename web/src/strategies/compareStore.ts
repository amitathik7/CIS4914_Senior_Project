// The Compare view's store: two independent configurations (A and B) over ONE shared dataset, and the comparison they produce.
// Framework-free (React reads it through useCompare.ts), so every rule below is unit-tested with a fake client.
//
// Rules this file exists to enforce (the same spirit as store.ts, extended to two sides):
//   * Only runComparison() starts the engine, and it starts exactly two replays (one per side, each in its own fresh engine run with
//     its own strategy state). Stepping, filtering, tabs, downloads and editing never do (state.launches counts launches).
//   * A comparison is an immutable snapshot of two results. Editing a configuration, the dataset or the symbols marks it stale and
//     changes nothing else; a failed launch keeps the last good comparison.
//   * One comparison at a time; a response that belongs to an older launch (cancelled, superseded) is dropped. If only one side
//     finishes, that is shown as exactly that: there is no comparison, and the finished side is not presented as half of one.
//   * Both sides share the dataset and the symbol allowlist; everything else about a side is its own.
//   * The service, catalog, dataset list and presets are the lab store's: Compare reads them and never changes them.

import { LabError, type LabClient, type ReplayRequest } from "../api/lab";
import { MEMBERS, OTHER, buildCompareModel, nextDifference, nextSignalEither, type DiffMode, type Member } from "./compare";
import {
  compareBlocker, memberRequest, memberSpec, type Attempt, type CompareOwnState, type CompareState, type CompareTab, type ComparisonSnapshot,
  type MemberAttempt, type MemberState,
} from "./compareState";
import { planQuickStart, type QuickStartId } from "./compareStarts";
import { clamp, nextEvent, previousEvent } from "./cursor";
import { buildRequest, defaultValues, newDraft, requestKey, validateDraft, withValue, type StrategyDraft } from "./draft";
import { makeSnapshot } from "./snapshot";
import { toLabError, type LabStore } from "./store";
import { describeUpload, precheckUpload, sha256Hex } from "./uploads";
import { availableStrategies, specOf } from "./catalog";

/** What Compare needs from the lab store: its state (service, catalog, datasets, presets) and its preset list. */
export type LabDirectory = Pick<LabStore, "getState" | "subscribe" | "savePresetDraft" | "deletePreset" | "pollStatus">;

export interface CompareOptions {
  now?: () => string;
  checkDebounceMs?: number;
}

const BLANK: StrategyDraft = { kind: "", values: {} };
const blankMember = (): MemberState => ({ draft: BLANK, check: { phase: "idle" } });

const encode = (text: string): Uint8Array => new TextEncoder().encode(text);

export class CompareStore {
  private own: CompareOwnState;
  private view: CompareState;
  private readonly listeners = new Set<() => void>();
  private aborts?: Record<Member, AbortController>;
  private readonly timers: Partial<Record<Member, ReturnType<typeof setTimeout>>> = {};
  private readonly checkSeq: Record<Member, number> = { A: 0, B: 0 };
  private serviceWasOnline = false;
  private readonly now: () => string;
  private readonly debounceMs: number;

  constructor(private readonly client: LabClient, private readonly lab: LabDirectory, options: CompareOptions = {}) {
    this.now = options.now ?? (() => new Date().toISOString());
    this.debounceMs = options.checkDebounceMs ?? 350;
    this.own = {
      initialized: false, members: { A: blankMember(), B: blankMember() }, dataset: { kind: "builtin", key: "" },
      run: { phase: "idle", token: 0 }, launches: 0, cursor: 0, symbol: null, tab: "signals", diffMode: "requests",
    };
    this.view = { ...this.own, lab: lab.getState() };
    lab.subscribe(this.onLab);
    this.onLab();
  }

  getState = (): CompareState => this.view;

  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener);
    return () => void this.listeners.delete(listener);
  };

  private publish(): void {
    this.view = { ...this.own, lab: this.lab.getState() };
    for (const listener of [...this.listeners]) listener();
  }

  private set(patch: Partial<CompareOwnState>): void {
    this.own = { ...this.own, ...patch };
    this.publish();
  }

  private setMember(member: Member, patch: Partial<MemberState>): void {
    this.set({ members: { ...this.own.members, [member]: { ...this.own.members[member], ...patch } } });
  }

  // ---- following the lab store -------------------------------------------------------------------------------------------

  private onLab = (): void => {
    const lab = this.lab.getState();
    const online = lab.service.phase === "online";
    const justOnline = online && !this.serviceWasOnline;
    this.serviceWasOnline = online;
    if (!this.own.initialized && lab.catalog && lab.draft.kind !== "") {
      this.initialize();
      return;
    }
    this.publish();
    if (justOnline && this.own.initialized) for (const m of MEMBERS) this.scheduleCheck(m, true);
  };

  /** Start from what Configure holds: A is a copy of its draft (independent from here on), B a copy of A. Nothing runs. */
  private initialize(): void {
    const lab = this.lab.getState();
    const spec = specOf(lab.catalog, lab.draft.kind);
    const a: StrategyDraft = spec?.available ? { kind: lab.draft.kind, values: { ...lab.draft.values } } : newDraft(availableStrategies(lab.catalog)[0]!);
    this.own = {
      ...this.own, initialized: true, dataset: lab.dataset, ...(lab.upload ? { upload: lab.upload } : {}),
      members: {
        A: { draft: a, ...(lab.origin ? { origin: { ...lab.origin } } : {}), check: { phase: "idle" } },
        B: { draft: { ...a, values: { ...a.values } }, origin: { label: "Copied from A", edited: false }, check: { phase: "idle" } },
      },
    };
    this.publish();
    this.serviceWasOnline = lab.service.phase === "online";
    for (const m of MEMBERS) this.scheduleCheck(m, true);
  }

  // ---- the two drafts ----------------------------------------------------------------------------------------------------

  private edited(member: Member): MemberState["origin"] {
    const origin = this.own.members[member].origin;
    return origin && { ...origin, edited: true };
  }

  selectKind = (member: Member, kind: string): void => {
    const spec = specOf(this.lab.getState().catalog, kind);
    const current = this.own.members[member].draft;
    if (!spec?.available || kind === current.kind) return;
    const fresh = newDraft(spec);
    const carried = Object.fromEntries(["symbols", "requested_quantity"].filter((n) => n in fresh.values && current.values[n]).map((n) => [n, current.values[n]!]));
    this.setMember(member, { draft: { kind, values: { ...fresh.values, ...carried } }, origin: undefined });
    this.scheduleCheck(member);
  };

  /** Editing the shared allowlist edits both sides: one symbol universe per comparison. */
  setSymbols = (text: string): void => {
    const members = Object.fromEntries(MEMBERS.map((m) => [m, { ...this.own.members[m], draft: withValue(this.own.members[m].draft, "symbols", text), origin: this.edited(m) }])) as CompareOwnState["members"];
    this.set({ members });
    for (const m of MEMBERS) this.scheduleCheck(m);
  };

  setValue = (member: Member, name: string, text: string): void => {
    if (name === "symbols") return this.setSymbols(text);
    this.setMember(member, { draft: withValue(this.own.members[member].draft, name, text), origin: this.edited(member) });
    this.scheduleCheck(member);
  };

  /** The engine's defaults for every parameter. The shared allowlist is kept. */
  restoreDefaults = (member: Member): void => {
    const spec = memberSpec(this.view, member);
    if (!spec) return;
    const keep = this.own.members[member].draft.values.symbols;
    this.setMember(member, { draft: { kind: spec.kind, values: { ...defaultValues(spec), ...(keep !== undefined ? { symbols: keep } : {}) } }, origin: undefined });
    this.scheduleCheck(member);
  };

  /** Replace one side with a preset, an example or a recorded run's settings (always an explicit action). The shared allowlist wins. */
  loadDraft = (member: Member, draft: StrategyDraft, label: string): boolean => {
    const spec = specOf(this.lab.getState().catalog, draft.kind);
    if (!spec?.available) return false;
    const base = defaultValues(spec);
    const shared = this.own.members.A.draft.values.symbols ?? "";
    const values = Object.fromEntries(Object.keys(base).map((k) => [k, draft.values[k] ?? base[k]!]));
    // With no allowlist yet the loaded one becomes the shared one; otherwise the shared one wins.
    const adopt = shared.trim() === "" && (values.symbols ?? "").trim() !== "";
    if (!adopt && "symbols" in values) values.symbols = shared;
    const members = { ...this.own.members, [member]: { draft: { kind: spec.kind, values }, origin: { label, edited: false }, check: { phase: "idle" as const } } };
    if (adopt) {
      const other = OTHER[member];
      members[other] = { ...members[other], draft: withValue(members[other].draft, "symbols", values.symbols!) };
    }
    this.set({ members });
    this.scheduleCheck(member);
    if (adopt) this.scheduleCheck(OTHER[member]);
    return true;
  };

  copyAToB = (): void => {
    const a = this.own.members.A.draft;
    this.setMember("B", { draft: { kind: a.kind, values: { ...a.values } }, origin: { label: "Copied from A", edited: false } });
    this.scheduleCheck("B");
  };

  quickStart = (id: QuickStartId): { ok: boolean; message?: string } => {
    const lab = this.lab.getState();
    const dataset = this.own.dataset.kind === "builtin" ? lab.datasets.find((d) => d.key === (this.own.dataset as { key: string }).key) : undefined;
    const plan = planQuickStart(id, lab.catalog, dataset);
    if ("error" in plan) return { ok: false, message: plan.error };
    const kept = this.own.members.A.draft.values.symbols ?? "";
    const shared = kept.trim() ? kept : plan.A.values.symbols ?? "";
    const withShared = (d: StrategyDraft): StrategyDraft => ({ ...d, values: { ...d.values, symbols: shared } });
    this.set({
      members: {
        A: { draft: withShared(plan.A), origin: { label: plan.labelA, edited: false }, check: { phase: "idle" } },
        B: { draft: withShared(plan.B), origin: { label: plan.labelB, edited: false }, check: { phase: "idle" } },
      },
    });
    for (const m of MEMBERS) this.scheduleCheck(m, true);
    return { ok: true };
  };

  loadPreset = (member: Member, id: string): boolean => {
    const preset = this.lab.getState().presets.find((p) => p.id === id);
    return preset ? this.loadDraft(member, preset.draft, `Preset: ${preset.name}`) : false;
  };

  savePreset = (member: Member, name: string) => this.lab.savePresetDraft(name, this.own.members[member].draft);
  deletePreset = (id: string): void => this.lab.deletePreset(id);

  // ---- the shared dataset ------------------------------------------------------------------------------------------------

  selectBuiltin = (key: string): void => this.set({ dataset: { kind: "builtin", key }, uploadError: undefined });

  selectUpload = (): void => {
    if (this.own.upload) this.set({ dataset: { kind: "upload" }, uploadError: undefined });
  };

  uploadBytes = async (name: string, bytes: Uint8Array): Promise<boolean> => {
    const problem = precheckUpload(name, bytes, this.lab.getState().uploadLimit);
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
    const limit = this.lab.getState().uploadLimit;
    if (file.size > limit) {
      this.set({ uploadError: `'${file.name}' is ${(file.size / 1048576).toFixed(1)} MiB; the limit is ${Math.floor(limit / 1048576)} MiB.` });
      return false;
    }
    return this.uploadBytes(file.name, new Uint8Array(await file.arrayBuffer()));
  };

  removeUpload = (): void => {
    const fallback = this.lab.getState().datasets.find((d) => d.available)?.key ?? "";
    this.set({ upload: undefined, uploadError: undefined, dataset: this.own.dataset.kind === "upload" ? { kind: "builtin", key: fallback } : this.own.dataset });
  };

  // ---- the engine check, per side ----------------------------------------------------------------------------------------

  private scheduleCheck(member: Member, immediate = false): void {
    const pending = this.timers[member];
    if (pending) clearTimeout(pending);
    const seq = ++this.checkSeq[member];
    const spec = memberSpec(this.view, member);
    const draft = this.own.members[member].draft;
    if (!spec || validateDraft(spec, draft).length) {
      this.setMember(member, { check: { phase: "idle" } });
      return;
    }
    const request = buildRequest(spec, draft);
    const key = JSON.stringify(request);
    const service = this.lab.getState().service;
    if (service.phase !== "online") {
      this.setMember(member, { check: { phase: "unavailable", key, error: service.error ?? new LabError("service_unavailable", "The Strategy Lab service is not reachable.") } });
      return;
    }
    this.setMember(member, { check: { phase: "checking", key } });
    this.timers[member] = setTimeout(async () => {
      try {
        const result = await this.client.check(request);
        if (seq === this.checkSeq[member]) this.setMember(member, { check: { phase: "ok", key, result } });
      } catch (error) {
        if (seq !== this.checkSeq[member]) return;
        const e = toLabError(error);
        this.setMember(member, { check: e.kind === "runner_rejected" ? { phase: "rejected", key, error: e } : { phase: "unavailable", key, error: e } });
      }
    }, immediate ? 0 : this.debounceMs);
  }

  // ---- the comparison ----------------------------------------------------------------------------------------------------

  private patchAttempt(token: number, member: Member, patch: Partial<MemberAttempt>): void {
    const attempt = this.own.attempt;
    if (this.own.run.token !== token || !attempt) return; // cancelled or superseded: never touches a newer state
    this.set({ attempt: { ...attempt, members: { ...attempt.members, [member]: { ...attempt.members[member], ...patch } } } });
  }

  /** THE only place the engine starts. Returns true when a complete comparison was stored. */
  runComparison = async (): Promise<boolean> => {
    if (compareBlocker(this.view)) return false;
    const requests = { A: memberRequest(this.view, "A")!, B: memberRequest(this.view, "B")! };
    const dataset = this.own.dataset;
    const ref: ReplayRequest["dataset"] = dataset.kind === "upload" ? { name: this.own.upload!.name, csv_base64: this.own.upload!.base64 } : { builtin: dataset.key };
    const token = this.own.run.token + 1;
    const launch = this.own.launches + 1;
    const controllers = { A: new AbortController(), B: new AbortController() };
    this.aborts = controllers;
    const titles = { A: memberSpec(this.view, "A")?.title ?? requests.A.kind, B: memberSpec(this.view, "B")?.title ?? requests.B.kind };
    const member = (m: Member): MemberAttempt => ({ phase: "running", request: requests[m], title: titles[m] });
    this.set({ run: { phase: "running", token }, launches: launch, attempt: { launch, startedAt: this.now(), members: { A: member("A"), B: member("B") }, outcome: "running" } });

    await Promise.all(MEMBERS.map(async (m) => {
      try {
        const response = await this.client.replay({ strategy: { kind: requests[m].kind, params: requests[m].params }, dataset: ref }, controllers[m].signal);
        if (this.own.run.token !== token) return;
        this.patchAttempt(token, m, { phase: "done", snapshot: makeSnapshot(launch, requests[m], titles[m], response, this.now()) });
      } catch (error) {
        const e = toLabError(error);
        this.patchAttempt(token, m, { phase: "failed", error: e });
        if (e.unreachable) void this.lab.pollStatus();
      }
    }));
    if (this.own.run.token !== token) return false;
    return this.finish(token, launch);
  };

  private async finish(token: number, launch: number): Promise<boolean> {
    const attempt = this.own.attempt!;
    const done = MEMBERS.filter((m) => attempt.members[m].phase === "done");
    const settle = (patch: Partial<Attempt>): boolean => {
      this.set({ run: { phase: "idle", token }, attempt: { ...this.own.attempt!, ...patch } });
      return false;
    };
    if (done.length === 0) return settle({ outcome: "failed" });
    if (done.length === 1) return settle({ outcome: "partial" });
    const a = attempt.members.A.snapshot!;
    const b = attempt.members.B.snapshot!;
    try {
      const model = buildCompareModel(a, b);
      const runner = a.meta.runner.sha256;
      const configIds = { A: await sha256Hex(encode(requestKey(a.request))), B: await sha256Hex(encode(requestKey(b.request))) };
      const comparisonId = await sha256Hex(encode(JSON.stringify([configIds.A, configIds.B, runner])));
      if (this.own.run.token !== token) return false;
      const comparison: ComparisonSnapshot = Object.freeze({
        id: launch, finishedAt: this.now(), snapshots: { A: a, B: b }, configIds, comparisonId, datasetSha256: a.doc.dataset.sha256, runnerSha256: runner, model,
      });
      this.set({ run: { phase: "idle", token }, attempt: { ...attempt, outcome: "complete" }, comparison, cursor: 0, symbol: null });
      return true;
    } catch (error) {
      if (this.own.run.token !== token) return false;
      return settle({ outcome: "mismatch", mismatch: toLabError(error) });
    }
  }

  cancelRun = (): void => {
    if (this.own.run.phase !== "running") return;
    for (const c of Object.values(this.aborts ?? {})) c.abort();
    this.aborts = undefined;
    this.set({ run: { phase: "idle", token: this.own.run.token + 1, cancelled: true }, attempt: undefined });
  };

  /** Hide a failure notice (the last complete comparison, if any, stays). */
  dismissAttempt = (): void => {
    if (this.own.attempt && this.own.attempt.outcome !== "running" && this.own.attempt.outcome !== "complete") this.set({ attempt: undefined });
  };

  // ---- the shared replay position (never starts the engine) --------------------------------------------------------------

  setCursor = (cursor: number): void => {
    const done = this.own.comparison;
    if (done) this.set({ cursor: clamp(done.model.members.A, cursor) });
  };

  step = (direction: "next" | "previous" | "next_signal" | "next_difference" | "reset"): void => {
    const done = this.own.comparison;
    if (!done) return;
    const { model } = done;
    const { cursor, symbol, diffMode } = this.own;
    const target =
      direction === "reset" ? 0
      : direction === "next" ? nextEvent(model.members.A, cursor, symbol)
      : direction === "previous" ? previousEvent(model.members.A, cursor, symbol)
      : direction === "next_signal" ? nextSignalEither(model, cursor, symbol)
      : nextDifference(model, cursor, symbol, diffMode);
    if (target !== undefined) this.set({ cursor: target });
  };

  setSymbol = (symbol: string | null): void => this.set({ symbol });
  setTab = (tab: CompareTab): void => this.set({ tab });
  setDiffMode = (diffMode: DiffMode): void => this.set({ diffMode });
}
