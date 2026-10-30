// DataTable sorts by comparing a number or a string. A price, a quantity or a 64-bit id must NOT go through a Number to be sorted
// (two different int64 values can be the same double), so these turn exact text into a string whose ordinary string order is the
// numeric order. Valid for non-negative values, which is all the lab writes for prices, quantities and ids.

const INT_WIDTH = 20; // enough for any int64 written in decimal

/** "9223372036854775807" -> a fixed-width, zero-padded key. */
export const intKey = (text: string | undefined): string => (text === undefined ? "" : text.padStart(INT_WIDTH, "0"));

/** "9223372036854.775807" -> "0000009223372036854.775807"; at most six decimal places, padded so "0.5" sorts before "0.75". */
export function decimalKey(text: string | undefined): string {
  if (text === undefined) return "";
  const [whole = "", fraction = ""] = text.split(".");
  return `${whole.padStart(INT_WIDTH, "0")}.${fraction.padEnd(6, "0")}`;
}

const TIMESTAMP = /^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?Z$/;

/**
 * "2026-01-05T14:30:00Z" and "...:00.5Z" -> the fraction padded to nine digits, so plain string order is time order. As written they
 * are not: the dataset allows 0 to 9 fractional digits, and "Z" sorts after ".". Anything that is not such a timestamp sorts as written.
 */
export function timeKey(text: string | undefined): string {
  if (text === undefined) return "";
  const match = TIMESTAMP.exec(text);
  return match ? `${match[1]}.${(match[2] ?? "").padEnd(9, "0")}Z` : text;
}
