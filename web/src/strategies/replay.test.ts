import { describe, expect, it } from "vitest";
import { LabError } from "../api/lab";
import { parseCatalog } from "./catalog";
import { parseReplay } from "./replay";
import { catalogText } from "./testClient";
import { at, docObject, makeDoc, num, toText } from "./testDoc";

const refused = (text: string, kind: string, pattern?: RegExp) => {
  try {
    parseReplay(text);
  } catch (error) {
    expect(error).toBeInstanceOf(LabError);
    expect((error as LabError).kind).toBe(kind);
    if (pattern) expect((error as LabError).message).toMatch(pattern);
    return;
  }
  expect.unreachable("the document was accepted");
};

/** Edit a hand-built document object, then render it with exact number text. */
const tweak = (edit: (doc: any) => void, specs = [{ symbol: "AAPL", minute: 0 }, { symbol: "AAPL", minute: 1 }]) => {
  const doc = docObject(specs);
  edit(doc);
  return toText(doc);
};

describe("parseReplay: exact values", () => {
  const text = makeDoc(
    [
      { symbol: "AAPL", minute: 0, price: "9223372036854.775807", volume: "9223372036854775807", indicators: { z_score: "-1.7320508075688774" } },
      { symbol: "AAPL", minute: 1, price: "0.000001", signals: [[0, "9223372036854775807", "buy"]] },
    ],
  );

  it("keeps int64-extreme prices, quantities and identifiers as exact text", () => {
    const doc = parseReplay(text);
    expect(doc.events[0]!.price).toBe("9223372036854.775807");
    expect(doc.events[0]!.volume).toBe("9223372036854775807");
    expect(doc.events[1]!.price).toBe("0.000001");
    expect(doc.signals[0]!.id).toBe("9223372036854775807");
    expect(doc.signals[0]!.ref).toBe("0123456789abcdef:9223372036854775807");
    expect(doc.signals[0]!.quantity).toBe("1");
    expect(doc.events[1]!.results[0]!.signalIds).toEqual(["9223372036854775807"]);
  });

  it("keeps full timestamp precision as text", () => {
    const doc = parseReplay(makeDoc([{ symbol: "AAPL", minute: 0, time: "2026-01-05T14:30:00.123456789Z" }]));
    expect(doc.events[0]!.time).toBe("2026-01-05T14:30:00.123456789Z");
  });

  it("reads an indicator as a double and keeps the text the tool wrote", () => {
    const z = parseReplay(text).events[0]!.results[0]!.indicators.z_score!;
    expect(z.value).toBe(-1.7320508075688774);
    expect(z.text).toBe("-1.7320508075688774");
  });

  it("a missing indicator stays missing: it is listed as unavailable with its reason, never read as zero", () => {
    const doc = parseReplay(makeDoc([{ symbol: "AAPL", minute: 0, unavailable: { z_score: "constant_window" } }]));
    const result = doc.events[0]!.results[0]!;
    expect(result.indicators.z_score).toBeUndefined();
    expect(result.unavailable.z_score).toBe("constant_window");
  });

  it("represents an event the strategy reported nothing for", () => {
    const doc = parseReplay(makeDoc([{ symbol: "AAPL", minute: 0, available: false }]));
    expect(doc.events[0]!.results[0]).toMatchObject({ available: false, unavailableReason: "the strategy did not report this event" });
  });

  it("reads the run, dataset and configuration", () => {
    const doc = parseReplay(text);
    expect(doc.runId).toBe("0123456789abcdef");
    expect(doc.dataset.rows).toBe(2);
    expect(doc.strategies[0]!.parameters.find((p) => p.name === "short_window")?.text).toBe("2");
    expect(doc.strategies[0]!.parameterNumbers.long_window).toBe(3);
    expect(doc.resultSha256).toBe("00".repeat(32));
  });
});

