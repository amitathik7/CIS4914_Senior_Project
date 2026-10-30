// The Strategy Lab's scenario validation, as the gateway reports it: which scenario files exist, and the outcome of running them
// through the real strategy_lab_replay executable.
//
// SCOPE (said wherever a result is shown): these are a few hand-derived scenario checks of the REAL strategy executable. They are not
// the repository's C++ CTest/GoogleTest suite, and passing them says nothing about that suite. A result belongs to the executable
// and the scenario files it names; it is never presented as a check of a different build or different files.
//
// Both documents are read losslessly and structurally (a missing or mistyped member is an error naming its path), and every value the
// person sees is the report's own text.

import { LabError } from "../api/lab";
import { LosslessParseError, derivedNumber, parseLossless, type LValue } from "../lib/losslessJson";
import { asArray, asBoolean, asCount, asObject, asString, invalid, need } from "./wire";

export type ScenarioStatus = "passed" | "failed" | "error" | "not_run";
const STATUSES: readonly string[] = ["passed", "failed", "error", "not_run"];
export const STATUS_TEXT: Readonly<Record<ScenarioStatus, string>> = { passed: "Passed", failed: "Failed", error: "Error", not_run: "Not run" };

export interface ScenarioInfo {
  id: string;
  title: string;
  purpose: string;
  covers: string[];
  outcome: string;
  datasetPath: string;
  strategies: { kind: string; strategyId: string }[];
}

export interface ValidationInfo {
  directory: string;
  identity: string;
  files: string[];
  scenarios: ScenarioInfo[];
  demonstrationAvailable: boolean;
  scopeNote: string;
}

export interface Mismatch {
  where: string;
  expected: string;
  actual: string;
  note: string;
}

export interface ScenarioOutcome {
  id: string;
  title: string;
  purpose: string;
  covers: string[];
  demonstration: boolean;
  status: ScenarioStatus;
  summary: string;
  checks: number;
  mismatches: Mismatch[];
  datasetPath: string;
  datasetSha256Expected: string;
  datasetSha256GivenToTool: string;
  executableSha256: string;
  configuration: { kind: string; strategyId: string; parameters: [string, string][] }[];
  runId?: string;
  resultSha256?: string;
  durationS: string;
  finishedAt: string;
  scenarioFile: string;
  error?: { kind: string; title: string; message: string; exitCode?: string; detail: string; stderr: string };
  derivedBy: string;
  sources: string[];
}

export interface ValidationReport {
  scopeNote: string;
  number: number;
  startedAt: string;
  finishedAt: string;
  summary: { scenarios: number; passed: number; failed: number; error: number; notRun: number; overall: string };
  demonstration: { included: boolean; note: string; results: { id: string; status: string; summary: string; failedAsDesigned: boolean }[] };
  executable: { fileName: string; sha256: string; sizeBytes: number; tool?: string; version?: string; compiler?: string; build?: string };
  scenarioFiles: { directory: string; identity: string; files: string[] };
  scenarios: ScenarioOutcome[];
}

type Obj = { [key: string]: LValue };

function read(text: string, what: string): Obj {
  try {
    return asObject(parseLossless(text), "$");
  } catch (error) {
    if (error instanceof LosslessParseError) throw new LabError("malformed_output", `The ${what} is not valid JSON: ${error.message}`);
    throw error;
  }
}

const str = (o: Obj, key: string, path: string) => asString(need(o, key, path), `${path}.${key}`);
const strings = (o: Obj, key: string, path: string) => asArray(need(o, key, path), `${path}.${key}`).map((v, i) => asString(v, `${path}.${key}[${i}]`));
const count = (o: Obj, key: string, path: string) => asCount(need(o, key, path), `${path}.${key}`);
/** A member that may be absent or null (the report writes null for "not applicable"). */
const maybe = (o: Obj, key: string, path: string): string | undefined => (o[key] === undefined || o[key] === null ? undefined : asString(o[key]!, `${path}.${key}`));
/** A member the report writes as text or as a number: kept as the text it was written with. */
function anyText(v: LValue | undefined): string {
  if (v === undefined || v === null) return "";
  if (typeof v === "string") return v;
  const d = derivedNumber(v);
  return d ? d.text : String(v);
}

