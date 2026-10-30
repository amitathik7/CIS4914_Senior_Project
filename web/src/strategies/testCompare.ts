// Test helper (no production code imports this): paired replay fixtures for Compare, built from hand-written event specs.

import { vi } from "vitest";
import type { ReplayRequest, ReplayResponse } from "../api/lab";
import { buildCompareModel } from "./compare";
import type { ComparisonSnapshot } from "./compareState";
import type { RequestSnapshot } from "./draft";
import { makeSnapshot } from "./snapshot";
import { CompareStore } from "./compareStore";
import { LabStore } from "./store";
import { FakeClient, MemoryStorage, OTHER_SHA, replayResponse, SHA } from "./testClient";
import { makeDoc, type Spec } from "./testDoc";

export function snap(specs: Spec[], options: { kind?: string; sha?: string; runId?: string; params?: [string, string][] } = {}) {
  const kind = options.kind ?? "sma_crossover";
  const sha = options.sha ?? SHA;
  const request: RequestSnapshot = { kind, params: options.params ?? [["strategy_id", kind]], dataset: { kind: "builtin", name: "x.csv", sha256: sha } };
  const response = replayResponse({ strategy: { kind, params: request.params }, dataset: { builtin: "x" } }, sha, specs);
  const raw = options.runId ? makeDoc(specs, { datasetSha: sha, kind, runId: options.runId }) : response.raw;
  return makeSnapshot(1, request, "Title", { ...response, raw }, "2026-10-09T12:00:00Z");
}

export const warm = (fill: number, size: number): Pick<Spec, "verdict" | "reason" | "window"> => ({ verdict: "warming_up", reason: "warming_up", window: { fill, remaining: size - fill, size } });
export const ready = (size: number): Pick<Spec, "window"> => ({ window: { fill: size, remaining: 0, size } });

/**
 * Nine AAPL bars. A (window 3) is ready from bar 3 and requests Buy at event 5 and Sell at event 8 (1-based); B (window 5) is ready
 * from bar 5 and requests Buy at event 6. Both sides' first request has raw signal id "1".
 */
export function nineBars(): { a: Spec[]; b: Spec[] } {
  const a: Spec[] = Array.from({ length: 9 }, (_, i) => ({
    symbol: "AAPL", minute: i, price: String(i + 1),
    ...(i < 2 ? warm(i + 1, 3) : ready(3)),
    ...(i === 4 ? { signals: [[0, "1", "buy"]] as [number, string, string][], reason: "crossover_buy" } : {}),
    ...(i === 7 ? { signals: [[0, "2", "sell"]] as [number, string, string][], reason: "crossover_sell" } : {}),
  }));
  const b: Spec[] = Array.from({ length: 9 }, (_, i) => ({
    symbol: "AAPL", minute: i, price: String(i + 1),
    ...(i < 4 ? warm(i + 1, 5) : ready(5)),
    ...(i === 5 ? { signals: [[0, "1", "buy"]] as [number, string, string][], reason: "crossover_buy" } : {}),
  }));
  return { a, b };
}

/** A finished comparison of two spec lists, built the way the store builds one (without running a store). */
export function comparisonOf(a: Spec[], b: Spec[], options: { runIdA?: string; runIdB?: string } = {}): ComparisonSnapshot {
  const crossover = (short: string, long: string): [string, string][] => [["strategy_id", "sma_crossover"], ["short_window", short], ["long_window", long]];
  const first = snap(a, { params: crossover("2", "3"), ...(options.runIdA ? { runId: options.runIdA } : {}) });
  const second = snap(b, { params: crossover("3", "5"), ...(options.runIdB ? { runId: options.runIdB } : {}) });
  return Object.freeze({
    id: 1, finishedAt: "2026-10-09T12:00:00.000Z", snapshots: { A: first, B: second }, configIds: { A: "1".repeat(64), B: "2".repeat(64) },
    comparisonId: "3".repeat(64), datasetSha256: SHA, runnerSha256: "c3".repeat(32), model: buildCompareModel(first, second),
  });
}

// ---- a connected Compare store over a fake gateway (shared by the store test files) ----------------------------------------------

export const NOW = "2026-10-09T12:00:00.000Z";
export const windowOf = (request: ReplayRequest): string | undefined => request.strategy.params.find(([n]) => n === "short_window")?.[1];

/** A replay answer whose rows depend on the side: the configuration with short_window 2 is "A", anything else is "B". */
export function sidesAnswer(request: ReplayRequest, a: Spec[] = nineBars().a, b: Spec[] = nineBars().b): ReplayResponse {
  const sha = "builtin" in request.dataset && request.dataset.builtin === "mr_rearm" ? OTHER_SHA : SHA;
  return replayResponse(request, sha, windowOf(request) === "2" ? a : b);
}

/** Call inside a test with fake timers: a lab store and a Compare store over one FakeClient, connected, with the engine checks settled. */
export async function connectCompare(): Promise<{ client: FakeClient; lab: LabStore; store: CompareStore }> {
  const client = new FakeClient();
  client.replayImpl = async (request) => sidesAnswer(request);
  const lab = new LabStore(client, { storage: new MemoryStorage(), now: () => NOW, checkDebounceMs: 0 });
  const store = new CompareStore(client, lab, { now: () => NOW, checkDebounceMs: 0 });
  await lab.connect();
  await vi.runAllTimersAsync();
  return { client, lab, store };
}