describe("parseReplay: refusals", () => {
  it.each([
    ["an exponent in a price", (d: any) => (d.result.events[0].price = num("1e3")), /price must be an exact plain decimal/],
    ["a seventh decimal place", (d: any) => (d.result.events[0].price = num("0.0000001")), /exact plain decimal/],
    ["a fractional quantity", (d: any) => (d.result.signals[0] = { ...d.result.signals[0], requested_quantity: num("1.5") }), /whole number/],
  ])("%s", (_name, edit, pattern) => {
    const specs = [{ symbol: "AAPL", minute: 0, signals: [[0, "1", "buy"]] as [number, string, string][] }, { symbol: "AAPL", minute: 1 }];
    refused(tweak(edit, specs), "invalid_structure", pattern);
  });

  it("an unsupported major version", () => refused(tweak((d) => (d.schema_version = "2.0")), "unsupported_schema", /major version 1/));
  it("a different document", () => refused(tweak((d) => (d.schema = "strategy_lab.catalog")), "malformed_output", /strategy_lab.catalog/));
  it("a mode that is not signal_replay", () => refused(tweak((d) => (d.result.run.mode = "backtest")), "unsupported_schema", /signal_replay/));
  it("not JSON at all", () => refused("this is not json", "malformed_output"));
  it("a duplicate key", () => refused('{"schema":"strategy_lab.replay","schema":"x"}', "malformed_output", /duplicate/));
  it("NaN", () => refused('{"schema":"strategy_lab.replay","x":NaN}', "malformed_output"));

  it("an event index out of order", () => refused(tweak((d) => (d.result.events[1].index = num("5"))), "invalid_structure", /events\[1\]\.index/));
  it("a row count that disagrees with the events", () => refused(tweak((d) => (d.result.input.dataset.rows = num("5"))), "invalid_structure", /dataset reports 5 rows/));
  it("a result list shorter than the strategy list", () => refused(tweak((d) => (d.result.events[0].results = [])), "invalid_structure", /one per strategy/));

  const signalled = [{ symbol: "AAPL", minute: 0 }, { symbol: "MSFT", minute: 0, signals: [[0, "1", "buy"]] as [number, string, string][] }];
  it("a signal that names the wrong symbol", () => refused(tweak((d) => (d.result.signals[0].symbol = "AAPL"), signalled), "invalid_structure", /differs from the symbol/));
  it("a signal pointing past the last event", () => refused(tweak((d) => (d.result.signals[0].event_index = num("9")), signalled), "invalid_structure", /past the last event/));
  it("a signal whose event does not list it", () => refused(tweak((d) => (d.result.events[1].results[0].signal_ids = []), signalled), "invalid_structure", /does not list signal 1/));
  it("an event that lists a signal that does not exist", () => refused(tweak((d) => (d.result.signals = []), signalled), "invalid_structure", /not in the signals array/));
  it("a signal_ref that is not run-scoped", () => refused(tweak((d) => (d.result.signals[0].signal_ref = "other:1"), signalled), "invalid_structure", /run-scoped/));
  it("a summary that disagrees", () => refused(tweak((d) => (d.result.summary.signals = num("4")), signalled), "invalid_structure", /summary\.signals/));
  it("an unknown strategy on a signal", () => refused(tweak((d) => (d.result.signals[0].strategy_id = "zzz"), signalled), "invalid_structure", /unknown strategy/));
});

describe("parseCatalog", () => {
  it("builds the parameter forms' source from the tool's own document", () => {
    const catalog = parseCatalog(catalogText);
    const sma = catalog.strategies.find((s) => s.kind === "sma_crossover")!;
    expect(sma.params.map((p) => [p.name, p.type, p.defaultText])).toEqual([
      ["strategy_id", "string", "sma_crossover"], ["short_window", "uint", "5"], ["long_window", "uint", "20"],
      ["requested_quantity", "uint", "1"], ["symbols", "string_list", undefined],
    ]);
    expect(sma.params.find((p) => p.name === "symbols")!.required).toBe(true);
    expect(catalog.strategies.find((s) => s.kind === "ml")).toMatchObject({ available: false });
    expect(catalog.strategies.find((s) => s.kind === "ml")!.unavailableReason).toMatch(/Not implemented/);
  });

  it("refuses a whole-number default that is not an exact integer, and an unknown parameter type", () => {
    expect(() => parseCatalog(catalogText.replace('"default":5', '"default":5.5'))).toThrow(/exact integer/);
    expect(() => parseCatalog(catalogText.replace('"type":"uint"', '"type":"complex"'))).toThrow(/unknown parameter type/);
  });

  it("refuses a different major version", () => {
    expect(() => parseCatalog(catalogText.replace('"schema_version":"1.0"', '"schema_version":"2.1"'))).toThrow(/major version 1/);
  });
});

it("the hand-built document helper itself produces the shape the tool documents", () => {
  const doc = JSON.parse(makeDoc([{ symbol: "AAPL", minute: 0 }]));
  expect(doc.result.events[0].exchange_time).toBe(at(0));
});
