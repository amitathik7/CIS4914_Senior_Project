import { describe, expect, it } from "vitest";
import { parseSymbols } from "./symbols";

describe("parseSymbols", () => {
  it("upper-cases, splits on commas and spaces, and drops repeats", () => {
    expect(parseSymbols(" aapl, msft  spy,AAPL ")).toEqual({ symbols: ["AAPL", "MSFT", "SPY"] });
    expect(parseSymbols("brk.b")).toEqual({ symbols: ["BRK.B"] });
    expect(parseSymbols("")).toEqual({ symbols: [] });
  });

  it("names the first token that is not a symbol", () => {
    expect(parseSymbols("AAPL $$$ MSFT")).toEqual({ symbols: ["AAPL"], invalid: "$$$" });
    expect(parseSymbols("1ABC")).toEqual({ symbols: [], invalid: "1ABC" });
  });
});