export function parseValidationInfo(text: string): ValidationInfo {
  const o = read(text, "scenario list");
  const set = asObject(need(o, "scenario_set", "$"), "$.scenario_set");
  return {
    directory: str(set, "directory", "$.scenario_set"), identity: str(set, "identity_sha256", "$.scenario_set"), files: strings(set, "files", "$.scenario_set"),
    scenarios: asArray(need(o, "scenarios", "$"), "$.scenarios").map((raw, i): ScenarioInfo => {
      const p = `$.scenarios[${i}]`;
      const s = asObject(raw, p);
      return {
        id: str(s, "id", p), title: str(s, "title", p), purpose: str(s, "purpose", p), covers: strings(s, "covers", p), outcome: str(s, "outcome", p),
        datasetPath: str(s, "dataset_path", p),
        strategies: asArray(need(s, "strategies", p), `${p}.strategies`).map((c, k) => {
          const cp = `${p}.strategies[${k}]`;
          const conf = asObject(c, cp);
          return { kind: str(conf, "kind", cp), strategyId: str(conf, "strategy_id", cp) };
        }),
      };
    }),
    demonstrationAvailable: asBoolean(need(o, "demonstration_available", "$"), "$.demonstration_available"),
    scopeNote: str(o, "scope_note", "$"),
  };
}

function scenarioOutcome(raw: LValue, path: string): ScenarioOutcome {
  const s = asObject(raw, path);
  const status = str(s, "status", path);
  if (!STATUSES.includes(status)) throw invalid(`${path}.status`, `'${status}' is not passed, failed, error or not_run.`);
  const dataset = asObject(need(s, "dataset", path), `${path}.dataset`);
  const provenance = asObject(need(s, "provenance_of_the_expectation", path), `${path}.provenance_of_the_expectation`);
  const failure = s.error === undefined || s.error === null ? undefined : asObject(s.error, `${path}.error`);
  const runId = maybe(s, "run_id", path);
  const resultSha = maybe(s, "result_sha256", path);
  return {
    id: str(s, "id", path), title: str(s, "title", path), purpose: str(s, "purpose", path), covers: strings(s, "covers", path),
    demonstration: asBoolean(need(s, "demonstration", path), `${path}.demonstration`), status: status as ScenarioStatus, summary: str(s, "summary", path),
    checks: count(s, "checks_made", path),
    mismatches: asArray(need(s, "mismatches", path), `${path}.mismatches`).map((m, i): Mismatch => {
      const mp = `${path}.mismatches[${i}]`;
      const mo = asObject(m, mp);
      return { where: str(mo, "where", mp), expected: str(mo, "expected", mp), actual: str(mo, "actual", mp), note: maybe(mo, "note", mp) ?? "" };
    }),
    datasetPath: str(dataset, "path", `${path}.dataset`), datasetSha256Expected: str(dataset, "sha256_lf_expected", `${path}.dataset`),
    datasetSha256GivenToTool: str(dataset, "sha256_given_to_tool", `${path}.dataset`), executableSha256: str(s, "executable_sha256", path),
    configuration: asArray(need(s, "configuration", path), `${path}.configuration`).map((c, i) => {
      const cp = `${path}.configuration[${i}]`;
      const co = asObject(c, cp);
      const params = asObject(need(co, "parameters", cp), `${cp}.parameters`);
      return { kind: str(co, "kind", cp), strategyId: str(co, "strategy_id", cp), parameters: Object.entries(params).map(([k, v]): [string, string] => [k, anyText(v)]) };
    }),
    ...(runId !== undefined ? { runId } : {}), ...(resultSha !== undefined ? { resultSha256: resultSha } : {}),
    durationS: anyText(s.duration_s), finishedAt: str(s, "finished_at", path), scenarioFile: str(s, "scenario_file", path),
    ...(failure ? { error: { kind: str(failure, "kind", `${path}.error`), title: str(failure, "title", `${path}.error`), message: str(failure, "message", `${path}.error`),
      ...(failure.exit_code !== undefined && failure.exit_code !== null ? { exitCode: anyText(failure.exit_code) } : {}), detail: maybe(failure, "detail", `${path}.error`) ?? "", stderr: maybe(failure, "stderr", `${path}.error`) ?? "" } } : {}),
    derivedBy: str(provenance, "derived_by", `${path}.provenance_of_the_expectation`), sources: strings(provenance, "sources", `${path}.provenance_of_the_expectation`),
  };
}

