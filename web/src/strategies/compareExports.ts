// Downloads for a completed comparison, in the formats the Strategy Lab's Compare already writes (a ZIP of a provenance JSON, labelled
// CSVs and both runs' original results), so a bundle from either place reads the same way.
//
// Every export says whether it covers the FULL RUN or the VISIBLE REPLAY PREFIX (what the shared position currently reveals, under the
// current symbol filter), in the file name and in a column of every row. Values are written exactly as recorded: prices, quantities and
// ids as exact text, timestamps in UTC with nine fractional digits. Counts are counts of recorded requests; no column ranks the sides.
// Building a file never starts the engine, and there is no wall-clock "exported at": the same comparison gives the same bytes.

import { zipStore } from "../lib/zip";
import { MEMBERS, memberLabel, type CompareSignal, type Member } from "./compare";
import { compareSignalsThrough } from "./compare";
import { AGREEMENT_DEFINITION, rowsThrough, summarize, type CompareSummary } from "./compareSummary";
import type { ComparisonSnapshot } from "./compareState";
import { csv, safeName, type ExportScope } from "./exports";
import { resultOf } from "./cursor";

export const COMPARE_NOTE = "Signal requests only: no fills, returns or portfolio. Counts are not a measure of strategy quality.";

const encode = (text: string): Uint8Array => new TextEncoder().encode(text);
const cell = (value: string | number | undefined): string => (value === undefined ? "" : String(value));

export const labelOf = (done: ComparisonSnapshot, member: Member): string => {
  const snap = done.snapshots[member];
  return memberLabel(member, snap.strategyTitle, snap.request.params);
};

export function scopeText(done: ComparisonSnapshot, scope: ExportScope, cursor: number, symbol: string | null): string {
  const total = done.model.total;
  return scope === "full_run"
    ? `full_run (all ${total} events, all symbols, both configurations)`
    : `visible_prefix (events 1-${cursor} of ${total}; symbol filter: ${symbol ?? "all symbols"}; both configurations)`;
}

const rowsFor = (done: ComparisonSnapshot, scope: ExportScope, cursor: number, symbol: string | null) =>
  scope === "full_run" ? [...done.model.rows] : rowsThrough(done.model, cursor, symbol);

const summaryFor = (done: ComparisonSnapshot, scope: ExportScope, cursor: number, symbol: string | null): CompareSummary =>
  scope === "full_run" ? summarize(done.model, done.model.total, null) : summarize(done.model, cursor, symbol);

// ---- CSV -------------------------------------------------------------------------------------------------------------------

export function signalsCsv(done: ComparisonSnapshot, scope: ExportScope, cursor: number, symbol: string | null): string {
  const signals: readonly CompareSignal[] = scope === "full_run" ? done.model.signals : compareSignalsThrough(done.model, cursor, symbol);
  const metadataKeys = [...new Set(signals.flatMap((s) => Object.keys(s.signal.metadata)))].sort();
  const header = [
    "export_scope", "member", "member_label", "strategy_kind", "strategy_id", "run_id", "signal_id", "signal_ref", "member_signal_key", "event_index",
    "event_number", "symbol", "side", "order_type", "requested_quantity", "limit_price", "created_at", "bus_sequence", ...metadataKeys.map((k) => `metadata.${k}`),
  ];
  const where = scopeText(done, scope, cursor, symbol);
  return csv([
    header,
    ...signals.map(({ member, key, signal: s }) => [
      where, member, labelOf(done, member), done.snapshots[member].request.kind, s.strategyId, done.snapshots[member].doc.runId, s.id, s.ref, key,
      String(s.eventIndex), String(s.eventIndex + 1), s.symbol, s.side, cell(s.orderType), cell(s.quantity), cell(s.limitPrice), s.createdAt,
      cell(s.busSequence), ...metadataKeys.map((k) => s.metadata[k] ?? ""),
    ]),
  ]);
}

