import { describe, expect, it } from "vitest";
import { precheckUpload } from "./uploads";

const columns = [
  { name: "symbol", required: true },
  { name: "exchange_time", required: true },
  { name: "type", required: true },
  { name: "price", required: true },
  { name: "volume", required: false },
];
const bytes = (text: string) => new TextEncoder().encode(text);

describe("precheckUpload", () => {
  it("accepts a header the replay tool reads, with or without optional columns and a BOM", () => {
    expect(precheckUpload("a.csv", bytes("symbol,exchange_time,type,price\nAAPL,2026-01-05T14:30:00Z,bar,3\n"), undefined, columns)).toBeUndefined();
    expect(precheckUpload("a.csv", bytes("﻿symbol,exchange_time,type,price,volume\r\nAAPL,2026-01-05T14:30:00Z,bar,3,10\r\n"), undefined, columns)).toBeUndefined();
  });

  it("refuses a file that is plainly not a bars dataset, naming what is missing", () => {
    expect(precheckUpload("bad.csv", bytes("garbage,not,a,csv\n1,2\n"), undefined, columns)).toMatch(/no symbol, exchange_time, type, price columns/);
  });

  it("refuses an unknown column and a header with no data rows", () => {
    expect(precheckUpload("x.csv", bytes("symbol,exchange_time,type,price,note\nAAPL,2026-01-05T14:30:00Z,bar,3,hi\n"), undefined, columns)).toMatch(/'note'/);
    expect(precheckUpload("x.csv", bytes("symbol,exchange_time,type,price\n\n"), undefined, columns)).toMatch(/no data rows/);
  });

  it("checks only size and text without the catalog's columns", () => {
    expect(precheckUpload("bad.csv", bytes("garbage\n"))).toBeUndefined();
    expect(precheckUpload("e.csv", bytes(""))).toMatch(/empty/);
  });
});
