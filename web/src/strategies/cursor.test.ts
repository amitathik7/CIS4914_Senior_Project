import { describe, expect, it } from "vitest";
import {
  buildModel, clamp, compareIds, counts, eventsThrough, lowerBound, nextEvent, nextSignal, plotNumber, previousEvent, resultOf, revealed,
  selectedEvent, signalsOn, signalsThrough, trackFor,
} from "./cursor";
import { parseReplay } from "./replay";
import { makeDoc, type Spec } from "./testDoc";

// Two symbols with equal timestamps, interleaved row by row: AAPL buys at row 2, MSFT sells at row 3, AAPL sells at row 4.
const SPECS: Spec[] = [
  { symbol: "AAPL", minute: 0, price: "3", verdict: "warming_up", reason: "warming_up", unavailable: { short_sma: "warming_up" } },
  { symbol: "MSFT", minute: 0, price: "10", verdict: "warming_up", reason: "warming_up" },
  { symbol: "AAPL", minute: 1, price: "4", indicators: { short_sma: "3.5", long_sma: "3" }, signals: [[0, "1", "buy"]], reason: "crossover_buy" },
  { symbol: "MSFT", minute: 1, price: "9", signals: [[0, "2", "sell"]], reason: "crossover_sell", indicators: { short_sma: "9.5", long_sma: "9.75" } },
  { symbol: "AAPL", minute: 2, price: "2", signals: [[0, "3", "sell"]], reason: "crossover_sell" },
  { symbol: "MSFT", minute: 2, price: "9", type: "trade", verdict: "ignored", reason: "not_a_bar" },
];
const model = buildModel(parseReplay(makeDoc(SPECS)));

describe("navigation keeps the recorded order", () => {
  it("steps over every row in file order, including equal timestamps", () => {
    const seen: number[] = [];
    for (let c = 0; ; ) {
      const next = nextEvent(model, c, null);
      if (next === undefined) break;
      seen.push(model.events[next - 1]!.index);
      c = next;
    }
    expect(seen).toEqual([0, 1, 2, 3, 4, 5]);
    expect(model.events[0]!.time).toBe(model.events[1]!.time); // equal timestamps...
    expect(model.events.slice(0, 2).map((e) => e.symbol)).toEqual(["AAPL", "MSFT"]); // ...replayed in the order recorded
  });

  it("a symbol filter skips the other symbol's rows without moving the cursor on its own", () => {
    expect(nextEvent(model, 0, "MSFT")).toBe(2); // the MSFT row is event index 1 -> position 2
    expect(nextEvent(model, 2, "MSFT")).toBe(4);
    expect(nextEvent(model, 4, "MSFT")).toBe(6);
    expect(nextEvent(model, 6, "MSFT")).toBeUndefined();
    expect(previousEvent(model, 6, "MSFT")).toBe(4);
    expect(previousEvent(model, 2, "MSFT")).toBe(0); // nothing earlier for MSFT: back to the start
    expect(nextEvent(model, 0, "ZZZ")).toBeUndefined(); // a symbol with no rows
  });

  it("has correct boundaries", () => {
    expect(previousEvent(model, 0, null)).toBeUndefined();
    expect(nextEvent(model, 6, null)).toBeUndefined();
    expect(previousEvent(model, 1, null)).toBe(0);
    expect(clamp(model, -3)).toBe(0);
    expect(clamp(model, 99)).toBe(6);
    expect(clamp(model, 2.9)).toBe(2);
  });

  it("Next signal makes one stop per event and honours the filter", () => {
    expect(nextSignal(model, 0, null)).toBe(3); // AAPL buy: event index 2 -> position 3
    expect(nextSignal(model, 3, null)).toBe(4);
    expect(nextSignal(model, 4, null)).toBe(5);
    expect(nextSignal(model, 5, null)).toBeUndefined();
    expect(nextSignal(model, 0, "MSFT")).toBe(4);
    expect(nextSignal(model, 4, "MSFT")).toBeUndefined();
  });

  it("two strategies signalling on one event are one stop and are all listed", () => {
    const two = buildModel(parseReplay(makeDoc([
      { symbol: "AAPL", minute: 0, signals: [[0, "1", "buy"], [1, "1", "sell"]] },
      { symbol: "AAPL", minute: 1 },
    ], { strategies: ["a", "b"] })), "b");
    expect(two.strategyId).toBe("b");
    expect(signalsOn(two, 0).map((s) => [s.strategyId, s.side])).toEqual([["b", "sell"]]);
    expect(nextSignal(two, 0, null)).toBe(1);
    expect(nextSignal(two, 1, null)).toBeUndefined();
    expect(() => buildModel(two.doc, "nope")).toThrow(/no strategy/);
  });
});

