// Test helper (no production code imports this): the gateway's validation documents as JSON text.

export const FILES_ID = "f1".repeat(32);
export const EXE_ID = "c3".repeat(32);

export interface ScenarioSpec {
  id: string;
  status?: "passed" | "failed" | "error" | "not_run";
  demonstration?: boolean;
  mismatches?: { where: string; expected: string; actual: string; note?: string }[];
  error?: { kind: string; title: string; message: string; exit_code?: number | null };
  runId?: string | null;
}

export function infoText(ids: string[] = ["alpha", "beta", "gamma"], identity = FILES_ID): string {
  return JSON.stringify({
    scenario_set: { directory: "tests/fixtures/strategy_lab/scenarios", identity_sha256: identity, files: ids.map((id) => `${id}.json`) },
    scenarios: ids.map((id) => ({ id, title: `Title of ${id}`, purpose: `Purpose of ${id}`, covers: ["warm-up"], outcome: "result", dataset_path: `datasets/${id}.csv`, strategies: [{ kind: "sma_crossover", strategy_id: "sma_crossover" }] })),
    demonstration_available: true,
    scope_note: "These are scenario checks. They are NOT the C++ CTest/GoogleTest suite.",
  });
}

export function reportText(specs: ScenarioSpec[] = [{ id: "alpha" }, { id: "beta" }, { id: "gamma" }], options: { exe?: string; files?: string; number?: number; include?: boolean } = {}): string {
  const normal = specs.filter((s) => !s.demonstration);
  const count = (status: string) => normal.filter((s) => (s.status ?? "passed") === status).length;
  return JSON.stringify({
    report: "strategy_lab.validation_report", report_version: "1.0", scope_note: "These are scenario checks. They are NOT the C++ CTest/GoogleTest suite.",
    validation_number_in_session: options.number ?? 1, started_at: "2026-10-09T12:00:00Z", finished_at: "2026-10-09T12:00:02Z",
    summary: { scenarios: normal.length, passed: count("passed"), failed: count("failed"), error: count("error"), not_run: 0, overall: count("error") ? "errors: some checks could not be carried out" : count("failed") ? "failures: some results differ from the expectations" : "all scenarios passed" },
    demonstration: { included: options.include ?? false, note: "An opt-in, in-memory scenario with deliberately wrong expectations.", results: specs.filter((s) => s.demonstration).map((s) => ({ id: s.id, status: s.status ?? "failed", summary: "did not match", failed_as_designed: (s.status ?? "failed") === "failed" })) },
    executable: { file_name: "strategy_lab_replay.exe", sha256: options.exe ?? EXE_ID, size_bytes: 123456, tool: "strategy_lab_replay", project_version: "0.0.1", compiler: "MSVC", build: "Release" },
    scenario_files: { directory: "tests/fixtures/strategy_lab/scenarios", identity_sha256: options.files ?? FILES_ID, files: normal.map((s) => `${s.id}.json`) },
    scenarios: specs.map((s) => ({
      id: s.id, title: `Title of ${s.id}`, purpose: `Purpose of ${s.id}`, covers: ["warm-up"], demonstration: s.demonstration ?? false, status: s.status ?? "passed",
      status_text: s.status ?? "passed", summary: s.status === "failed" ? `${s.mismatches?.length ?? 0} of 5 checks did not match.` : "All 5 checks matched.", checks_made: 5,
      mismatches: (s.mismatches ?? []).map((m) => ({ note: "", ...m })),
      dataset: { path: `datasets/${s.id}.csv`, sha256_lf_expected: "aa".repeat(32), sha256_given_to_tool: "ab".repeat(32) },
      configuration: [{ kind: "sma_crossover", strategy_id: "sma_crossover", parameters: { short_window: "2", long_window: "3", symbols: "AAPL" } }],
      expected: {}, provenance_of_the_expectation: { derived_by: "by hand", sources: ["docs/strategies/moving_average_crossover.md"], derivation: [], cross_checks: [] }, tolerances: {},
      scenario_file: `${s.id}.json`, run_id: s.runId === undefined ? "0123456789abcdef" : s.runId, result_sha256: s.runId === null ? null : "00".repeat(32),
      executable_sha256: options.exe ?? EXE_ID, duration_s: 0.125, finished_at: "2026-10-09T12:00:01Z",
      ...(s.error ? { error: { detail: "", stderr: "", ...s.error } } : {}),
    })),
  });
}
