// A minimal ZIP writer for the comparison bundle: STORED entries only (no compression), UTF-8 names, a fixed timestamp.
//
// Deterministic on purpose: the same entries in the same order give the same bytes in every browser, so a bundle exported twice
// from the same comparison is byte-identical. (A compressor's output can differ between implementations; STORE cannot.) The price
// is size: the run files are written as they are, uncompressed.
//
// No ZIP64: a bundle is refused (never silently truncated) if it would need it.

export interface ZipEntry {
  name: string;
  data: Uint8Array;
}

const TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

/** CRC-32 (IEEE 802.3), the checksum ZIP stores for each file. */
export function crc32(data: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) c = TABLE[(c ^ data[i]!) & 0xff]! ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const DOS_TIME = 0; // 00:00:00
const DOS_DATE = (0 << 9) | (1 << 5) | 1; // 1980-01-01, the earliest date ZIP can say
const UTF8_NAMES = 0x0800;
const LIMIT = 0xffffffff;

function checkName(name: string, seen: Set<string>): void {
  if (name === "" || name.startsWith("/") || name.includes("\\") || name.split("/").some((part) => part === ".." || part === "")) {
    throw new RangeError(`"${name}" is not a safe file name for a ZIP entry.`);
  }
  if (seen.has(name)) throw new RangeError(`"${name}" appears twice in the ZIP.`);
  seen.add(name);
}

export function zipStore(entries: readonly ZipEntry[]): Uint8Array {
  const encoder = new TextEncoder();
  const seen = new Set<string>();
  const names = entries.map((e) => {
    checkName(e.name, seen);
    return encoder.encode(e.name);
  });
  if (entries.length > 0xffff) throw new RangeError("Too many files for a ZIP without ZIP64.");
  const sizes = entries.map((e) => e.data.length);
  const total = entries.reduce((sum, _e, i) => sum + 30 + names[i]!.length + sizes[i]! + 46 + names[i]!.length, 22);
  if (total > LIMIT) throw new RangeError("The bundle is larger than a ZIP without ZIP64 can hold (4 GiB).");

  const out = new Uint8Array(total);
  const view = new DataView(out.buffer);
  let at = 0;
  const u16 = (v: number) => { view.setUint16(at, v, true); at += 2; };
  const u32 = (v: number) => { view.setUint32(at, v, true); at += 4; };
  const bytes = (b: Uint8Array) => { out.set(b, at); at += b.length; };

  const crcs = entries.map((e) => crc32(e.data));
  const offsets: number[] = [];
  entries.forEach((e, i) => {
    offsets.push(at);
    u32(0x04034b50); u16(20); u16(UTF8_NAMES); u16(0); u16(DOS_TIME); u16(DOS_DATE);
    u32(crcs[i]!); u32(sizes[i]!); u32(sizes[i]!); u16(names[i]!.length); u16(0);
    bytes(names[i]!); bytes(e.data);
  });
  const directory = at;
  entries.forEach((_e, i) => {
    u32(0x02014b50); u16(20); u16(20); u16(UTF8_NAMES); u16(0); u16(DOS_TIME); u16(DOS_DATE);
    u32(crcs[i]!); u32(sizes[i]!); u32(sizes[i]!); u16(names[i]!.length); u16(0); u16(0); u16(0); u16(0); u32(0); u32(offsets[i]!);
    bytes(names[i]!);
  });
  const directorySize = at - directory;
  u32(0x06054b50); u16(0); u16(0); u16(entries.length); u16(entries.length); u32(directorySize); u32(directory); u16(0);
  return out;
}
