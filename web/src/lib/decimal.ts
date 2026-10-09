// Exact decimal text <-> integer millionths, mirroring common::Decimal (int64 count of 10^-6).
// Display and arithmetic never go through a double; toNumber() exists only for chart coordinates.

const SCALE = 1_000_000n;
const PATTERN = /^(-)?(\d*)(?:\.(\d*))?$/;

export function toMicros(text: string): bigint {
  const m = PATTERN.exec(text.trim());
  if (!m || (m[2] === "" && (m[3] ?? "") === "")) throw new RangeError(`not a decimal: "${text}"`);
  const frac = (m[3] ?? "").replace(/0+$/, "");
  if (frac.length > 6) throw new RangeError(`more than 6 decimal places: "${text}"`);
  const value = BigInt(m[2] || "0") * SCALE + BigInt(frac.padEnd(6, "0") || "0");
  return m[1] ? -value : value;
}

export function fromMicros(micros: bigint | number): string {
  const v = typeof micros === "bigint" ? micros : BigInt(Math.round(micros));
  const neg = v < 0n;
  const abs = neg ? -v : v;
  const whole = abs / SCALE;
  const frac = (abs % SCALE).toString().padStart(6, "0").replace(/0+$/, "");
  return `${neg ? "-" : ""}${whole}${frac ? `.${frac}` : ""}`;
}

export function toNumber(text: string): number {
  return Number(toMicros(text)) / 1e6;
}

// Round half away from zero to `dp` places (0..6), returning sign, whole part and padded fraction.
function roundParts(micros: bigint, dp: number) {
  const step = 10n ** BigInt(6 - dp);
  const neg = micros < 0n;
  const abs = neg ? -micros : micros;
  const rounded = ((abs + step / 2n) / step) * step;
  const whole = rounded / SCALE;
  const frac = ((rounded % SCALE) / step).toString().padStart(dp, "0");
  return { neg: neg && rounded !== 0n, whole, frac: dp > 0 ? frac : "" };
}

function group(whole: bigint): string {
  return whole.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ",");
}

export interface FormatOptions {
  dp?: number;
  currency?: boolean;
  signed?: boolean; // always show + for positive values
}

export function formatDecimal(text: string, { dp = 2, currency = false, signed = false }: FormatOptions = {}): string {
  const { neg, whole, frac } = roundParts(toMicros(text), dp);
  const sign = neg ? "−" : signed && (whole > 0n || /[1-9]/.test(frac)) ? "+" : "";
  return `${sign}${currency ? "$" : ""}${group(whole)}${frac ? `.${frac}` : ""}`;
}

export const money = (text: string, signed = false) => formatDecimal(text, { dp: 2, currency: true, signed });
export const price = (text: string) => formatDecimal(text, { dp: 2 });

export function sign(text: string): -1 | 0 | 1 {
  const v = toMicros(text);
  return v > 0n ? 1 : v < 0n ? -1 : 0;
}

export function add(a: string, b: string): string {
  return fromMicros(toMicros(a) + toMicros(b));
}

export function sub(a: string, b: string): string {
  return fromMicros(toMicros(a) - toMicros(b));
}

// value / limit as a plain ratio for meters; null when the limit is zero.
export function ratio(value: string, limit: string): number | null {
  const l = toMicros(limit);
  return l === 0n ? null : Number((toMicros(value) * 1_000_000n) / l) / 1e6;
}

export function mul(text: string, quantity: number): string {
  return fromMicros(toMicros(text) * BigInt(quantity));
}
