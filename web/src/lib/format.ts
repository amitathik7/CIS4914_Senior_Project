// Times are stored in UTC and shown in exchange time (New York), labelled ET.

const ZONE = "America/New_York";

const timeFmt = new Intl.DateTimeFormat("en-US", { timeZone: ZONE, hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false });
const minuteFmt = new Intl.DateTimeFormat("en-US", { timeZone: ZONE, hour: "2-digit", minute: "2-digit", hour12: false });
const dateFmt = new Intl.DateTimeFormat("en-US", { timeZone: ZONE, month: "short", day: "numeric" });
const dayFmt = new Intl.DateTimeFormat("en-US", { timeZone: ZONE, weekday: "short", month: "short", day: "numeric" });
const fullFmt = new Intl.DateTimeFormat("en-US", { timeZone: ZONE, month: "short", day: "numeric", year: "numeric", hour: "2-digit", minute: "2-digit", hour12: false });
const intFmt = new Intl.NumberFormat("en-US");
const compactFmt = new Intl.NumberFormat("en-US", { notation: "compact", maximumFractionDigits: 1 });

export const time = (iso: string) => timeFmt.format(new Date(iso));
export const minute = (iso: string) => minuteFmt.format(new Date(iso));
export const date = (iso: string) => dateFmt.format(new Date(iso));
export const day = (iso: string) => dayFmt.format(new Date(iso));
export const dateTime = (iso: string) => fullFmt.format(new Date(iso));

// Sub-millisecond part of an RFC 3339 timestamp, for ordering steps inside one bar.
export function micros(iso: string): string {
  const m = /\.(\d{3})(\d{3})/.exec(iso);
  return m ? `${time(iso)}.${m[1]}${m[2]}` : time(iso);
}

export function period(start: string, end?: string): string {
  if (!end) return `since ${dateTime(start)} ET`;
  const a = date(start);
  const b = date(end);
  return a === b ? a : `${a} – ${b}`;
}

export const int = (n: number) => intFmt.format(n);
export const compact = (n: number) => compactFmt.format(n);

export function signedInt(n: number): string {
  return n > 0 ? `+${intFmt.format(n)}` : n < 0 ? `−${intFmt.format(-n)}` : "0";
}

export function percent(fraction: number, dp = 2, signed = true): string {
  const v = fraction * 100;
  const s = Math.abs(v).toFixed(dp);
  const sign = v > 0 && signed && Number(s) !== 0 ? "+" : v < 0 && Number(s) !== 0 ? "−" : "";
  return `${sign}${s}%`;
}

export function ratioText(n: number | undefined, dp = 2): string {
  if (n === undefined || !Number.isFinite(n)) return "—";
  return n < 0 ? `−${Math.abs(n).toFixed(dp)}` : n.toFixed(dp);
}

export function latency(us: number): string {
  if (us >= 1000) return `${(us / 1000).toFixed(us >= 10_000 ? 0 : 1)} ms`;
  return `${us >= 100 ? us.toFixed(0) : us.toFixed(1)} µs`;
}

export const label = (code: string) => code.replace(/_/g, " ").replace(/^./, (c) => c.toUpperCase());

export function relative(iso: string, now = Date.now()): string {
  const s = Math.round((now - Date.parse(iso)) / 1000);
  if (s < 60) return "just now";
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
  return `${Math.floor(s / 86400)} d ago`;
}