export function parseValidationReport(text: string): ValidationReport {
  const o = read(text, "validation report");
  if (str(o, "report", "$") !== "strategy_lab.validation_report") throw new LabError("malformed_output", "Expected a strategy_lab.validation_report document.");
  const version = str(o, "report_version", "$");
  if (!/^1\.\d+$/.test(version)) throw new LabError("unsupported_schema", `The validation report is version ${version}; this console understands version 1.`);
  const summary = asObject(need(o, "summary", "$"), "$.summary");
  const demo = asObject(need(o, "demonstration", "$"), "$.demonstration");
  const exe = asObject(need(o, "executable", "$"), "$.executable");
  const files = asObject(need(o, "scenario_files", "$"), "$.scenario_files");
  return {
    scopeNote: str(o, "scope_note", "$"), number: count(o, "validation_number_in_session", "$"), startedAt: str(o, "started_at", "$"), finishedAt: str(o, "finished_at", "$"),
    summary: {
      scenarios: count(summary, "scenarios", "$.summary"), passed: count(summary, "passed", "$.summary"), failed: count(summary, "failed", "$.summary"),
      error: count(summary, "error", "$.summary"), notRun: count(summary, "not_run", "$.summary"), overall: str(summary, "overall", "$.summary"),
    },
    demonstration: {
      included: asBoolean(need(demo, "included", "$.demonstration"), "$.demonstration.included"), note: str(demo, "note", "$.demonstration"),
      results: asArray(need(demo, "results", "$.demonstration"), "$.demonstration.results").map((r, i) => {
        const p = `$.demonstration.results[${i}]`;
        const ro = asObject(r, p);
        return { id: str(ro, "id", p), status: str(ro, "status", p), summary: str(ro, "summary", p), failedAsDesigned: asBoolean(need(ro, "failed_as_designed", p), `${p}.failed_as_designed`) };
      }),
    },
    executable: {
      fileName: str(exe, "file_name", "$.executable"), sha256: str(exe, "sha256", "$.executable"), sizeBytes: count(exe, "size_bytes", "$.executable"),
      ...(maybe(exe, "tool", "$.executable") !== undefined ? { tool: maybe(exe, "tool", "$.executable")! } : {}),
      ...(maybe(exe, "project_version", "$.executable") !== undefined ? { version: maybe(exe, "project_version", "$.executable")! } : {}),
      ...(maybe(exe, "compiler", "$.executable") !== undefined ? { compiler: maybe(exe, "compiler", "$.executable")! } : {}),
      ...(maybe(exe, "build", "$.executable") !== undefined ? { build: maybe(exe, "build", "$.executable")! } : {}),
    },
    scenarioFiles: { directory: str(files, "directory", "$.scenario_files"), identity: str(files, "identity_sha256", "$.scenario_files"), files: strings(files, "files", "$.scenario_files") },
    scenarios: asArray(need(o, "scenarios", "$"), "$.scenarios").map((s, i) => scenarioOutcome(s, `$.scenarios[${i}]`)),
  };
}
