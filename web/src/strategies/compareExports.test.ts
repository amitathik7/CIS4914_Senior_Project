import { describe, expect, it } from "vitest";
import {
  agreementCsv, bundleName, bundleZip, csvName, eventsCsv, labelOf, provenanceJson, scopeText, signalsCsv, summaryCsv,
} from "./compareExports";
import { nineBars, comparisonOf } from "./testCompare";
import type { Spec } from "./testDoc";

/** Split CSV text (CRLF rows, quoted cells) into rows of cells; enough for the exports' own output. */
function parse(text: string): string[][] {
  const rows: string[][] = [];
  let row: string[] = [];
  let cell = "";
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i]!;
    if (quoted) {
      if (c === '"' && text[i + 1] === '"') { cell += '"'; i++; }
      else if (c === '"') quoted = false;
      else cell += c;
    } else if (c === '"') quoted = true;
    else if (c === ",") { row.push(cell); cell = ""; }
    else if (c === "\r") continue;
    else if (c === "\n") { row.push(cell); rows.push(row); row = []; cell = ""; }
    else cell += c;
  }
  return rows;
}

const nine = () => {
  const { a, b } = nineBars();
  return comparisonOf(a, b);
};

/** The exact values the contract protects: an int64-extreme price, a quantity and signal ids beyond 2^53. */
function bigValues() {
  const price = "9223372036854.775807";
  const a: Spec[] = [
    { symbol: "AAPL", minute: 0, price }, { symbol: "AAPL", minute: 1, price, signals: [[0, "9007199254740993", "buy", "9223372036854775807"]], reason: "crossover_buy" },
  ];
  const b: Spec[] = [
    { symbol: "AAPL", minute: 0, price }, { symbol: "AAPL", minute: 1, price, signals: [[0, "9007199254740992", "buy", "9223372036854775806"]], reason: "crossover_buy" },
  ];
  return comparisonOf(a, b);
}

describe("signals export", () => {
  it("writes both sides' requests with member-scoped keys, because raw ids repeat across the runs", () => {
    const done = nine();
    const rows = parse(signalsCsv(done, "full_run", 9, null));
    const head = rows[0]!;
    const col = (name: string) => head.indexOf(name);
    expect(rows).toHaveLength(1 + 3);
    const body = rows.slice(1);
    expect(body.map((r) => [r[col("member")], r[col("signal_id")], r[col("member_signal_key")]])).toEqual([
      ["A", "1", "A/0123456789abcdef:1"], ["B", "1", "B/0123456789abcdef:1"], ["A", "2", "A/0123456789abcdef:2"],
    ]);
    expect(new Set(body.map((r) => r[col("member_signal_key")])).size).toBe(3);
    expect(body[0]![col("member_label")]).toBe("A: Title (short_window 2, long_window 3)");
    expect(head).toContain("metadata.trigger");
  });

  it("states its scope on every row and in the file name, and the prefix holds only what is revealed", () => {
    const done = nine();
    const full = parse(signalsCsv(done, "full_run", 5, "AAPL")).slice(1);
    const prefix = parse(signalsCsv(done, "visible_prefix", 5, null)).slice(1);
    expect(full).toHaveLength(3);
    expect(full.every((r) => r[0] === "full_run (all 9 events, all symbols, both configurations)")).toBe(true);
    expect(prefix).toHaveLength(1); // only A's first Buy is within events 1-5
    expect(prefix[0]![0]).toBe("visible_prefix (events 1-5 of 9; symbol filter: all symbols; both configurations)");
    expect(parse(signalsCsv(done, "visible_prefix", 9, "MSFT"))).toHaveLength(1); // the header alone
    expect(csvName("signals", "full_run", done, 5, null)).toBe("strategy_lab_compare_01234567_01234567_signals_full_run.csv");
    expect(csvName("signals", "visible_prefix", done, 5, "AAPL")).toBe("strategy_lab_compare_01234567_01234567_signals_prefix_5of9_AAPL.csv");
    expect(csvName("events", "visible_prefix", done, 0, null)).toBe("strategy_lab_compare_01234567_01234567_events_prefix_0of9_all.csv");
  });

  it("preserves int64 quantities, prices and 64-bit ids exactly (no Number round trip)", () => {
    const done = bigValues();
    const rows = parse(signalsCsv(done, "full_run", 2, null));
    const col = (name: string) => rows[0]!.indexOf(name);
    expect(rows.slice(1).map((r) => [r[col("signal_id")], r[col("requested_quantity")], r[col("member_signal_key")]])).toEqual([
      ["9007199254740993", "9223372036854775807", "A/0123456789abcdef:9007199254740993"],
      ["9007199254740992", "9223372036854775806", "B/0123456789abcdef:9007199254740992"],
    ]);
    const events = parse(eventsCsv(done, "full_run", 2, null));
    expect(events[1]![events[0]!.indexOf("price")]).toBe("9223372036854.775807");
    expect(JSON.parse(`[${"9223372036854775807"}]`)[0]).not.toBe(9223372036854775807n); // the trap: a Number loses it
  });
});

