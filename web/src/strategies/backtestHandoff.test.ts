import { describe, expect, it } from "vitest";
import { CARRIES_TEXT, destinationText, draftFromRecorded, draftFromRequest, handoffBlocker, isCarried, NOT_CARRIED_TEXT, PLAIN_DECIMAL, toCarried } from "./backtestHandoff";
import { parseCatalog, specOf } from "./catalog";
import { defaultValues, newDraft, validateDraft, withValue } from "./draft";
import { catalogText } from "./testClient";

const catalog = parseCatalog(catalogText);
const sma = specOf(catalog, "sma_crossover")!;
const mr = specOf(catalog, "mean_reversion")!;
const COVERAGE = ["AAPL", "MSFT", "SPY"];
const ready = withValue(withValue(newDraft(sma), "symbols", "AAPL, SPY"), "short_window", "2");

describe("carrying a draft to the backtest form", () => {
  it("carries every setting as the text typed", () => {
    expect(toCarried(ready)).toEqual({ kind: "sma_crossover", strategy_id: "sma_crossover", symbols: ["AAPL", "SPY"], quantity: "1", short_window: "2", long_window: "20" });
    const m = withValue(withValue(withValue(newDraft(mr), "symbols", "SPY"), "entry_threshold", "2.2"), "rearm_threshold", "0.4");
    expect(toCarried(m)).toEqual({ kind: "mean_reversion", strategy_id: "mean_reversion", symbols: ["SPY"], quantity: "1", lookback: "20", entry: "2.2", rearm: "0.4" });
  });

  it("is allowed when the draft is valid, accepted by the engine and covered by recorded data", () => {
    expect(handoffBlocker(ready, sma, 0, undefined, COVERAGE)).toBeUndefined();
  });

  it("says why not, at the control", () => {
    expect(handoffBlocker(ready, undefined, 0, undefined, COVERAGE)).toMatch(/Choose a strategy/);
    expect(handoffBlocker({ kind: "ml", values: {} }, { ...sma, kind: "ml", title: "Custom ML strategy" }, 0, undefined, COVERAGE)).toMatch(/supports only/);
    expect(handoffBlocker(ready, sma, 2, undefined, COVERAGE)).toMatch(/Fix the highlighted/);
    expect(handoffBlocker(ready, sma, 0, "long_window must be greater", COVERAGE)).toMatch(/engine rejected/);
    expect(handoffBlocker(withValue(ready, "symbols", "AAPL,NVDA,TSLA"), sma, 0, undefined, COVERAGE)).toMatch(/no recorded market data for NVDA, TSLA/);
  });

  it("refuses a share count a JavaScript number cannot hold exactly instead of silently rounding it", () => {
    const big = withValue(ready, "requested_quantity", "9223372036854775807");
    expect(handoffBlocker(big, sma, 0, undefined, COVERAGE)).toMatch(/exact only up to 9,007,199,254,740,991/);
    expect(handoffBlocker(withValue(ready, "requested_quantity", "9007199254740991"), sma, 0, undefined, COVERAGE)).toBeUndefined();
    expect(handoffBlocker(withValue(ready, "requested_quantity", "9007199254740993"), sma, 0, undefined, COVERAGE)).toMatch(/exact only up to/);
  });

  it("does not block on coverage that is not known yet", () => {
    expect(handoffBlocker(ready, sma, 0, undefined, undefined)).toBeUndefined();
  });

  it("recognises only a well-formed handoff", () => {
    const state = { fromStrategies: toCarried(ready)!, label: "x" };
    expect(isCarried(state)).toBe(true);
    for (const bad of [null, undefined, {}, { fromStrategies: { kind: "ml" }, label: "x" }, { fromStrategies: { ...state.fromStrategies, symbols: "AAPL" }, label: "x" }, { fromStrategies: state.fromStrategies }]) expect(isCarried(bad)).toBe(false);
  });
});

describe("loading a recorded run's strategy as a draft", () => {
  it("copies what the run recorded and keeps the catalog defaults for anything it did not", () => {
    const draft = draftFromRecorded(sma, { kind: "sma_crossover", strategy_id: "sma_10_40", symbols: ["AAPL", "NVDA"], requested_quantity: 200, short_window: 10, long_window: 40 }, defaultValues(sma));
    expect(draft).toEqual({ kind: "sma_crossover", values: { strategy_id: "sma_10_40", short_window: "10", long_window: "40", requested_quantity: "200", symbols: "AAPL,NVDA" } });
    const m = draftFromRecorded(mr, { kind: "mean_reversion", strategy_id: "mean_reversion", symbols: ["SPY"], requested_quantity: 40, lookback: 30, entry_threshold: 2.2, rearm_threshold: 0.4 }, defaultValues(mr));
    expect(m.values).toMatchObject({ lookback: "30", entry_threshold: "2.2", rearm_threshold: "0.4", requested_quantity: "40", symbols: "SPY" });
  });
});

