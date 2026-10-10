// The state of the Advanced > Validation area: the list of scenario files, and the last validation the person asked for.
//
// Rules this file exists to enforce:
//   * A validation runs ONLY when run() is called (the button). Opening the page, refreshing the file list, changing the
//     demonstration checkbox and switching tabs start nothing; `launches` counts real runs and the tests assert it.
//   * A result is a frozen record of ONE validation: the executable (SHA-256) and the scenario files (identity) it ran. If the engine
//     is rebuilt or the files change afterwards, the result is marked "does not describe the current build / files": it is kept, never
//     relabelled, and never offered as a check of the newer one.
//   * One validation at a time; an answer for a cancelled or superseded run is dropped.

import { LabError, type LabClient } from "../api/lab";
import { toLabError, type LabState, type LabStore } from "./store";
import { parseValidationInfo, parseValidationReport, type ValidationInfo, type ValidationReport } from "./validation";

export interface ValidationResult {
  readonly launch: number;
  readonly receivedAt: string;
  /** The gateway's answer exactly as it arrived (this is what "Download JSON" writes). */
  readonly raw: string;
  readonly report: ValidationReport;
}

export interface ValidationOwnState {
  info?: ValidationInfo;
  infoPhase: "idle" | "loading" | "ready" | "failed";
  infoError?: LabError;
  includeDemonstration: boolean;
  run: { phase: "idle" | "running" | "failed"; token: number; error?: LabError; cancelled?: boolean };
  launches: number;
  result?: ValidationResult;
}

export type ValidationState = ValidationOwnState & { lab: LabState };

export type LabDirectory = Pick<LabStore, "getState" | "subscribe">;

export interface ValidationOptions {
  now?: () => string;
}

/** Why Run validation is unavailable, in a sentence; undefined when it can start. */
export function validationBlocker(state: ValidationState): string | undefined {
  if (state.run.phase === "running") return "A validation is already running.";
  const phase = state.lab.service.phase;
  if (phase === "offline") return "The Strategy Lab service is not running, so nothing can be validated. Start it, then retry.";
  if (phase === "no_runner") return "The service is running but the replay engine (strategy_lab_replay) has not been built, so there is nothing to validate.";
  if (phase !== "online") return phase === "error" ? "The service answered, but the console could not use its answer." : "Connecting to the Strategy Lab service…";
  if (state.infoPhase === "failed") return "The scenario files could not be read; see the message above.";
  return undefined;
}

export interface ValidationStatus {
  /** none: nothing run yet. current: describes the engine and files in use now. stale: it does not. unknown: nothing to compare with yet. */
  relation: "none" | "current" | "stale" | "unknown";
  reasons: string[];
}

export function validationStatus(state: ValidationState): ValidationStatus {
  const result = state.result;
  if (!result) return { relation: "none", reasons: [] };
  const reasons: string[] = [];
  const runner = state.lab.service.status?.runner;
  if (runner?.state === "ready" && runner.sha256 !== result.report.executable.sha256) reasons.push("The replay engine was rebuilt since this validation, so it does not describe the build in use now.");
  if (state.info && state.info.identity !== result.report.scenarioFiles.identity) reasons.push("The scenario files changed since this validation, so it does not describe the files in use now.");
  if (reasons.length) return { relation: "stale", reasons };
  return { relation: runner?.state === "ready" && state.info ? "current" : "unknown", reasons };
}

export class ValidationStore {
  private own: ValidationOwnState;
  private view: ValidationState;
  private readonly listeners = new Set<() => void>();
  private abort?: AbortController;
  private readonly now: () => string;

  constructor(private readonly client: LabClient, private readonly lab: LabDirectory, options: ValidationOptions = {}) {
    this.now = options.now ?? (() => new Date().toISOString());
    this.own = { infoPhase: "idle", includeDemonstration: false, run: { phase: "idle", token: 0 }, launches: 0 };
    this.view = { ...this.own, lab: lab.getState() };
    lab.subscribe(() => this.publish());
  }

  getState = (): ValidationState => this.view;

  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener);
    return () => void this.listeners.delete(listener);
  };

  private publish(): void {
    this.view = { ...this.own, lab: this.lab.getState() };
    for (const listener of [...this.listeners]) listener();
  }

  private set(patch: Partial<ValidationOwnState>): void {
    this.own = { ...this.own, ...patch };
    this.publish();
  }

  /** Read the list of scenario files (nothing is run). Safe to call again. */
  loadInfo = async (): Promise<void> => {
    if (this.own.infoPhase === "loading") return;
    this.set({ infoPhase: "loading", infoError: undefined });
    try {
      const info = parseValidationInfo(await this.client.validationInfo());
      this.set({ info, infoPhase: "ready" });
    } catch (error) {
      this.set({ infoPhase: "failed", infoError: toLabError(error) });
    }
  };

  setIncludeDemonstration = (include: boolean): void => this.set({ includeDemonstration: include });

  /** THE only place a validation starts. Returns true when a new result was stored. */
  run = async (): Promise<boolean> => {
    if (validationBlocker(this.view)) return false;
    const token = this.own.run.token + 1;
    const launch = this.own.launches + 1;
    const controller = new AbortController();
    this.abort = controller;
    this.set({ run: { phase: "running", token }, launches: launch });
    try {
      const raw = await this.client.validate({ include_demonstration: this.own.includeDemonstration }, controller.signal);
      if (this.own.run.token !== token) return false; // cancelled or superseded
      const report = parseValidationReport(raw);
      this.set({ result: Object.freeze({ launch, receivedAt: this.now(), raw, report }), run: { phase: "idle", token } });
      return true;
    } catch (error) {
      if (this.own.run.token !== token) return false;
      this.set({ run: { phase: "failed", token, error: toLabError(error) } });
      return false;
    } finally {
      if (this.abort === controller) this.abort = undefined;
    }
  };

  cancel = (): void => {
    if (this.own.run.phase !== "running") return;
    this.abort?.abort();
    this.set({ run: { phase: "idle", token: this.own.run.token + 1, cancelled: true } });
  };

  dismissError = (): void => {
    if (this.own.run.phase === "failed") this.set({ run: { phase: "idle", token: this.own.run.token } });
  };
}
