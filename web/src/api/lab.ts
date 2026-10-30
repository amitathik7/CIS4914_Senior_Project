// Client for the Strategy Lab gateway (python/strategy_lab/strategy_lab_ui/gateway.py), which runs the real C++
// `strategy_lab_replay` tool. This is NOT the engine API of ADR 0006: it never goes through EngineClient or the demo engine.
//
// Wire rules (see the gateway's module docstring):
//   * every parameter travels as text, exactly as the person typed it, never as a number;
//   * a replay answer has two parts: line 1 is gateway metadata (plain JSON), everything after the first newline is the
//     replay tool's stdout verbatim, kept here as a string and read with the lossless reader (lib/losslessJson.ts).

export interface LabProblem {
  message: string;
  line: number | null;
  column: string | null;
  where: string | null;
}

export class LabError extends Error {
  constructor(
    readonly kind: string,
    message: string,
    readonly details: {
      status?: number;
      title?: string;
      problems?: LabProblem[];
      totalProblems?: number;
      fields?: string[];
      field?: string;
      exitStatus?: number | null;
      detail?: string;
      stderr?: string;
      setup?: string;
    } = {},
  ) {
    super(message);
  }
  get problems(): LabProblem[] {
    return this.details.problems ?? [];
  }
  get fields(): string[] {
    return this.details.fields ?? [];
  }
  /** The service itself could not be reached (as opposed to the engine refusing something). */
  get unreachable(): boolean {
    return this.kind === "service_unavailable";
  }
}

export type RunnerInfo =
  | {
      state: "ready"; name: string; size: number; sha256: string; tool: string; project_version: string;
      schema_version: string; default_max_rows: number; hard_max_rows: number; max_rows_used: number;
    }
  | { state: "missing" | "invalid"; message: string; detail: string; setup?: string };

export interface LabStatus {
  gateway: { version: string; api: string };
  runner: RunnerInfo;
}

export interface BuiltinDataset {
  key: string;
  label: string;
  description: string;
  filename: string;
  presets: Record<string, Record<string, string>>; // strategy kind -> parameter text (examples, not recommendations)
  synthetic: boolean;
  provenance: string;
  available: boolean;
  message?: string;
  bytes?: number;
  sha256?: string;
  rows?: number;
  symbols?: string[];
}

export interface DatasetsResponse {
  datasets: BuiltinDataset[];
  upload_limit_bytes: number;
}

export interface StrategyRequest {
  kind: string;
  params: [string, string][];
}

export type DatasetRef = { builtin: string } | { name: string; csv_base64: string };

export interface ReplayRequest {
  strategy: StrategyRequest;
  dataset: DatasetRef;
}

export interface CheckResult {
  ok: true;
  kind: string;
  strategy_id: string;
  window_size: number;
  derived: Record<string, number | boolean | string>;
  parameters: Record<string, unknown>;
  warnings: { code: string; message: string }[];
}

export interface ReplayMeta {
  gateway: { version: string; api: string };
  runner: { name: string; size: number; sha256: string };
  dataset: { kind: "builtin" | "upload"; name: string; sha256: string; bytes: number; synthetic: boolean; provenance: string };
  request: { kind: string; params: [string, string][]; max_rows: number };
  process: { exit_code: number; duration_ms: number; args_file: string; stderr: string; stderr_truncated: boolean };
}

export interface ReplayResponse {
  meta: ReplayMeta;
  /** The replay tool's stdout, byte for byte (including its trailing newline). */
  raw: string;
}

export interface ValidationRequest {
  include_demonstration: boolean;
}

export interface LabClient {
  status(signal?: AbortSignal): Promise<LabStatus>;
  /** The tool's own `describe` document as text. */
  catalog(signal?: AbortSignal): Promise<string>;
  datasets(signal?: AbortSignal): Promise<DatasetsResponse>;
  /** Resolves when the real strategy constructors accept the configuration; rejects with a LabError(runner_rejected) otherwise. */
  check(strategy: StrategyRequest, signal?: AbortSignal): Promise<CheckResult>;
  replay(request: ReplayRequest, signal?: AbortSignal): Promise<ReplayResponse>;
  /** The scenario files a validation would run, as JSON text. Runs nothing. */
  validationInfo(signal?: AbortSignal): Promise<string>;
  /** Run the Lab's scenario checks through the real executable; the answer is the Lab's validation report as JSON text. */
  validate(request: ValidationRequest, signal?: AbortSignal): Promise<string>;
}