describe("summary and agreement exports", () => {
  it("labels each side's counts and keeps warming up apart from evaluated", () => {
    const done = nine();
    const rows = parse(summaryCsv(done, "full_run", 9, null));
    const head = rows[0]!;
    const get = (r: string[], name: string) => r[head.indexOf(name)];
    expect(rows).toHaveLength(3);
    expect(get(rows[1]!, "member")).toBe("A");
    expect([get(rows[1]!, "buy_requests"), get(rows[1]!, "sell_requests"), get(rows[1]!, "evaluated"), get(rows[1]!, "warming_up")]).toEqual(["1", "1", "7", "2"]);
    expect([get(rows[2]!, "buy_requests"), get(rows[2]!, "sell_requests"), get(rows[2]!, "evaluated"), get(rows[2]!, "warming_up")]).toEqual(["1", "0", "5", "4"]);
    expect(head).toContain("no_diagnostics");
  });

  it("discloses the denominator and the definition with the agreement figures, per scope", () => {
    const done = nine();
    const full = parse(agreementCsv(done, "full_run", 9, null));
    expect(full[1]!.slice(1, 7)).toEqual(["5", "2", "3", "4", "3", "2"]);
    expect(full[1]![7]).toMatch(/^Comparable events are rows that both configurations evaluated/);
    const prefix = parse(agreementCsv(done, "visible_prefix", 5, null));
    expect(prefix[1]![0]).toMatch(/^visible_prefix \(events 1-5 of 9/);
    expect(prefix[1]!.slice(1, 5)).toEqual(["1", "0", "1", "4"]);
  });
});

describe("events export", () => {
  it("has one row per recorded event, each side's own verdict and request, and how the two relate", () => {
    const done = nine();
    const rows = parse(eventsCsv(done, "full_run", 9, null));
    const head = rows[0]!;
    const col = (name: string) => head.indexOf(name);
    expect(rows).toHaveLength(10);
    expect(rows.slice(1).map((r) => r[col("relation")])).toEqual(["not_comparable", "not_comparable", "not_comparable", "not_comparable", "differ", "differ", "agree", "differ", "agree"]);
    const fifth = rows[5]!;
    expect([fifth[col("A_action")], fifth[col("B_action")], fifth[col("A_signal_keys")], fifth[col("B_signal_keys")]]).toEqual(["buy", "none", "A/0123456789abcdef:1", ""]);
    expect(rows[1]![col("A_verdict")]).toBe("warming_up");
    expect(rows[1]![col("A_window_fill")]).toBe("1");
    expect(parse(eventsCsv(done, "visible_prefix", 3, null))).toHaveLength(4);
  });

  it("leaves an unavailable value empty rather than 0", () => {
    const a: Spec[] = [{ symbol: "AAPL", minute: 0, unavailable: { z_score: "constant_window" } }];
    const done = comparisonOf(a, a);
    const rows = parse(eventsCsv(done, "full_run", 1, null));
    const col = (name: string) => rows[0]!.indexOf(name);
    expect(rows[1]![col("A.z_score")]).toBe("");
    expect(rows[1]![col("A.z_score_unavailable")]).toBe("constant_window");
  });
});

describe("provenance and the bundle", () => {
  it("records both configurations, the dataset, the engine file and the scope", () => {
    const done = nine();
    const json = JSON.parse(provenanceJson(done, scopeText(done, "full_run", 9, null), { A: "A.json", B: "B.json" }));
    expect(json.export).toBe("strategy_lab.comparison_provenance");
    expect(json.scope).toBe("full_run (all 9 events, all symbols, both configurations)");
    expect(json.comparison_id).toBe("3".repeat(64));
    expect(json.dataset).toMatchObject({ sha256: "a1".repeat(32), name: "x.csv", kind: "builtin", synthetic_fixture: true });
    expect(json.members.map((m: { member: string; configuration_id: string }) => [m.member, m.configuration_id])).toEqual([["A", "1".repeat(64)], ["B", "2".repeat(64)]]);
    expect(json.members[0].parameters_as_passed).toEqual([{ name: "strategy_id", value: "sma_crossover" }, { name: "short_window", value: "2" }, { name: "long_window", value: "3" }]);
    expect(json.members[1].parameters_as_passed.map((p: { value: string }) => p.value)).toEqual(["sma_crossover", "3", "5"]);
    expect(json.members[0].raw_result_file).toBe("A.json");
    expect(json.executable).toMatchObject({ sha256: "c3".repeat(32), file_name: "strategy_lab_replay.exe", tool: "strategy_lab_replay" });
    expect(json.agreement_full_run).toMatchObject({ comparable_events: 5, agree: 2, differ: 3, not_comparable: 4 });
    expect(json.shared_by_both_members).toMatchObject({ event_count: 9, replay_order: "file order", symbols_allowlist: null });
    expect(json.note).toMatch(/Signal requests only/);
  });

  it("contains each run exactly as the tool wrote it, and is byte-identical when exported again", () => {
    const done = nine();
    const zip = bundleZip(done);
    expect(Array.from(bundleZip(done))).toEqual(Array.from(zip));
    expect(bundleName(done)).toBe("strategy_lab_compare_01234567_01234567_full_run.zip");
    const view = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
    const count = view.getUint16(zip.length - 22 + 10, true);
    let at = view.getUint32(zip.length - 22 + 16, true);
    const files: Record<string, string> = {};
    for (let i = 0; i < count; i++) {
      const size = view.getUint32(at + 24, true);
      const nameLength = view.getUint16(at + 28, true);
      const offset = view.getUint32(at + 42, true);
      const name = new TextDecoder().decode(zip.subarray(at + 46, at + 46 + nameLength));
      const start = offset + 30 + nameLength;
      files[name] = new TextDecoder().decode(zip.subarray(start, start + size));
      at += 46 + nameLength;
    }
    expect(Object.keys(files)).toEqual([
      "A_0123456789abcdef_run.json", "B_0123456789abcdef_run.json", "comparison_agreement_full_run.csv", "comparison_events_full_run.csv",
      "comparison_provenance.json", "comparison_signals_full_run.csv", "comparison_summary_full_run.csv",
    ]);
    expect(files["A_0123456789abcdef_run.json"]).toBe(done.snapshots.A.raw);
    expect(files["B_0123456789abcdef_run.json"]).toBe(done.snapshots.B.raw);
    expect(files["comparison_signals_full_run.csv"]).toBe(signalsCsv(done, "full_run", 9, null));
    expect(JSON.parse(files["comparison_provenance.json"]!).scope).toMatch(/^full_run/);
  });

  it("names each side by its exact settings", () => {
    const done = nine();
    expect(labelOf(done, "A")).toBe("A: Title (short_window 2, long_window 3)");
    expect(labelOf(done, "B")).toBe("B: Title (short_window 3, long_window 5)");
  });
});
