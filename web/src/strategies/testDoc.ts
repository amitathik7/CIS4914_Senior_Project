// Test helper (no production code imports this): builds a schema-1.0 `strategy_lab.replay` document as JSON TEXT from hand-written
// event specs, so parser, cursor, chart-data and export logic can be tested on exact inputs the real fixtures do not contain
// (ids beyond 2^53, int64-extreme prices and quantities, equal timestamps across symbols, two strategies signalling on one event)
// without needing the executable. Numbers are written verbatim: pass num("9223372036854.775807") to get a bare JSON number.

const MARK = "@@";
export const num = (text: string): string => `${MARK}${text}${MARK}`;
export const toText = (value: unknown): string => JSON.stringify(value).replace(new RegExp(`"${MARK}([^"]+)${MARK}"`, "g"), "$1") + "\n";

export interface Spec {
  symbol: string;
  minute: number;
  price?: string | null;
  type?: string;
  verdict?: string;
  reason?: string;
  signals?: [slot: number, id: string, side: string, quantity?: string][];
  indicators?: Record<string, string>;
  unavailable?: Record<string, string>;
  time?: string;
  window?: { fill?: number; remaining?: number; size: number };
  volume?: string;
  available?: boolean;
}

export const at = (minute: number): string => `2026-01-05T14:${String(minute).padStart(2, "0")}:00.000000000Z`;

export interface Options {
  strategies?: string[];
  kind?: string;
  parameters?: Record<string, string>;
  runId?: string;
  datasetSha?: string;
  symbols?: string[];
}

export function docObject(specs: Spec[], options: Options = {}): Record<string, unknown> {
  const strategies = options.strategies ?? ["s1"];
  const runId = options.runId ?? "0123456789abcdef";
  const events: unknown[] = [];
  const signals: unknown[] = [];
  specs.forEach((spec, index) => {
    const time = spec.time ?? at(spec.minute);
    const results = strategies.map((strategyId, slot) => {
      const mine = (spec.signals ?? []).filter(([s]) => s === slot);
      if (spec.available === false) {
        return { strategy_id: strategyId, diagnostics_available: false, diagnostics_unavailable_reason: "the strategy did not report this event", signal_ids: mine.map(([, id]) => num(id)) };
      }
      for (const [, id, side, quantity] of mine) {
        signals.push({
          signal_id: num(id), signal_ref: `${runId}:${id}`, strategy_id: strategyId, event_index: index, bus_sequence: index * 2 + 2, symbol: spec.symbol,
          side, order_type: "market", requested_quantity: num(quantity ?? "1"), created_at: time, metadata: { trigger: `test_${side}` },
        });
      }
      return {
        strategy_id: strategyId, diagnostics_available: true, verdict: spec.verdict ?? "evaluated", reason: spec.reason ?? "same_side",
        action: mine[0]?.[2] ?? "none", window: spec.window ?? { fill: 3, remaining: 0, size: 3 },
        indicators: Object.fromEntries(Object.entries(spec.indicators ?? {}).map(([k, v]) => [k, num(v)])),
        unavailable: spec.unavailable ?? {}, states: { relation_before: "none" }, signal_ids: mine.map(([, id]) => num(id)),
      };
    });
    events.push({
      index, source_line: index + 2, symbol: spec.symbol, exchange_time: time, type: spec.type ?? "bar",
      ...(spec.price === null ? {} : { price: num(spec.price ?? "1") }), ...(spec.volume ? { volume: num(spec.volume) } : {}),
      bus_sequence: index * 2 + 1, results,
    });
  });
  const seen = options.symbols ?? [...new Set(specs.map((s) => s.symbol))];
  return {
    schema: "strategy_lab.replay", schema_version: "1.0",
    provenance: { tool: "strategy_lab_replay", project_version: "0.0.1", compiler: "test", build: "debug", result_sha256: "00".repeat(32) },
    result: {
      run: {
        run_id: runId, mode: "signal_replay", notice: "test notice", signal_id_scope: "ids are unique within the run",
        context: { engine: "e", event_bus: "b", clock: "c", replay_order: "file order", bus_fault: "none" },
      },
      input: {
        dataset: {
          name: "dataset.csv", format: "strategy_lab_bars_csv/1", sha256: options.datasetSha ?? "ab".repeat(32), bytes: num("100"), rows: num(String(specs.length)),
          first_exchange_time: specs[0] ? (specs[0].time ?? at(specs[0].minute)) : "", last_exchange_time: specs.length ? (specs[specs.length - 1]!.time ?? at(specs[specs.length - 1]!.minute)) : "",
          symbols: seen.map((symbol) => ({
            symbol, bar_rows: num(String(specs.filter((s) => s.symbol === symbol && (s.type ?? "bar") === "bar").length)),
            trade_rows: num(String(specs.filter((s) => s.symbol === symbol && s.type === "trade").length)),
            first_exchange_time: at(0), last_exchange_time: at(59),
          })),
        },
      },
      configuration: {
        max_rows: num("20000"), bus_fault: "none",
        strategies: strategies.map((strategyId) => ({
          kind: options.kind ?? "sma_crossover", strategy_id: strategyId, window_size: num("3"),
          parameters: Object.fromEntries(Object.entries({ strategy_id: strategyId, short_window: "2", long_window: "3", requested_quantity: "1", ...(options.parameters ?? {}) })
            .map(([k, v]) => [k, /^-?\d/.test(v) ? num(v) : v])),
          derived: { earliest_signal_accepted_bar: num("4") },
        })),
      },
      warnings: [], events, signals, publication_failures: [],
      engine: { events_received: num(String(specs.length)) },
      summary: { events: num(String(specs.length)), bars: num(String(specs.length)), signals: num(String(signals.length)) },
    },
  };
}

export const makeDoc = (specs: Spec[], options: Options = {}): string => toText(docObject(specs, options));
