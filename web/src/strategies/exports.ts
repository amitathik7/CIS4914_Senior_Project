// Downloads for a completed result. Every export says which part of the run it covers: the VISIBLE PREFIX (what the replay
// position currently reveals, under the current symbol filter) or the FULL RUN. Building a file never starts the engine.
//
// Values are written exactly as recorded (prices and quantities as exact text, timestamps in UTC with nine fractional digits).
// A spreadsheet may treat a cell starting with =, +, - or @ as a formula; nothing is altered to prevent that.

import { counts, eventsThrough, resultOf, signalsThrough, type ReplayModel } from "./cursor";
import type { StrategySpec } from "./catalog";
import { SIDE_TEXT } from "./explain";
import type { Snapshot } from "./snapshot";

export type ExportScope = "visible_prefix" | "full_run";

const quote = (cell: string): string => (/[",\r\n]/.test(cell) ? `"${cell.replace(/"/g, '""')}"` : cell);
export const csv = (rows: readonly (readonly string[])[]): string => rows.map((r) => r.map(quote).join(",")).join("\r\n") + "\r\n";

export function scopeText(model: ReplayModel, scope: ExportScope, cursor: number, symbol: string | null): string {
  if (scope === "full_run") return `full_run (all ${model.total} events, all symbols)`;
  return `visible_prefix (events 1-${cursor} of ${model.total}; symbol filter: ${symbol ?? "all symbols"})`;
}

export function signalsCsv(model: ReplayModel, scope: ExportScope, cursor: number, symbol: string | null): string {
  const signals = scope === "full_run" ? model.signals : signalsThrough(model, cursor, symbol);
  const metadataKeys = [...new Set(signals.flatMap((s) => Object.keys(s.metadata)))].sort();
  const header = [
    "export_scope", "signal_ref", "signal_id", "strategy_id", "event_index", "event_number", "symbol", "side", "requested_quantity",
    "order_type", "limit_price", "created_at_utc", "bus_sequence", ...metadataKeys.map((k) => `metadata.${k}`),
  ];
  const scopeCell = scopeText(model, scope, cursor, symbol);
  return csv([
    header,
    ...signals.map((s) => [
      scopeCell, s.ref, String(s.id), s.strategyId, String(s.eventIndex), String(s.eventIndex + 1), s.symbol, s.side, s.quantity ?? "",
      s.orderType ?? "", s.limitPrice ?? "", s.createdAt, s.busSequence === undefined ? "" : String(s.busSequence),
      ...metadataKeys.map((k) => s.metadata[k] ?? ""),
    ]),
  ]);
}

export function diagnosticsCsv(model: ReplayModel, spec: StrategySpec | undefined, scope: ExportScope, cursor: number, symbol: string | null): string {
  const events = scope === "full_run" ? [...model.events] : eventsThrough(model, cursor, symbol);
  const names = [...new Set([...(spec?.indicators ?? []), ...events.flatMap((e) => Object.keys(resultOf(model, e).indicators))])];
  const states = [...new Set([...(spec?.states ?? []), ...events.flatMap((e) => Object.keys(resultOf(model, e).states))])];
  const header = [
    "export_scope", "event_index", "event_number", "source_line", "symbol", "exchange_time_utc", "type", "price", "diagnostics_available",
    "verdict", "reason", "action", "window_fill", "window_remaining", "window_size", ...names, ...names.map((n) => `${n}_unavailable`),
    ...states.map((s) => `state.${s}`), "signal_ids",
  ];
  const scopeCell = scopeText(model, scope, cursor, symbol);
  return csv([
    header,
    ...events.map((e) => {
      const r = resultOf(model, e);
      return [
        scopeCell, String(e.index), String(e.index + 1), e.sourceLine === undefined ? "" : String(e.sourceLine), e.symbol, e.time, e.type,
        e.price ?? "", r.available ? "true" : "false", r.verdict ?? "", r.reason ?? "", r.action ?? "",
        r.window?.fill === undefined ? "" : String(r.window.fill), r.window?.remaining === undefined ? "" : String(r.window.remaining),
        r.window ? String(r.window.size) : "", ...names.map((n) => r.indicators[n]?.text ?? ""), ...names.map((n) => r.unavailable[n] ?? ""),
        ...states.map((s) => r.states[s] ?? ""), r.signalIds.join(";"),
      ];
    }),
  ]);
}

/** The configuration and metadata of the completed result (not the replay itself: that is the Run JSON). */
export function runDetailsJson(snapshot: Snapshot, exportedAt: string): string {
  const { doc, meta, request } = snapshot;
  const strategy = doc.strategies[0]!;
  const total = counts(snapshot.model, snapshot.model.total, null);
  return JSON.stringify(
    {
      export: "strategy_console.run_details/1",
      scope: "full_run: configuration and metadata of the completed result. The replay itself is the Run JSON export.",
      exported_at: exportedAt,
      launch_number: snapshot.id,
      run: { run_id: doc.runId, mode: "signal_replay", result_sha256: doc.resultSha256 ?? null, schema_version: doc.schemaVersion },
      strategy: {
        kind: strategy.kind, strategy_id: strategy.strategyId, window_size: strategy.windowSize,
        parameters: Object.fromEntries(strategy.parameters.map((p) => [p.name, p.text])),
        derived: Object.fromEntries(strategy.derived.map((p) => [p.name, p.text])),
      },
      request: { kind: request.kind, params: request.params.map(([name, text]) => ({ name, text })) },
      dataset: { ...meta.dataset, rows: doc.dataset.rows, first_exchange_time: doc.dataset.firstTime, last_exchange_time: doc.dataset.lastTime,
        symbols: doc.dataset.symbols.map((s) => ({ symbol: s.symbol, bar_rows: s.barRows, trade_rows: s.tradeRows })) },
      runner: meta.runner,
      counts: { events: total.events, bars: total.bars, buy_requests: total.buys, sell_requests: total.sells },
      warnings: doc.warnings.map((w) => ({ code: w.code, message: w.message })),
    },
    null,
    2,
  ) + "\n";
}

export const safeName = (text: string): string => text.replace(/[^A-Za-z0-9._-]+/g, "_").replace(/^\.+/, "").slice(0, 80) || "export";

export function exportName(kind: "signals" | "diagnostics", scope: ExportScope, snapshot: Snapshot, cursor: number): string {
  const part = scope === "full_run" ? "full-run" : `visible-prefix-events-1-to-${cursor}`;
  return `${kind}_${part}_${safeName(snapshot.doc.runId)}.csv`;
}

export function download(filename: string, content: string | Uint8Array, type: string): void {
  const url = URL.createObjectURL(new Blob([content as BlobPart], { type }));
  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  document.body.append(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export { SIDE_TEXT };
