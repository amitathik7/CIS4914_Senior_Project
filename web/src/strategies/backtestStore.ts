// The Backtest view's state: the simulation/risk settings and the last full backtest, kept apart from the replay store so a
// backtest never replaces the replay on screen (and the other way round). The strategy, its parameters and the dataset are
// the shared draft in the replay store: Configure, Explore and Backtest edit the same configuration.
//
// Rules, as for replays: only run() starts the engine; one backtest at a time; an answer for an older launch is dropped;
// a result is never edited, it only becomes "stale" when the configuration on screen no longer matches it.

import type { BacktestMeta, LabClient } from "../api/lab";
import { parseBacktest, settingIssues, toSettings, DEFAULT_SETTINGS, type BacktestDoc, type SettingName, type SettingsDraft } from "./backtest";
import { specOf } from "./catalog";
import type { RequestSnapshot } from "./draft";
import { currentRequest, runBlocker, toLabError, type LabState } from "./store";
import type { LabError } from "../api/lab";

export interface BacktestResult {
  id: number;
  doc: BacktestDoc;
  meta: BacktestMeta;
  title: string;
  request: RequestSnapshot;
  /** Identity of what was run: the request and the settings. */
  key: string;
  at: string;
}

export interface BacktestState {
  settings: SettingsDraft;
  run: { phase: "idle" | "running" | "failed"; token: number; error?: LabError };
  result?: BacktestResult;
  launches: number;
}

const keyOf = (request: RequestSnapshot, settings: SettingsDraft) => JSON.stringify([request.kind, request.params, request.dataset.sha256, toSettings(settings)]);

/** Why Run backtest is unavailable, in a sentence; undefined when it can start. */
export function backtestBlocker(lab: LabState, bt: BacktestState): string | undefined {
  if (bt.run.phase === "running") return "A backtest is already running.";
  const replay = runBlocker(lab);
  if (replay) return replay.replace("replayed", "backtested");
  const tool = lab.service.status?.backtest;
  if (!tool) return "This Strategy Lab service is older than the console and has no backtest endpoint. Restart it from the current checkout.";
  if (tool.state !== "ready") return `${tool.message} Build the strategy_lab_backtest target, then retry.`;
  if (settingIssues(bt.settings).length) return "Fix the highlighted backtest settings first.";
  return undefined;
}

/** Whether the result on screen still matches the configuration and settings on screen. */
export function backtestRelation(lab: LabState, bt: BacktestState): "none" | "current" | "stale" {
  if (!bt.result) return "none";
  const request = currentRequest(lab);
  return request && keyOf(request, bt.settings) === bt.result.key ? "current" : "stale";
}

export class BacktestStore {
  private state: BacktestState = { settings: DEFAULT_SETTINGS, run: { phase: "idle", token: 0 }, launches: 0 };
  private readonly listeners = new Set<() => void>();
  private abort?: AbortController;

  constructor(private readonly client: LabClient, private readonly now: () => string = () => new Date().toISOString()) {}

  getState = (): BacktestState => this.state;

  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener);
    return () => void this.listeners.delete(listener);
  };

  private set(patch: Partial<BacktestState>): void {
    this.state = { ...this.state, ...patch };
    for (const listener of [...this.listeners]) listener();
  }

  setSetting = (name: SettingName, text: string): void => this.set({ settings: { ...this.state.settings, [name]: text } });
  setRiskPolicies = (on: boolean): void => this.set({ settings: { ...this.state.settings, risk_policies: on } });
  resetSettings = (): void => this.set({ settings: DEFAULT_SETTINGS });

  run = async (lab: LabState): Promise<boolean> => {
    if (backtestBlocker(lab, this.state)) return false;
    const request = currentRequest(lab)!;
    const title = specOf(lab.catalog, request.kind)?.title ?? request.kind;
    const settings = this.state.settings;
    const dataset = lab.dataset.kind === "upload" ? { name: lab.upload!.name, csv_base64: lab.upload!.base64 } : { builtin: lab.dataset.key };
    const token = this.state.run.token + 1;
    const launch = this.state.launches + 1;
    this.abort = new AbortController();
    this.set({ run: { phase: "running", token }, launches: launch });
    try {
      const response = await this.client.backtest(
        { strategy: { kind: request.kind, params: request.params }, dataset, settings: toSettings(settings) }, this.abort.signal);
      if (this.state.run.token !== token) return false;
      const doc = parseBacktest(response.raw);
      if (doc.dataset.sha256 !== request.dataset.sha256) throw toLabError(new Error("The backtest ran on a different dataset than the one requested."));
      this.set({
        result: { id: launch, doc, meta: response.meta, title, request, key: keyOf(request, settings), at: this.now() },
        run: { phase: "idle", token },
      });
      return true;
    } catch (error) {
      if (this.state.run.token !== token) return false;
      this.set({ run: { phase: "failed", token, error: toLabError(error) } });
      return false;
    } finally {
      if (this.state.run.token === token) this.abort = undefined;
    }
  };

  cancel = (): void => {
    this.abort?.abort();
    this.abort = undefined;
    this.set({ run: { phase: "idle", token: this.state.run.token + 1 } });
  };
}
