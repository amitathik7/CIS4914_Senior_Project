import { describe, expect, it } from "vitest";
import { decimalKey, intKey, timeKey } from "./sortKeys";

describe("exact sort keys", () => {
  it("order int64 text numerically with plain string comparison", () => {
    const ids = ["9223372036854775807", "9", "10", "100", "9223372036854775806", "9007199254740993", "9007199254740992"];
    expect([...ids].sort((a, b) => (intKey(a) < intKey(b) ? -1 : 1))).toEqual(["9", "10", "100", "9007199254740992", "9007199254740993", "9223372036854775806", "9223372036854775807"]);
    expect(Number("9223372036854775806")).toBe(Number("9223372036854775807")); // why a Number key would be wrong
  });

  it("order six-place decimals numerically, including values one millionth apart", () => {
    const prices = ["9223372036854.775807", "0.75", "0.5", "10", "9", "9223372036854.775806", "0.000001", "123456789.123456", "1.25"];
    expect([...prices].sort((a, b) => (decimalKey(a) < decimalKey(b) ? -1 : 1))).toEqual(["0.000001", "0.5", "0.75", "1.25", "9", "10", "123456789.123456", "9223372036854.775806", "9223372036854.775807"]);
  });

  it("treat a missing value as the smallest", () => {
    expect(intKey(undefined) < intKey("0")).toBe(true);
    expect(decimalKey(undefined) < decimalKey("0")).toBe(true);
  });

  it("order timestamps by instant even when the fractional digits differ in number (the dataset allows 0 to 9)", () => {
    const times = ["2026-01-05T14:30:00.5Z", "2026-01-05T14:30:00Z", "2026-01-05T14:30:00.000000001Z", "2026-01-05T14:30:00.25Z", "2026-01-05T14:29:59.999999999Z", "2026-01-05T14:30:01Z"];
    const chronological = ["2026-01-05T14:29:59.999999999Z", "2026-01-05T14:30:00Z", "2026-01-05T14:30:00.000000001Z", "2026-01-05T14:30:00.25Z", "2026-01-05T14:30:00.5Z", "2026-01-05T14:30:01Z"];
    expect([...times].sort((a, b) => (timeKey(a) < timeKey(b) ? -1 : 1))).toEqual(chronological);
    expect([...times].sort()).not.toEqual(chronological); // the trap: as text, "00Z" sorts after "00.5Z" because "Z" is greater than "."
    expect(timeKey("2026-01-05T14:30:00Z")).toBe("2026-01-05T14:30:00.000000000Z");
    expect(timeKey("2026-01-05T14:30:00.5Z")).toBe("2026-01-05T14:30:00.500000000Z");
  });

  it("leave a value that is not a timestamp as written, and a missing one smallest", () => {
    expect(timeKey("not a time")).toBe("not a time");
    expect(timeKey(undefined) < timeKey("2026-01-05T14:30:00Z")).toBe(true);
  });
});