export function summaryCsv(done: ComparisonSnapshot, scope: ExportScope, cursor: number, symbol: string | null): string {
  const summary = summaryFor(done, scope, cursor, symbol);
  const where = scopeText(done, scope, cursor, symbol);
  return csv([
    ["export_scope", "member", "member_label", "strategy_kind", "strategy_id", "run_id", "events_counted", "buy_requests", "sell_requests", "signal_requests",
      "evaluated", "warming_up", "ignored", "no_diagnostics"],
    ...MEMBERS.map((m) => {
      const c = summary.members[m];
      const snap = done.snapshots[m];
      return [where, m, labelOf(done, m), snap.request.kind, snap.doc.strategies[0]!.strategyId, snap.doc.runId, String(c.events), String(c.buys), String(c.sells),
        String(c.buys + c.sells), String(c.verdicts.evaluated), String(c.verdicts.warming_up), String(c.verdicts.ignored), String(c.verdicts.unavailable)];
    }),
  ]);
}

/** The agreement figures with their denominator and definition, for the same scope as the other files. */
export function agreementCsv(done: ComparisonSnapshot, scope: ExportScope, cursor: number, symbol: string | null): string {
  const a = summaryFor(done, scope, cursor, symbol).agreement;
  return csv([
    ["export_scope", "comparable_events", "agree", "differ", "not_comparable", "request_differences", "verdict_differences", "definition"],
    [scopeText(done, scope, cursor, symbol), String(a.comparable), String(a.agree), String(a.differ), String(a.notComparable), String(a.requestDifferences),
      String(a.verdictDifferences), AGREEMENT_DEFINITION],
  ]);
}

/** One row per recorded event with both sides' verdicts, requests and readiness, and how the two relate. */
export function eventsCsv(done: ComparisonSnapshot, scope: ExportScope, cursor: number, symbol: string | null): string {
  const { model } = done;
  const rows = rowsFor(done, scope, cursor, symbol);
  const results = (m: Member, i: number) => resultOf(model.members[m], model.members[m].events[i]!);
  const names = Object.fromEntries(MEMBERS.map((m) => [m, [...new Set(model.members[m].events.flatMap((e) => {
    const r = e.results[model.members[m].slot]!;
    return [...Object.keys(r.indicators), ...Object.keys(r.unavailable)];
  }))]])) as Record<Member, string[]>;
  const header = [
    "export_scope", "event_index", "event_number", "source_line", "symbol", "exchange_time_utc", "type", "price", "relation", "requests_differ", "verdicts_differ",
    ...MEMBERS.flatMap((m) => [
      `${m}_diagnostics_available`, `${m}_verdict`, `${m}_reason`, `${m}_action`, `${m}_window_fill`, `${m}_window_remaining`, `${m}_window_size`,
      `${m}_signal_keys`, ...names[m].flatMap((n) => [`${m}.${n}`, `${m}.${n}_unavailable`]),
    ]),
  ];
  const where = scopeText(done, scope, cursor, symbol);
  return csv([
    header,
    ...rows.map((row) => {
      const e = model.events[row.index]!;
      return [
        where, String(e.index), String(e.index + 1), cell(e.sourceLine), e.symbol, e.time, e.type, cell(e.price), row.relation, String(row.requestsDiffer), String(row.standingsDiffer),
        ...MEMBERS.flatMap((m) => {
          const r = results(m, row.index);
          return [
            String(r.available), cell(r.verdict), cell(r.reason), cell(r.action), cell(r.window?.fill), cell(r.window?.remaining), cell(r.window?.size),
            (model.signalsByEvent.get(row.index) ?? []).filter((s) => s.member === m).map((s) => s.key).join(";"),
            ...names[m].flatMap((n) => [r.indicators[n]?.text ?? "", r.unavailable[n] ?? ""]),
          ];
        }),
      ];
    }),
  ]);
}

// ---- provenance and the bundle ---------------------------------------------------------------------------------------------

const entry = (rows: readonly { name: string; text: string }[], name: string): string | null => rows.find((r) => r.name === name)?.text ?? null;

