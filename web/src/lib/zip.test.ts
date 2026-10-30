/// <reference types="node" />
import { spawnSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { crc32, zipStore } from "./zip";

const text = (s: string) => new TextEncoder().encode(s);

describe("crc32", () => {
  it("matches the standard check values", () => {
    expect(crc32(text("123456789"))).toBe(0xcbf43926);
    expect(crc32(new Uint8Array())).toBe(0);
    expect(crc32(text("a"))).toBe(0xe8b7be43);
  });
});

/** Read a stored ZIP back through its central directory (the way an unzip tool does), checking every CRC. */
function read(zip: Uint8Array): Record<string, Uint8Array> {
  const view = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  const end = zip.length - 22;
  expect(view.getUint32(end, true)).toBe(0x06054b50);
  const count = view.getUint16(end + 10, true);
  let at = view.getUint32(end + 16, true);
  const out: Record<string, Uint8Array> = {};
  for (let i = 0; i < count; i++) {
    expect(view.getUint32(at, true)).toBe(0x02014b50);
    const crc = view.getUint32(at + 16, true);
    const size = view.getUint32(at + 24, true);
    const nameLength = view.getUint16(at + 28, true);
    const offset = view.getUint32(at + 42, true);
    const name = new TextDecoder().decode(zip.subarray(at + 46, at + 46 + nameLength));
    expect(view.getUint32(offset, true)).toBe(0x04034b50);
    const start = offset + 30 + view.getUint16(offset + 26, true) + view.getUint16(offset + 28, true);
    const data = zip.subarray(start, start + size);
    expect(crc32(data)).toBe(crc);
    out[name] = data;
    at += 46 + nameLength;
  }
  return out;
}

describe("zipStore", () => {
  it("round-trips entries byte for byte, including UTF-8 names and empty files", () => {
    const entries = [{ name: "a.json", data: text('{"price":9223372036854.775807}\n') }, { name: "dir/é.csv", data: text("x,y\r\n1,2\r\n") }, { name: "empty.txt", data: new Uint8Array() }];
    const back = read(zipStore(entries));
    expect(Object.keys(back)).toEqual(["a.json", "dir/é.csv", "empty.txt"]);
    for (const e of entries) expect(Array.from(back[e.name]!)).toEqual(Array.from(e.data));
  });

  it("is deterministic: the same entries give the same bytes", () => {
    const entries = [{ name: "b", data: text("two") }, { name: "a", data: text("one") }];
    expect(Array.from(zipStore(entries))).toEqual(Array.from(zipStore(entries)));
    expect(Array.from(zipStore(entries))).not.toEqual(Array.from(zipStore([...entries].reverse()))); // order is the caller's choice
  });

  it("refuses unsafe or repeated names rather than writing a bundle that extracts elsewhere", () => {
    for (const name of ["", "/abs.txt", "../x", "a/../b", "a\\b", "a//b", "dir/"]) expect(() => zipStore([{ name, data: text("x") }]), name).toThrow(RangeError);
    expect(() => zipStore([{ name: "a", data: text("1") }, { name: "a", data: text("2") }])).toThrow(/twice/);
  });

  it("is a valid archive for an independent reader (Python's zipfile), when Python is available", () => {
    const python = spawnSync("python", ["--version"], { encoding: "utf8" });
    if (python.status !== 0) {
      console.warn("[zip.test] python not on PATH: the independent-reader check is SKIPPED.");
      return;
    }
    const dir = mkdtempSync(join(tmpdir(), "te-zip-"));
    try {
      const big = new Uint8Array(300_000).map((_, i) => (i * 31) % 251);
      const path = join(dir, "bundle.zip");
      writeFileSync(path, zipStore([{ name: "one.txt", data: text("hello\n") }, { name: "big.bin", data: big }, { name: "é/two.csv", data: text("a,b\r\n") }]));
      const script = "import sys, zipfile, hashlib\nz = zipfile.ZipFile(sys.argv[1])\nprint(z.testzip())\nfor i in z.infolist(): print(ascii(i.filename), i.file_size, i.compress_type, i.date_time, hashlib.sha256(z.read(i)).hexdigest())\n";
      const result = spawnSync("python", ["-I", "-c", script, path], { encoding: "utf8" });
      expect(result.status, result.stderr).toBe(0);
      const lines = result.stdout.trim().split(/\r?\n/);
      expect(lines[0]).toBe("None"); // testzip: no bad file
      expect(lines[1]).toContain("'one.txt' 6 0 (1980, 1, 1, 0, 0, 0)");
      expect(lines[2]).toContain("'big.bin' 300000 0");
      expect(lines[3]).toContain("'\\xe9/two.csv' 5 0"); // the name is UTF-8 in the archive; ascii() only keeps the console's code page out of it
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
});
