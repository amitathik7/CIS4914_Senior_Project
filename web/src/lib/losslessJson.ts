// A strict JSON reader that never turns a number into a double.
//
// The Strategy Lab writes prices as exact decimal text inside JSON numbers (150.02, 9223372036854.775807) and quantities as
// int64 integers (9223372036854775807). JSON.parse reads every number as an IEEE double and silently rounds those, so the
// replay documents are read here instead: each number becomes an LNum that keeps its source text, and the caller decides, per
// field, whether it is an exact decimal, an exact integer or a derived double (see strategies/replay.ts).
//
// Strictness matches the Python reader the Streamlit lab uses: RFC 8259 grammar only (no NaN, Infinity, comments, trailing
// commas or leading zeros), no duplicate object keys, nothing after the document but whitespace.

export class LNum {
  constructor(readonly text: string) {}
  toString(): string {
    return this.text;
  }
}

export type LValue = string | boolean | null | LNum | LValue[] | { [key: string]: LValue };

export class LosslessParseError extends Error {
  constructor(message: string, readonly offset: number) {
    super(`${message} (at character ${offset})`);
  }
}

const NUMBER = /-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/y;
const MAX_DEPTH = 64;

export function parseLossless(text: string): LValue {
  let at = 0;

  const fail = (message: string): never => {
    throw new LosslessParseError(message, at);
  };

  const skip = () => {
    while (at < text.length) {
      const c = text.charCodeAt(at);
      if (c === 0x20 || c === 0x0a || c === 0x0d || c === 0x09) at++;
      else break;
    }
  };

  const string = (): string => {
    const start = at;
    at++; // opening quote
    let escaped = false;
    for (;;) {
      if (at >= text.length) return fail("unterminated string");
      const c = text.charCodeAt(at);
      if (c === 0x22) break;
      if (c < 0x20) return fail("control character inside a string");
      if (c === 0x5c) {
        escaped = true;
        at += 2;
      } else at++;
    }
    at++; // closing quote
    const literal = text.slice(start, at);
    if (!escaped) return literal.slice(1, -1);
    try {
      return JSON.parse(literal) as string; // validates and decodes the escapes (including surrogate pairs)
    } catch {
      at = start;
      return fail("invalid escape sequence in a string");
    }
  };

  const value = (depth: number): LValue => {
    if (depth > MAX_DEPTH) return fail("nesting is too deep");
    skip();
    const c = text[at];
    if (c === "{") {
      at++;
      const out: { [key: string]: LValue } = Object.create(null) as { [key: string]: LValue };
      skip();
      if (text[at] === "}") {
        at++;
        return out;
      }
      for (;;) {
        skip();
        if (text[at] !== '"') return fail("expected an object key");
        const keyAt = at;
        const key = string();
        if (key in out) {
          at = keyAt;
          return fail(`duplicate object key "${key}"`);
        }
        skip();
        if (text[at] !== ":") return fail("expected ':'");
        at++;
        out[key] = value(depth + 1);
        skip();
        if (text[at] === ",") {
          at++;
          continue;
        }
        if (text[at] === "}") {
          at++;
          return out;
        }
        return fail("expected ',' or '}'");
      }
    }
    if (c === "[") {
      at++;
      const out: LValue[] = [];
      skip();
      if (text[at] === "]") {
        at++;
        return out;
      }
      for (;;) {
        out.push(value(depth + 1));
        skip();
        if (text[at] === ",") {
          at++;
          continue;
        }
        if (text[at] === "]") {
          at++;
          return out;
        }
        return fail("expected ',' or ']'");
      }
    }
    if (c === '"') return string();
    if (text.startsWith("true", at)) {
      at += 4;
      return true;
    }
    if (text.startsWith("false", at)) {
      at += 5;
      return false;
    }
    if (text.startsWith("null", at)) {
      at += 4;
      return null;
    }
    NUMBER.lastIndex = at;
    const match = NUMBER.exec(text);
    if (match) {
      at += match[0].length;
      return new LNum(match[0]);
    }
    return fail(c === undefined ? "unexpected end of input" : `unexpected character ${JSON.stringify(c)}`);
  };

  const root = value(0);
  skip();
  if (at < text.length) fail("text after the end of the JSON document");
  return root;
}

// ---- typed access used by the parsers ----------------------------------------------------------------------------

const INTEGER = /^-?(?:0|[1-9]\d*)$/;
const DECIMAL = /^-?(?:0|[1-9]\d*)(?:\.\d{1,6})?$/;

export const isLNum = (v: unknown): v is LNum => v instanceof LNum;
export const isObject = (v: unknown): v is { [key: string]: LValue } =>
  typeof v === "object" && v !== null && !Array.isArray(v) && !(v instanceof LNum);

/** Exact integer text ("9223372036854775807"), or undefined when the number is not written as an integer. */
export const integerText = (v: LValue): string | undefined => (isLNum(v) && INTEGER.test(v.text) ? v.text : undefined);

/** Exact plain decimal text with at most six places (a price), or undefined (an exponent or a seventh place is refused). */
export const decimalText = (v: LValue): string | undefined => (isLNum(v) && DECIMAL.test(v.text) ? v.text : undefined);

/** A safe JavaScript integer, or undefined. Only for counts and indexes that are bounded by the dataset size. */
export function safeInteger(v: LValue): number | undefined {
  const text = integerText(v);
  if (text === undefined) return undefined;
  const n = Number(text);
  return Number.isSafeInteger(n) ? n : undefined;
}

/** A derived statistic (an average, a z-score): a double, keeping the text the tool wrote. */
export function derivedNumber(v: LValue): { value: number; text: string } | undefined {
  if (!isLNum(v)) return undefined;
  const value = Number(v.text);
  return Number.isFinite(value) ? { value, text: v.text } : undefined;
}