describe("the visible prefix", () => {
  it("shows nothing at position 0 and only events 1..k at position k", () => {
    expect(selectedEvent(model, 0)).toBeUndefined();
    expect(selectedEvent(model, 3)!.index).toBe(2);
    expect(eventsThrough(model, 3, null).map((e) => e.index)).toEqual([0, 1, 2]);
    expect(eventsThrough(model, 3, "AAPL").map((e) => e.index)).toEqual([0, 2]);
    expect(signalsThrough(model, 3, null).map((s) => s.id)).toEqual(["1"]);
    expect(signalsThrough(model, 6, null).map((s) => s.id)).toEqual(["1", "2", "3"]);
    expect(signalsThrough(model, 6, "MSFT").map((s) => s.id)).toEqual(["2"]);
  });

  it("counts only the prefix and the filter, never the future", () => {
    const c = counts(model, 3, null);
    expect(c).toMatchObject({ events: 3, bars: 3, buys: 1, sells: 0 });
    expect(c.verdicts).toEqual({ ignored: 0, warming_up: 2, evaluated: 1, unavailable: 0 });
    expect(c.reasons.get("warming_up")).toBe(2);
    const all = counts(model, 6, null);
    expect(all).toMatchObject({ events: 6, bars: 5, buys: 1, sells: 2 });
    expect(all.verdicts.ignored).toBe(1);
    expect(counts(model, 6, "MSFT")).toMatchObject({ events: 3, sells: 1 });
    expect(counts(model, 0, null)).toMatchObject({ events: 0, buys: 0, sells: 0 });
  });

  it("a track holds every row but reveals only the prefix", () => {
    const track = trackFor(model, "AAPL", ["short_sma", "long_sma"]);
    expect(track.indexes).toEqual([0, 2, 4]);
    expect(track.close).toEqual([3, 4, 2]);
    expect(track.sides).toEqual([null, "buy", "sell"]);
    expect(revealed(track, 0)).toBe(0);
    expect(revealed(track, 1)).toBe(1);
    expect(revealed(track, 3)).toBe(2); // events 0 and 2 are inside the first three
    expect(revealed(track, 6)).toBe(3);
    expect(trackFor(model, "AAPL", ["short_sma", "long_sma"])).toBe(track); // cached
  });

  it("an indicator that does not exist is a gap (null), never zero", () => {
    const track = trackFor(model, "AAPL", ["short_sma"]);
    expect(track.indicators.short_sma).toEqual([null, 3.5, null]);
    expect(resultOf(model, model.events[0]!).unavailable.short_sma).toBe("warming_up");
  });

  it("a trade row has no effect on bar counts but is still a recorded event", () => {
    expect(model.events[5]!.type).toBe("trade");
    expect(trackFor(model, "MSFT", []).types).toEqual(["bar", "bar", "trade"]);
  });
});

describe("ids and numbers", () => {
  it("orders 64-bit identifiers exactly, as text", () => {
    expect(["9223372036854775807", "9", "10", "9223372036854775806"].sort(compareIds)).toEqual(["9", "10", "9223372036854775806", "9223372036854775807"]);
    expect(compareIds("9007199254740993", "9007199254740992")).toBeGreaterThan(0); // indistinguishable as doubles
  });

  it("orders a model's signals by event then exact id", () => {
    const big = buildModel(parseReplay(makeDoc([
      { symbol: "AAPL", minute: 0, signals: [[0, "9007199254740993", "buy"]] },
      { symbol: "AAPL", minute: 1, signals: [[0, "9007199254740992", "sell"]] },
    ])));
    expect(big.signals.map((s) => s.id)).toEqual(["9007199254740993", "9007199254740992"]);
  });

  it("plotNumber is the only price-to-Number conversion and reads text", () => {
    expect(plotNumber("9223372036854.775807")).toBeCloseTo(9223372036854.775, 0);
    expect(plotNumber(undefined)).toBeNull();
  });

  it("lowerBound finds the first element at or after a value", () => {
    expect([0, 1, 2, 3, 4].map((x) => lowerBound([1, 3, 3, 7], x))).toEqual([0, 0, 1, 1, 3]);
    expect(lowerBound([], 5)).toBe(0);
  });
});
