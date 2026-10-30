import { describe, expect, it } from "vitest";
import { parseCatalog, specOf } from "./catalog";
import { csv, diagnosticsCsv, exportName, runDetailsJson, safeName, scopeText, signalsCsv } from "./exports";
import { makeSnapshot } from "./snapshot";
import { catalogText, replayResponse, SHA } from "./testClient";
import { at, makeDoc, type Spec } from "./testDoc";
import { buildModel } from "./cursor";
import { parseReplay } from "./replay";

const spec = specOf(parseCatalog(catalogText), "sma_crossover");

const SPECS: Spec[] = [
  { symbol: "AAPL", minute: 0, price: "9223372036854.775807", volume: "9223372036854775807", verdict: "warming_up", reason: "warming_up", unavailable: { short_sma: "warming_up" }, window: { fill: 1, remaining: 2, size: 3 } },
  { symbol: "MSFT", minute: 0, price: "10" },
  { symbol: "AAPL", minute: 1, price: "0.000001", indicators: { short_sma: "3.5", long_sma: "3" }, signals: [[0, "9007199254740993", "buy"]], reason: "crossover_buy" },
  { symbol: "MSFT", minute: 1, price: "9", signals: [[0, "9007199254740994", "sell"]], reason: "crossover_sell" },
];
const model = buildModel(parseReplay(makeDoc(SPECS)));
/** A minimal RFC 4180 reader (quoted cells may hold commas, quotes and line breaks), so the tests read files the way a spreadsheet would. */
function rows(text: string): string[][] {
  const out: string[][] = [];
  let row: string[] = [];
  let cell = "";
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i]!;
    if (quoted) {
      if (c === '"' && text[i + 1] === '"') {
        cell += '"';
        i++;
      } else if (c === '"') quoted = false;
      else cell += c;
    } else if (c === '"') quoted = true;
    else if (c === ",") {
      row.push(cell);
      cell = "";
    } else if (c === "\r" && text[i + 1] === "\n") {
      row.push(cell);
      out.push(row);
      row = [];
      cell = "";
      i++;
    } else cell += c;
  }
  return out;
}

describe("csv", () => {
  it("quotes only what needs it and ends every row with CRLF", () => {
    expect(csv([["a", 'b,"c"'], ["line\nbreak", ""]])).toBe('a,"b,""c"""\r\n"line\nbreak",\r\n');
  });
});

describe("signals export names its scope and stays exact", () => {
  it("visible prefix: exactly the signals the replay position and filter show", () => {
    const text = signalsCsv(model, "visible_prefix", 3, null);
    const [header, ...body] = rows(text);
    expect(header!.slice(0, 8)).toEqual(["export_scope", "signal_ref", "signal_id", "strategy_id", "event_index", "event_number", "symbol", "side"]);
    expect(body).toHaveLength(1);
    expect(body[0]![0]).toBe("visible_prefix (events 1-3 of 4; symbol filter: all symbols)");
    expect(body[0]!.slice(2, 8)).toEqual(["9007199254740993", "s1", "2", "3", "AAPL", "buy"]); // 0-based index, 1-based number, id exact
  });

  it("visible prefix with a symbol filter", () => {
    const body = rows(signalsCsv(model, "visible_prefix", 4, "MSFT")).slice(1);
    expect(body.map((r) => r[2])).toEqual(["9007199254740994"]);
    expect(body[0]![0]).toContain("symbol filter: MSFT");
  });

  it("full run: every signal whatever the position or filter, and the scope says so on each row", () => {
    const body = rows(signalsCsv(model, "full_run", 0, "AAPL")).slice(1);
    expect(body.map((r) => r[2])).toEqual(["9007199254740993", "9007199254740994"]);
    expect(body.every((r) => r[0] === "full_run (all 4 events, all symbols)")).toBe(true);
  });

  it("the position 0 prefix has a header and no rows", () => {
    expect(rows(signalsCsv(model, "visible_prefix", 0, null))).toHaveLength(1);
  });

  it("carries quantity, metadata and creation time exactly as recorded", () => {
    const [header, row] = rows(signalsCsv(model, "full_run", 0, null));
    const cell = (name: string) => row![header!.indexOf(name)];
    expect(cell("requested_quantity")).toBe("1");
    expect(cell("created_at_utc")).toBe(at(1));
    expect(cell("metadata.trigger")).toBe("test_buy");
  });
});

describe("diagnostics export", () => {
  it("full run lists every event with exact price text and unavailable reasons", () => {
    const [header, ...body] = rows(diagnosticsCsv(model, spec, "full_run", 0, null));
    expect(body).toHaveLength(4);
    const col = (name: string) => header!.indexOf(name);
    expect(body[0]![col("price")]).toBe("9223372036854.775807");
    expect(body[0]![col("short_sma")]).toBe("");
    expect(body[0]![col("short_sma_unavailable")]).toBe("warming_up");
    expect(body[0]![col("window_remaining")]).toBe("2");
    expect(body[2]![col("short_sma")]).toBe("3.5");
    expect(body[2]![col("signal_ids")]).toBe("9007199254740993");
    expect(body[2]![col("exchange_time_utc")]).toBe(at(1));
  });

  it("visible prefix is cut at the replay position and filtered", () => {
    expect(rows(diagnosticsCsv(model, spec, "visible_prefix", 2, null))).toHaveLength(3);
    expect(rows(diagnosticsCsv(model, spec, "visible_prefix", 4, "MSFT"))).toHaveLength(3);
    expect(rows(diagnosticsCsv(model, spec, "visible_prefix", 0, null))).toHaveLength(1);
  });
});

describe("run details and file names", () => {
  const request = { kind: "sma_crossover", params: [["short_window", "2"], ["symbols", "AAPL"]] as [string, string][], dataset: { kind: "builtin" as const, name: "sma_crossover.csv", sha256: SHA } };
  const snapshot = makeSnapshot(3, request, "Moving-average crossover", replayResponse({ strategy: { kind: request.kind, params: request.params }, dataset: { builtin: "sma_crossover" } }), "2026-10-09T12:00:00.000Z");

  it("records the configuration and dataset identity of the completed result", () => {
    const details = JSON.parse(runDetailsJson(snapshot, "2026-10-09T12:05:00.000Z"));
    expect(details).toMatchObject({
      export: "strategy_console.run_details/1", launch_number: 3, exported_at: "2026-10-09T12:05:00.000Z",
      run: { run_id: "0123456789abcdef", mode: "signal_replay" },
      request: { kind: "sma_crossover", params: [{ name: "short_window", text: "2" }, { name: "symbols", text: "AAPL" }] },
      dataset: { sha256: SHA, kind: "builtin", name: "sma_crossover.csv", rows: 9 },
      counts: { events: 9, buy_requests: 1, sell_requests: 1 },
    });
    expect(details.scope).toMatch(/full_run/);
  });

  it("names files by scope", () => {
    expect(exportName("signals", "visible_prefix", snapshot, 5)).toBe("signals_visible-prefix-events-1-to-5_0123456789abcdef.csv");
    expect(exportName("diagnostics", "full_run", snapshot, 5)).toBe("diagnostics_full-run_0123456789abcdef.csv");
    expect(safeName("../a b\\c?.csv")).toBe("_a_b_c_.csv");
    expect(scopeText(model, "full_run", 0, null)).toBe("full_run (all 4 events, all symbols)");
  });
});