export function provenanceJson(done: ComparisonSnapshot, scope: string, files: Readonly<Record<Member, string>>): string {
  const a = done.snapshots.A;
  const b = done.snapshots.B;
  const doc = a.doc;
  const members = MEMBERS.map((m) => {
    const snap = done.snapshots[m];
    const strategy = snap.doc.strategies[0]!;
    return {
      member: m, label: labelOf(done, m), strategy_kind: snap.request.kind,
      parameters_as_passed: snap.request.params.map(([name, value]) => ({ name, value })),
      parameters_as_run: Object.fromEntries(strategy.parameters.map((p) => [p.name, p.text])),
      derived: Object.fromEntries(strategy.derived.map((p) => [p.name, p.text])),
      strategy_id: strategy.strategyId, run_id: snap.doc.runId, configuration_id: done.configIds[m], result_sha256: snap.doc.resultSha256 ?? null,
      schema_version: snap.doc.schemaVersion, signal_requests_full_run: snap.doc.signals.length, warnings: snap.doc.warnings.map((w) => w.code),
      raw_result_file: files[m],
    };
  });
  const full = summarize(done.model, done.model.total, null);
  const document = {
    export: "strategy_lab.comparison_provenance", export_version: "1.0", scope, note: COMPARE_NOTE,
    produced_by: "Engine console, Strategies > Compare (signal replay through the real strategy_lab_replay)",
    comparison_number_in_session: done.id, comparison_id: done.comparisonId, finished_at: done.finishedAt,
    dataset: {
      name: a.meta.dataset.name, kind: a.meta.dataset.kind, synthetic_fixture: a.meta.dataset.synthetic, provenance: a.meta.dataset.provenance,
      sha256: doc.dataset.sha256, bytes: doc.dataset.bytes, rows: doc.dataset.rows, format: doc.dataset.format,
    },
    shared_by_both_members: {
      dataset_sha256: doc.dataset.sha256, event_count: doc.events.length,
      symbols_allowlist: a.request.params.find(([n]) => n === "symbols")?.[1] ?? null,
      symbols_allowlist_is_identical: (a.request.params.find(([n]) => n === "symbols")?.[1] ?? null) === (b.request.params.find(([n]) => n === "symbols")?.[1] ?? null),
      replay_order: entry(doc.context, "replay_order"), clock: entry(doc.context, "clock"), bus_fault: doc.busFault,
      independent_engine_runs: "each member ran in its own fresh engine; signal ids repeat across them",
    },
    agreement_full_run: {
      definition: AGREEMENT_DEFINITION, comparable_events: full.agreement.comparable, agree: full.agreement.agree, differ: full.agreement.differ,
      not_comparable: full.agreement.notComparable, request_differences: full.agreement.requestDifferences, verdict_differences: full.agreement.verdictDifferences,
    },
    members,
    executable: {
      file_name: a.meta.runner.name, sha256: a.meta.runner.sha256, size_bytes: a.meta.runner.size, tool: entry(doc.provenance, "tool"),
      version: entry(doc.provenance, "project_version"), compiler: entry(doc.provenance, "compiler"), build: entry(doc.provenance, "build"),
    },
  };
  return JSON.stringify(document, null, 2) + "\n";
}

const ids = (done: ComparisonSnapshot): string => MEMBERS.map((m) => safeName(done.snapshots[m].doc.runId).slice(0, 8)).join("_");

export const bundleName = (done: ComparisonSnapshot): string => `strategy_lab_compare_${ids(done)}_full_run.zip`;

export function csvName(kind: "signals" | "summary" | "agreement" | "events", scope: ExportScope, done: ComparisonSnapshot, cursor: number, symbol: string | null): string {
  const part = scope === "full_run" ? "full_run" : `prefix_${cursor}of${done.model.total}_${symbol ? safeName(symbol) : "all"}`;
  return `strategy_lab_compare_${ids(done)}_${kind}_${part}.csv`;
}

/** The FULL RUN bundle: provenance, the labelled CSVs, and both original run results byte for byte. */
export function bundleZip(done: ComparisonSnapshot): Uint8Array {
  const scope = scopeText(done, "full_run", done.model.total, null);
  const files = { A: `A_${safeName(done.snapshots.A.doc.runId)}_run.json`, B: `B_${safeName(done.snapshots.B.doc.runId)}_run.json` } as const;
  const text: Record<string, string> = {
    "comparison_provenance.json": provenanceJson(done, scope, files),
    "comparison_summary_full_run.csv": summaryCsv(done, "full_run", done.model.total, null),
    "comparison_agreement_full_run.csv": agreementCsv(done, "full_run", done.model.total, null),
    "comparison_signals_full_run.csv": signalsCsv(done, "full_run", done.model.total, null),
    "comparison_events_full_run.csv": eventsCsv(done, "full_run", done.model.total, null),
    [files.A]: done.snapshots.A.raw, // the tool's own output, never re-serialized
    [files.B]: done.snapshots.B.raw,
  };
  return zipStore(Object.keys(text).sort().map((name) => ({ name, data: encode(text[name]!) })));
}
