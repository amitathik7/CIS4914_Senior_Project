import { describe, expect, it } from "vitest";
import { epochSeconds, etClock, etDay, etFull, splitTimestamp } from "./exactTime";

describe("exchange-time display of the tool's UTC timestamps", () => {
  it("converts to New York time and labels it", () => {
    expect(etClock("2026-01-05T14:30:00.000000000Z")).toBe("09:30:00"); // EST
    expect(etClock("2026-07-01T14:30:00.000000000Z")).toBe("10:30:00"); // EDT
    expect(etFull("2026-01-05T14:30:00.000000000Z")).toBe("Jan 5, 2026, 09:30:00.000000000 ET");
  });

  it("keeps every recorded fractional digit, nanoseconds included", () => {
    expect(etClock("2026-01-05T14:30:00.123456789Z")).toBe("09:30:00.123456789");
    expect(etClock("2026-01-05T14:30:00.500000000Z")).toBe("09:30:00.5");
    expect(etClock("2026-01-05T14:30:00.000000001Z")).toBe("09:30:00.000000001");
    expect(etFull("2026-01-05T14:30:00.000000001Z")).toBe("Jan 5, 2026, 09:30:00.000000001 ET");
  });

  it("accepts a timestamp with no fraction, and returns unparsable text unchanged", () => {
    expect(etClock("2026-01-05T14:30:00Z")).toBe("09:30:00");
    expect(etClock("not a time")).toBe("not a time");
    expect(splitTimestamp("2026-02-30T00:00:00Z")).toBeUndefined(); // not a real date
  });

  it("midnight is 00:00:00, never 24:00:00", () => {
    expect(etClock("2026-01-05T05:00:00Z")).toBe("00:00:00");
  });

  it("tells the ET calendar day (a late UTC row can belong to the previous ET day)", () => {
    expect(etDay("2026-01-06T02:00:00Z")).toBe("2026-01-05");
    expect(etDay("2026-01-05T14:30:00Z")).toBe("2026-01-05");
  });

  it("orders whole seconds for axis labels only", () => {
    expect(epochSeconds("2026-01-05T14:30:01.9Z")! - epochSeconds("2026-01-05T14:30:00Z")!).toBe(1);
  });
});
