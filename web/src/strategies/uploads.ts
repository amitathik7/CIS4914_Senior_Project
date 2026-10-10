// What the browser can say about a chosen CSV before any engine is involved: is it plausible text, how big, what fingerprint.
// Whether it is a VALID dataset is decided only by the replay tool's own parser, which reports line and column.

export const DEFAULT_UPLOAD_LIMIT = 8 * 1024 * 1024;

export interface UploadedFile {
  name: string;
  size: number;
  sha256: string;
  base64: string;
  /** Distinct values of the `symbol` column in first-seen order: only a suggestion for the allowlist. */
  symbols: string[];
}

export async function sha256Hex(bytes: Uint8Array): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", bytes as BufferSource);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export function toBase64(bytes: Uint8Array): string {
  let out = "";
  for (let i = 0; i < bytes.length; i += 0x8000) out += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(out);
}

/** The first problem that makes a file unusable before it is sent, as a sentence; undefined when it looks like text. */
export function precheckUpload(name: string, bytes: Uint8Array, limit = DEFAULT_UPLOAD_LIMIT): string | undefined {
  const shown = name || "upload.csv";
  if (bytes.length === 0) return `'${shown}' is empty (0 bytes). Choose a CSV with a header row and at least one data row.`;
  if (bytes.length > limit) return `'${shown}' is ${(bytes.length / 1048576).toFixed(1)} MiB; the limit is ${Math.floor(limit / 1048576)} MiB.`;
  if (bytes.subarray(0, 65536).includes(0)) return `'${shown}' contains NUL bytes, so it is not a CSV text file.`;
  return undefined;
}

export function detectSymbols(bytes: Uint8Array, maxRows = 200_000, maxSymbols = 64): string[] {
  const lines = new TextDecoder("utf-8").decode(bytes).replace(/^﻿/, "").split(/\r?\n/);
  const column = (lines[0] ?? "").split(",").map((c) => c.trim()).indexOf("symbol");
  if (column < 0) return [];
  const seen = new Set<string>();
  for (const line of lines.slice(1, maxRows + 1)) {
    const cell = line.split(",")[column];
    if (cell) seen.add(cell);
    if (seen.size >= maxSymbols) break;
  }
  return [...seen];
}

export async function describeUpload(name: string, bytes: Uint8Array): Promise<UploadedFile> {
  return { name: name || "upload.csv", size: bytes.length, sha256: await sha256Hex(bytes), base64: toBase64(bytes), symbols: detectSymbols(bytes) };
}