const OFFLINE =
  "The Strategy Lab service is not reachable. Start it with: python python/strategy_lab/lab_gateway.py (see docs/STRATEGIES_CONSOLE.md).";

export function splitReplayBody(text: string): ReplayResponse {
  const newline = text.indexOf("\n");
  if (newline < 0) throw new LabError("malformed_output", "The service answered with an unexpected format (no metadata line).");
  try {
    return { meta: JSON.parse(text.slice(0, newline)) as ReplayMeta, raw: text.slice(newline + 1) };
  } catch {
    throw new LabError("malformed_output", "The service's metadata line is not valid JSON.");
  }
}

interface ErrorBody {
  error?: {
    code?: string; title?: string; message?: string; problems?: LabProblem[]; total_problems?: number; fields?: string[];
    field?: string; exit_status?: number | null; detail?: string; stderr?: string; setup?: string;
  };
}

function errorFrom(e: NonNullable<ErrorBody["error"]>, status: number): LabError {
  return new LabError(e.code ?? "gateway_error", e.message ?? "The service reported an error.", {
    status, title: e.title, problems: e.problems, totalProblems: e.total_problems, fields: e.fields,
    field: e.field, exitStatus: e.exit_status, detail: e.detail, stderr: e.stderr, setup: e.setup,
  });
}

export class HttpLabClient implements LabClient {
  constructor(private readonly base: string = "/lab") {}

  private async send(path: string, init?: RequestInit): Promise<Response> {
    let response: Response;
    try {
      response = await fetch(`${this.base}/v1${path}`, init);
    } catch (error) {
      if (error instanceof DOMException && error.name === "AbortError") throw error;
      throw new LabError("service_unavailable", OFFLINE);
    }
    const type = response.headers.get("Content-Type") ?? "";
    if (response.ok) {
      // A dev server that was started before /lab was proxied answers every unknown path with the app's index.html.
      if (!type.includes("json")) {
        throw new LabError("service_unavailable", `${OFFLINE} If it is running, restart the console's dev server so that /lab is proxied to it.`, { status: response.status });
      }
      return response;
    }
    // A dev-server proxy with nothing behind it answers 500 with no JSON body: that is "not running", not an engine error.
    const body = type.includes("json") ? ((await response.json().catch(() => null)) as ErrorBody | null) : null;
    if (!body?.error?.code) throw new LabError("service_unavailable", OFFLINE, { status: response.status });
    throw errorFrom(body.error, response.status);
  }

  private post(path: string, payload: unknown, signal?: AbortSignal) {
    return this.send(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload), signal });
  }

  status = async (signal?: AbortSignal) => (await this.send("/status", { signal })).json() as Promise<LabStatus>;
  catalog = async (signal?: AbortSignal) => (await this.send("/catalog", { signal })).text();
  datasets = async (signal?: AbortSignal) => (await this.send("/datasets", { signal })).json() as Promise<DatasetsResponse>;
  check = async (strategy: StrategyRequest, signal?: AbortSignal): Promise<CheckResult> => {
    const body = (await (await this.post("/check", { strategy }, signal)).json()) as CheckResult | { ok: false; error: ErrorBody["error"] };
    if (body.ok) return body;
    throw errorFrom(body.error ?? {}, 422);
  };
  replay = async (request: ReplayRequest, signal?: AbortSignal) => splitReplayBody(await (await this.post("/replay", request, signal)).text());
  validationInfo = async (signal?: AbortSignal) => (await this.send("/validation", { signal })).text();
  validate = async (request: ValidationRequest, signal?: AbortSignal) => (await this.post("/validate", request, signal)).text();
}

export function createLabClient(): LabClient {
  const base = (import.meta.env.VITE_LAB_API as string | undefined) || "/lab";
  return new HttpLabClient(base.replace(/\/$/, ""));
}
