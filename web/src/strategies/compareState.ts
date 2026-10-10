// What the Compare view holds, and the pure questions asked of it (can it run, is the result still current). The store that changes
// this state is compareStore.ts; the lab store supplies the service, the catalog and the datasets (shared, read-only here).

import type { LabError } from "../api/lab";
import { specOf, type StrategySpec } from "./catalog";
import type { CompareModel, DiffMode, Member } from "./compare";
import { MEMBERS } from "./compare";
import {
  buildRequest, describeChanges, requestKey, validateDraft, type DatasetIdentity, type FieldIssue, type RequestSnapshot, type StrategyDraft,
} from "./draft";
import type { Snapshot } from "./snapshot";
import { datasetIdentity, serviceBlocker, type CheckState, type DatasetSelection, type LabState } from "./store";
import type { UploadedFile } from "./uploads";

export type CompareTab = "signals" | "events" | "details";

export interface MemberState {
  draft: StrategyDraft;
  /** Where the draft came from, for a one-line caption; edited once the person changes it. */
  origin?: { label: string; edited: boolean };
  check: CheckState;
}

/** One complete comparison: two independent replays of the same dataset, paired. Immutable; editing a draft only makes it stale. */
export interface ComparisonSnapshot {
  /** Which comparison launch of this tab produced it (1, 2, 3...). */
  readonly id: number;
  readonly finishedAt: string;
  readonly snapshots: Readonly<Record<Member, Snapshot>>;
  /** SHA-256 of the exact request that was sent for each side: that side's configuration identity. */
  readonly configIds: Readonly<Record<Member, string>>;
  /** SHA-256 over both configuration identities and the engine file: identifies the INPUTS of the comparison. */
  readonly comparisonId: string;
  readonly datasetSha256: string;
  readonly runnerSha256: string;
  readonly model: CompareModel;
}

export interface MemberAttempt {
  phase: "running" | "done" | "failed";
  /** What was sent for this side. */
  request: RequestSnapshot;
  title: string;
  error?: LabError;
  snapshot?: Snapshot;
}

export interface Attempt {
  launch: number;
  startedAt: string;
  members: Readonly<Record<Member, MemberAttempt>>;
  /** complete: both sides finished and describe the same events. partial: exactly one side finished. failed: neither did.
   *  mismatch: both finished but are not the same recorded run, so they are not paired. */
  outcome: "running" | "complete" | "partial" | "failed" | "mismatch";
  mismatch?: LabError;
}

export interface CompareOwnState {
  initialized: boolean;
  members: Readonly<Record<Member, MemberState>>;
  dataset: DatasetSelection;
  upload?: UploadedFile;
  uploadError?: string;
  run: { phase: "idle" | "running"; token: number; cancelled?: boolean };
  /** Comparison launches in this browser tab (two engine replays each). The engine check is separate. */
  launches: number;
  /** The latest launch, whatever came of it. */
  attempt?: Attempt;
  /** The last COMPLETE comparison. Kept (and marked stale) when a later launch fails. */
  comparison?: ComparisonSnapshot;
  cursor: number;
  /** Display filter: a symbol, or null for all. Never an input to either strategy. */
  symbol: string | null;
  tab: CompareTab;
  diffMode: DiffMode;
}

export type CompareState = CompareOwnState & { lab: LabState };

// ---- selectors ---------------------------------------------------------------------------------------------------------

export const memberSpec = (state: CompareState, member: Member): StrategySpec | undefined => specOf(state.lab.catalog, state.members[member].draft.kind);

export function memberIssues(state: CompareState, member: Member): FieldIssue[] {
  const spec = memberSpec(state, member);
  return spec ? validateDraft(spec, state.members[member].draft) : [];
}

/** The engine check that belongs to the draft on screen (an older answer is ignored). */
export function memberCheck(state: CompareState, member: Member): CheckState {
  const spec = memberSpec(state, member);
  const check = state.members[member].check;
  if (!spec || check.phase === "idle") return { phase: "idle" };
  return check.key === JSON.stringify(buildRequest(spec, state.members[member].draft)) ? check : { phase: "idle" };
}

export const compareDataset = (state: CompareState): DatasetIdentity | undefined =>
  datasetIdentity({ dataset: state.dataset, datasets: state.lab.datasets, upload: state.upload });

/** The allowlist both sides share (one symbol universe per comparison). */
export const sharedSymbols = (state: CompareState): string => state.members.A.draft.values.symbols ?? "";

export function memberRequest(state: CompareState, member: Member): RequestSnapshot | undefined {
  const spec = memberSpec(state, member);
  const dataset = compareDataset(state);
  return spec && dataset ? { ...buildRequest(spec, state.members[member].draft), dataset } : undefined;
}

/** Why Run comparison is unavailable right now, in a sentence; undefined when it can start. */
export function compareBlocker(state: CompareState): string | undefined {
  if (state.run.phase === "running") return "A comparison is already running.";
  const service = serviceBlocker(state.lab.service);
  if (service) return service;
  if (!state.initialized) return "Setting up the two configurations…";
  if (!compareDataset(state)) return state.dataset.kind === "upload" ? "Choose a CSV file." : "Choose a dataset.";
  for (const member of MEMBERS) {
    const spec = memberSpec(state, member);
    if (!spec) return `Choose a strategy for ${member}.`;
    if (!spec.available) return `${member}: ${spec.unavailableReason || "this strategy is not available."}`;
    if (memberIssues(state, member).length) return `Fix the highlighted settings in ${member} first.`;
    const check = memberCheck(state, member);
    if (check.phase === "rejected") return `The engine rejected configuration ${member}: ${check.error.message}`;
  }
  return undefined;
}

export interface CompareStatus {
  phase: "idle" | "running";
  /** none: no comparison yet. current: the settings on screen are the ones that produced it. stale: they are not. */
  relation: "none" | "current" | "stale";
  /** Why a result is stale, per side; `shared` is the dataset and the engine file. */
  reasons: { shared: string[]; A: string[]; B: string[] };
}

export function compareStatus(state: CompareState): CompareStatus {
  const phase = state.run.phase;
  const done = state.comparison;
  const reasons = { shared: [] as string[], A: [] as string[], B: [] as string[] };
  if (!done) return { phase, relation: "none", reasons };
  const dataset = compareDataset(state);
  if (!dataset) reasons.shared.push("The dataset is being changed");
  else if (dataset.sha256 !== done.datasetSha256) reasons.shared.push(`Dataset: ${done.snapshots.A.request.dataset.name} → ${dataset.name}`);
  const runner = state.lab.service.status?.runner;
  if (runner?.state === "ready" && runner.sha256 !== done.runnerSha256) reasons.shared.push("The replay engine was rebuilt since this result");
  let changed = reasons.shared.length > 0;
  for (const member of MEMBERS) {
    const before = done.snapshots[member].request;
    const now = memberRequest(state, member);
    if (!now) {
      reasons[member].push("Settings are being edited");
      changed = true;
      continue;
    }
    // The dataset is reported once (above), so compare each side's own settings against the dataset it ran on.
    const same = { ...now, dataset: before.dataset };
    const list = describeChanges(before, same, (kind) => specOf(state.lab.catalog, kind));
    reasons[member].push(...list);
    if (requestKey(same) !== requestKey(before)) changed = true;
  }
  return { phase, relation: changed ? "stale" : "current", reasons };
}
