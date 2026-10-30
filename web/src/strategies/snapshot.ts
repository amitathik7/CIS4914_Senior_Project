// The immutable record of one completed replay: the request that was sent, the dataset identity, the tool's exact output, and
// what was parsed from it. Editing the draft afterwards never changes a snapshot; it only makes the snapshot "stale".

import type { ReplayMeta, ReplayResponse } from "../api/lab";
import { LabError } from "../api/lab";
import { buildModel, type ReplayModel } from "./cursor";
import { requestKey, type RequestSnapshot } from "./draft";
import { parseReplay, type ReplayDoc } from "./replay";

export interface Snapshot {
  /** Which launch of this browser session produced it (1, 2, 3...). */
  readonly id: number;
  readonly finishedAt: string;
  readonly request: RequestSnapshot;
  readonly requestKey: string;
  readonly strategyTitle: string;
  /** The replay tool's stdout, byte for byte (its trailing newline included). */
  readonly raw: string;
  readonly doc: ReplayDoc;
  readonly model: ReplayModel;
  readonly meta: ReplayMeta;
}

/** Parse and cross-check a replay answer against the request it answers. Throws LabError (result_mismatch, invalid_structure...). */
export function makeSnapshot(id: number, request: RequestSnapshot, strategyTitle: string, response: ReplayResponse, finishedAt: string): Snapshot {
  const doc = parseReplay(response.raw);
  const problems: string[] = [];
  if (doc.dataset.sha256 !== request.dataset.sha256) problems.push(`the tool read a dataset with SHA-256 ${doc.dataset.sha256}, not ${request.dataset.sha256}`);
  if (response.meta.dataset.sha256 !== request.dataset.sha256) problems.push("the service reports a different dataset than the one requested");
  if (doc.strategies.length !== 1 || doc.strategies[0]!.kind !== request.kind) problems.push(`expected one ${request.kind} strategy, got ${doc.strategies.map((s) => s.kind).join(", ") || "none"}`);
  if (problems.length) throw new LabError("result_mismatch", `The result does not describe the run that was requested: ${problems.join("; ")}.`);
  const frozen = Object.freeze({
    id, finishedAt, request: Object.freeze({ ...request, params: Object.freeze([...request.params]) as unknown as [string, string][] }),
    requestKey: requestKey(request), strategyTitle, raw: response.raw, doc: Object.freeze(doc), model: buildModel(doc), meta: Object.freeze(response.meta),
  });
  Object.freeze(doc.events);
  Object.freeze(doc.signals);
  return frozen;
}
