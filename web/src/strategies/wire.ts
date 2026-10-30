// Shared readers for the replay tool's JSON documents (catalog and replay): envelope checks and typed access that turn a
// structural surprise into a LabError(invalid_structure) naming the JSON path, never a silent default.

import { LabError } from "../api/lab";
import { LosslessParseError, isObject, parseLossless, safeInteger, type LValue } from "../lib/losslessJson";

export const SUPPORTED_SCHEMA_MAJOR = 1;

export const invalid = (path: string, message: string) => new LabError("invalid_structure", `${path}: ${message}`);

export const need = (o: { [key: string]: LValue }, key: string, path: string): LValue => {
  if (!(key in o)) throw invalid(path, `required member '${key}' is missing.`);
  return o[key]!;
};

export const asObject = (v: LValue, path: string) => {
  if (!isObject(v)) throw invalid(path, "expected an object.");
  return v;
};
export const asArray = (v: LValue, path: string) => {
  if (!Array.isArray(v)) throw invalid(path, "expected an array.");
  return v;
};
export const asString = (v: LValue, path: string) => {
  if (typeof v !== "string") throw invalid(path, "expected a string.");
  return v;
};
export const asBoolean = (v: LValue, path: string) => {
  if (typeof v !== "boolean") throw invalid(path, "expected true or false.");
  return v;
};
export const asCount = (v: LValue, path: string) => {
  const n = safeInteger(v);
  if (n === undefined || n < 0) throw invalid(path, "expected a whole number that fits a JavaScript integer.");
  return n;
};

/** Parse a lab document and check its envelope: the schema name and the supported major version. */
export function readEnvelope(text: string, schema: string): { [key: string]: LValue } {
  let root: LValue;
  try {
    root = parseLossless(text);
  } catch (error) {
    if (error instanceof LosslessParseError) throw new LabError("malformed_output", `The replay tool's output is not valid JSON: ${error.message}`);
    throw error;
  }
  const o = asObject(root, "$");
  const name = asString(need(o, "schema", "$"), "$.schema");
  if (name !== schema) throw new LabError("malformed_output", `Expected a '${schema}' document but got '${name}'.`);
  const version = asString(need(o, "schema_version", "$"), "$.schema_version");
  const major = /^(0|[1-9]\d*)\.(0|[1-9]\d*)$/.exec(version);
  if (!major) throw new LabError("unsupported_schema", `$.schema_version: '${version}' is not a MAJOR.MINOR version.`);
  if (Number(major[1]) !== SUPPORTED_SCHEMA_MAJOR) {
    throw new LabError(
      "unsupported_schema",
      `The tool's output uses schema version ${version}; this console understands major version ${SUPPORTED_SCHEMA_MAJOR}. Rebuild strategy_lab_replay and update the console from the same checkout.`,
    );
  }
  return o;
}
