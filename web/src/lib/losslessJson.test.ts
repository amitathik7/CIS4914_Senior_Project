import { describe, expect, it } from "vitest";
import {
  LNum, LosslessParseError, decimalText, derivedNumber, integerText, isLNum, parseLossless, safeInteger,
  type LValue,
} from "./losslessJson";

const obj = (v: LValue) => v as { [key: string]: LValue };

describe("parseLossless", () => {
  it("keeps every number as its source text", () => {
    const doc = obj(parseLossless('{"p":9223372036854.775807,"q":9223372036854775807,"z":-1.7320508075688774,"t":1e-7,"n":-0,"i":0}'));
    expect(["p", "q", "z", "t", "n", "i"].map((k) => (doc[k] as LNum).text)).toEqual([
      "9223372036854.775807", "9223372036854775807", "-1.7320508075688774", "1e-7", "-0", "0",
    ]);
    // The same text through JSON.parse is NOT what was written, which is the whole reason this reader exists.
    const lossy = JSON.parse('{"q":9223372036854775807,"p":9223372036854.775806}') as { q: number; p: number };
    expect(String(lossy.q)).not.toBe("9223372036854775807");
    expect(String(lossy.p)).not.toBe("9223372036854.775806");
  });

  it("reads the other JSON values", () => {
    const doc = obj(parseLossless('{"a":[1,"x",true,false,null,{}],"s":"é\\u00e8\\n\\"\\ud83d\\ude00","e":{}}'));
    expect((doc.a as LValue[]).length).toBe(6);
    expect((doc.a as LValue[]).slice(2, 5)).toEqual([true, false, null]);
    expect(doc.s).toBe('éè\n"😀');
    expect(doc.e).toEqual({});
  });

  it("accepts the whitespace JSON allows, including the tool's trailing newline", () => {
    expect(isLNum(parseLossless(" \t\r\n 7 \n"))).toBe(true);
  });

  it.each([
    ["NaN", "NaN"], ["Infinity", "Infinity"], ["leading zero", "[01]"], ["plus sign", "[+1]"], ["bare fraction", "[.5]"],
    ["trailing comma array", "[1,]"], ["trailing comma object", '{"a":1,}'], ["single quotes", "{'a':1}"],
    ["unterminated string", '"abc'], ["control character", '"a\tb"'], ["bad escape", '"\\x"'],
    ["text after the document", "{} x"], ["two documents", "{}\n{}"], ["empty input", ""], ["comment", "[1 /* c */]"],
    ["missing colon", '{"a" 1}'], ["unquoted key", "{a:1}"], ["lone minus", "[-]"],
  ])("refuses %s", (_name, text) => {
    expect(() => parseLossless(text)).toThrow(LosslessParseError);
  });

  it("refuses a duplicate object key, wherever it is", () => {
    expect(() => parseLossless('{"a":1,"a":2}')).toThrow(/duplicate object key "a"/);
    expect(() => parseLossless('{"x":{"b":1,"b":1}}')).toThrow(/duplicate/);
  });

  it("is not fooled by __proto__ or constructor keys", () => {
    const doc = obj(parseLossless('{"__proto__":{"polluted":true},"constructor":1}'));
    expect(Object.keys(doc)).toEqual(["__proto__", "constructor"]);
    expect(({} as { polluted?: boolean }).polluted).toBeUndefined();
  });

  it("refuses absurd nesting instead of overflowing the stack", () => {
    expect(() => parseLossless("[".repeat(500) + "]".repeat(500))).toThrow(/too deep/);
  });

  it("reports where it stopped", () => {
    try {
      parseLossless('{"a": [1, 2,, 3]}');
      expect.unreachable();
    } catch (error) {
      expect(error).toBeInstanceOf(LosslessParseError);
      expect((error as LosslessParseError).offset).toBe(12); // the second comma
    }
  });

  it("reads a large document quickly", () => {
    const rows = Array.from({ length: 30_000 }, (_, i) => `{"index":${i},"price":${i}.123456,"symbol":"AAPL"}`).join(",");
    const started = performance.now();
    const doc = obj(parseLossless(`{"events":[${rows}]}`));
    expect((doc.events as LValue[]).length).toBe(30_000);
    expect(performance.now() - started).toBeLessThan(2000);
  });
});

describe("typed access", () => {
  it("separates exact integers, exact decimals and derived doubles", () => {
    const q = new LNum("9223372036854775807");
    expect(integerText(q)).toBe("9223372036854775807");
    expect(safeInteger(q)).toBeUndefined(); // beyond 2^53: must never become a Number
    expect(safeInteger(new LNum("9007199254740991"))).toBe(9007199254740991);
    expect(safeInteger(new LNum("9007199254740992"))).toBeUndefined();
    expect(safeInteger(new LNum("1.0"))).toBeUndefined();
    expect(decimalText(new LNum("9223372036854.775807"))).toBe("9223372036854.775807");
    expect(decimalText(new LNum("0.000001"))).toBe("0.000001");
    expect(decimalText(new LNum("0.0000001"))).toBeUndefined(); // seventh place
    expect(decimalText(new LNum("1e3"))).toBeUndefined(); // exponent
    expect(derivedNumber(new LNum("-1.7320508075688774"))).toEqual({ value: -1.7320508075688774, text: "-1.7320508075688774" });
    expect(derivedNumber("x")).toBeUndefined();
  });
});