describe("what the form cannot hold exactly is refused with its reason, never rewritten or dropped", () => {
  const meanReversion = (entry: string, rearm: string) => withValue(withValue(withValue(newDraft(mr), "symbols", "SPY"), "entry_threshold", entry), "rearm_threshold", rearm);

  it("accepts the decimals the form reads, and nothing else it would read differently", () => {
    for (const text of ["2", "0.5", ".5", "2.25", "10"]) {
      expect(PLAIN_DECIMAL.test(text), text).toBe(true);
      expect(handoffBlocker(meanReversion(text, "0.1"), mr, 0, undefined, COVERAGE), text).toBeUndefined();
    }
  });

  it("refuses a decimal the Lab accepts but the form would read as NaN (a trailing point, an exponent)", () => {
    for (const [entry, rearm, bad, label] of [["2.", "0.5", "2.", "Entry threshold"], ["2.0e0", "0.5", "2.0e0", "Entry threshold"], ["3", "1e-1", "1e-1", "Re-arm threshold"], ["3", "5E-1", "5E-1", "Re-arm threshold"]] as const) {
      const draft = meanReversion(entry, rearm);
      expect(validateDraft(mr, draft), `${bad} is legal in the Lab`).toEqual([]);
      expect(PLAIN_DECIMAL.test(bad)).toBe(false);
      expect(handoffBlocker(draft, mr, 0, undefined, COVERAGE)).toBe(`The backtest form reads plain decimals such as 2 or 0.5, not “${bad}” (${label}). Rewrite it that way first.`);
    }
  });

  it("refuses any whole number beyond 2^53-1 (window, lookback, quantity) instead of rounding it", () => {
    const big = "9007199254740993";
    expect(handoffBlocker(withValue(ready, "long_window", big), sma, 0, undefined, COVERAGE)).toMatch(/Long window as a JavaScript number.*9,007,199,254,740,991/);
    expect(handoffBlocker(withValue(ready, "short_window", big), sma, 0, undefined, COVERAGE)).toMatch(/Short window/);
    expect(handoffBlocker(withValue(meanReversion("2", "0.5"), "lookback", big), mr, 0, undefined, COVERAGE)).toMatch(/Lookback/);
    expect(handoffBlocker(withValue(ready, "long_window", "9007199254740991"), sma, 0, undefined, COVERAGE)).toBeUndefined();
  });

  it("names a parameter the form has no field for, so a future engine setting is never silently dropped", () => {
    const wider = { ...sma, params: [...sma.params, { ...sma.params[0]!, name: "cooldown_bars" }] };
    expect(handoffBlocker(withValue(ready, "cooldown_bars", "3"), wider, 0, undefined, COVERAGE)).toMatch(/no field for cooldown_bars.*without losing it/);
    expect(handoffBlocker(ready, sma, 0, undefined, COVERAGE)).toBeUndefined();
  });

  it("refuses an id the form would trim", () => {
    expect(handoffBlocker(withValue(ready, "strategy_id", " crossover"), sma, 0, undefined, COVERAGE)).toMatch(/trims spaces around the strategy id/);
  });

  it("carries every parameter of both kinds exactly as written (nothing substituted)", () => {
    const m = withValue(withValue(withValue(withValue(meanReversion("2.5", "0.25"), "lookback", "30"), "requested_quantity", "40"), "strategy_id", "mr_a"), "symbols", "SPY, AAPL");
    expect(toCarried(m)).toEqual({ kind: "mean_reversion", strategy_id: "mr_a", symbols: ["SPY", "AAPL"], quantity: "40", lookback: "30", entry: "2.5", rearm: "0.25" });
    const c = withValue(withValue(withValue(ready, "long_window", "7"), "requested_quantity", "250"), "strategy_id", "x_1");
    expect(toCarried(c)).toEqual({ kind: "sma_crossover", strategy_id: "x_1", symbols: ["AAPL", "SPY"], quantity: "250", short_window: "2", long_window: "7" });
  });
});

describe("which settings a completed comparison ran with", () => {
  it("rebuilds exactly the parameter text that was sent", () => {
    const request = { kind: "sma_crossover", params: [["strategy_id", "cross_a"], ["short_window", "3"], ["long_window", "5"], ["requested_quantity", "10"], ["symbols", "AAPL,SPY"]] as [string, string][], dataset: { kind: "builtin" as const, name: "x.csv", sha256: "a".repeat(64) } };
    const draft = draftFromRequest(request);
    expect(toCarried(draft)).toEqual({ kind: "sma_crossover", strategy_id: "cross_a", symbols: ["AAPL", "SPY"], quantity: "10", short_window: "3", long_window: "5" });
    expect(handoffBlocker(draft, sma, validateDraft(sma, draft).length, undefined, COVERAGE)).toBeUndefined();
  });
});

describe("what the person is told about the destination", () => {
  it("says the form runs the demo simulator and does not replay the Strategies dataset, by name", () => {
    const text = destinationText("demo", "uploaded.csv");
    expect(text).toMatch(/in-browser demo simulator/);
    expect(text).toMatch(/its own synthetic market data \(Demo data\)/);
    expect(text).toMatch(/does not replay “uploaded\.csv”/);
    expect(text).toMatch(/cannot be tied to the signals seen in Strategies/);
  });

  it("does not claim the demo simulator when an engine API is connected, and never claims the dataset is replayed", () => {
    const text = destinationText("engine", "uploaded.csv");
    expect(text).toMatch(/engine API this console is connected to/);
    expect(text).not.toMatch(/demo simulator over its own synthetic/);
    expect(text).toMatch(/does not replay/);
  });

  it("is honest before the engine has answered, and without a dataset name", () => {
    const text = destinationText(undefined);
    expect(text).toMatch(/whatever engine this console is connected to/);
    expect(text).toMatch(/does not replay the dataset used in Strategies/);
  });

  it("states what travels and what does not", () => {
    expect(CARRIES_TEXT).toMatch(/kind, id, symbols, shares per signal and its parameters/);
    expect(NOT_CARRIED_TEXT).toMatch(/the dataset and its time range.*other configuration.*review them/);
  });
});
