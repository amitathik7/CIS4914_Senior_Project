// The replay tool's `describe` document: the single authority for strategy kinds, parameter names, types, defaults and the
// plain-language text of every decision reason (docs/STRATEGY_LAB.md section 8). Nothing here is a copy of it: the Console
// builds its forms from this document and keeps no table of its own.

import { LabError } from "../api/lab";
import { integerText, isLNum, type LValue } from "../lib/losslessJson";
import { asArray, asBoolean, asCount, asObject, asString, invalid, need, readEnvelope } from "./wire";

export type ParamType = "string" | "uint" | "double" | "string_list";

export interface ParamSpec {
  name: string;
  type: ParamType;
  required: boolean;
  constraint: string;
  /** The default as text (what a person would type); undefined when the tool declares none. */
  defaultText?: string;
}

export interface ReasonSpec {
  code: string;
  verdict: string;
  text: string;
}

export interface StrategySpec {
  kind: string;
  title: string;
  summary: string;
  docs: string;
  available: boolean;
  unavailableReason: string;
  params: ParamSpec[];
  reasons: ReasonSpec[];
  indicators: string[];
  states: string[];
  metadataKeys: string[];
}

export interface Catalog {
  tool: string;
  projectVersion: string;
  schemaVersion: string;
  defaultMaxRows: number;
  hardMaxRows: number;
  datasetFormat: string;
  strategies: StrategySpec[];
}

const PARAM_TYPES: readonly string[] = ["string", "uint", "double", "string_list"];

function defaultText(type: ParamType, raw: LValue, path: string): string {
  if (type === "uint") {
    const text = integerText(raw);
    if (text === undefined) throw invalid(path, "a whole-number default must be an exact integer.");
    return text;
  }
  if (type === "double") {
    if (!isLNum(raw)) throw invalid(path, "expected a number.");
    return raw.text;
  }
  if (type === "string") return asString(raw, path);
  return asArray(raw, path).map((item, i) => asString(item, `${path}[${i}]`)).join(",");
}

function param(raw: LValue, path: string): ParamSpec {
  const o = asObject(raw, path);
  const type = asString(need(o, "type", path), `${path}.type`);
  if (!PARAM_TYPES.includes(type)) throw new LabError("unsupported_schema", `${path}.type: unknown parameter type '${type}'.`);
  const kind = type as ParamType;
  return {
    name: asString(need(o, "name", path), `${path}.name`),
    type: kind,
    required: asBoolean(need(o, "required", path), `${path}.required`),
    constraint: asString(need(o, "constraint", path), `${path}.constraint`),
    ...("default" in o ? { defaultText: defaultText(kind, o.default!, `${path}.default`) } : {}),
  };
}

const strings = (o: { [key: string]: LValue }, key: string, path: string) =>
  asArray(need(o, key, path), `${path}.${key}`).map((v, i) => asString(v, `${path}.${key}[${i}]`));

function strategy(raw: LValue, path: string): StrategySpec {
  const o = asObject(raw, path);
  const reasons = asArray(need(o, "decision_reasons", path), `${path}.decision_reasons`).map((r, i): ReasonSpec => {
    const p = `${path}.decision_reasons[${i}]`;
    const e = asObject(r, p);
    return {
      code: asString(need(e, "code", p), `${p}.code`),
      verdict: asString(need(e, "verdict", p), `${p}.verdict`),
      text: asString(need(e, "text", p), `${p}.text`),
    };
  });
  return {
    kind: asString(need(o, "kind", path), `${path}.kind`),
    title: asString(need(o, "title", path), `${path}.title`),
    summary: asString(need(o, "summary", path), `${path}.summary`),
    docs: asString(need(o, "docs", path), `${path}.docs`),
    available: asBoolean(need(o, "available", path), `${path}.available`),
    unavailableReason: "unavailable_reason" in o ? asString(o.unavailable_reason!, `${path}.unavailable_reason`) : "",
    params: asArray(need(o, "parameters", path), `${path}.parameters`).map((p, i) => param(p, `${path}.parameters[${i}]`)),
    reasons,
    indicators: strings(o, "indicators", path),
    states: strings(o, "states", path),
    metadataKeys: strings(o, "signal_metadata_keys", path),
  };
}

export function parseCatalog(text: string): Catalog {
  const root = readEnvelope(text, "strategy_lab.catalog");
  const body = asObject(need(root, "catalog", "$"), "$.catalog");
  const limits = asObject(need(body, "limits", "$.catalog"), "$.catalog.limits");
  const format = asObject(need(body, "dataset_format", "$.catalog"), "$.catalog.dataset_format");
  const strategies = asArray(need(body, "strategies", "$.catalog"), "$.catalog.strategies").map((s, i) => strategy(s, `$.catalog.strategies[${i}]`));
  const kinds = strategies.map((s) => s.kind);
  if (new Set(kinds).size !== kinds.length) throw invalid("$.catalog.strategies", "a strategy kind is listed twice.");
  return {
    tool: asString(need(body, "tool", "$.catalog"), "$.catalog.tool"),
    projectVersion: asString(need(body, "project_version", "$.catalog"), "$.catalog.project_version"),
    schemaVersion: asString(root.schema_version!, "$.schema_version"),
    defaultMaxRows: asCount(need(limits, "default_max_rows", "$.catalog.limits"), "$.catalog.limits.default_max_rows"),
    hardMaxRows: asCount(need(limits, "hard_max_rows", "$.catalog.limits"), "$.catalog.limits.hard_max_rows"),
    datasetFormat: asString(need(format, "id", "$.catalog.dataset_format"), "$.catalog.dataset_format.id"),
    strategies,
  };
}

export const specOf = (catalog: Catalog | undefined, kind: string): StrategySpec | undefined => catalog?.strategies.find((s) => s.kind === kind);
export const availableStrategies = (catalog: Catalog | undefined): StrategySpec[] => catalog?.strategies.filter((s) => s.available) ?? [];
export const reasonText = (spec: StrategySpec | undefined, code: string): string | undefined => spec?.reasons.find((r) => r.code === code)?.text;
