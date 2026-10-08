import { describe, expect, it } from "vitest";
import { formatDecimal, fromMicros, money, ratio, toMicros } from "./decimal";

describe("decimal", () => {
  it("round-trips exact text", () => {
    for (const t of ["150.02", "0.000001", "9223372036854.775807", "-3.5", "0", "100"]) {
      expect(fromMicros(toMicros(t))).toBe(t);
    }
  });

  it("accepts trailing zeros past six places and refuses real seventh digits", () => {
    expect(fromMicros(toMicros("1.2500000"))).toBe("1.25");
    expect(() => toMicros("0.0000001")).toThrow(RangeError);
    expect(() => toMicros("1e3")).toThrow(RangeError);
    expect(() => toMicros("")).toThrow(RangeError);
  });

  it("formats with grouping, rounding half away from zero and a true minus sign", () => {
    expect(money("1234567.895")).toBe("$1,234,567.90");
    expect(money("-0.004")).toBe("$0.00");
    expect(money("-12.345")).toBe("−$12.35");
    expect(money("5", true)).toBe("+$5.00");
    expect(formatDecimal("0.123456", { dp: 4 })).toBe("0.1235");
  });

  it("computes ratios without floating-point input", () => {
    expect(ratio("25000", "100000")).toBe(0.25);
    expect(ratio("1", "0")).toBeNull();
  });
});
