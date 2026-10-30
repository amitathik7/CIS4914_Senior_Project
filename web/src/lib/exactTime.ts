// Exchange-time display for the lab's RFC 3339 UTC timestamps ("2026-01-05T14:30:00.000000000Z").
//
// The tool writes nine fractional digits. A JavaScript Date holds milliseconds, so the fraction is never given to Date: only the
// whole-second part is converted to New York time, and the recorded fractional digits are put back verbatim. Nothing here
// rounds a timestamp, and the original UTC text is what the inspector and the exports show.

const ZONE = "America/New_York";
const clockFmt = new Intl.DateTimeFormat("en-US", { timeZone: ZONE, hourCycle: "h23", hour: "2-digit", minute: "2-digit", second: "2-digit" });
const dayFmt = new Intl.DateTimeFormat("en-CA", { timeZone: ZONE, year: "numeric", month: "2-digit", day: "2-digit" });
const longDayFmt = new Intl.DateTimeFormat("en-US", { timeZone: ZONE, month: "short", day: "numeric", year: "numeric" });

const PATTERN = /^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?Z$/;

export function splitTimestamp(iso: string): { whole: Date; fraction: string } | undefined {
  const m = PATTERN.exec(iso);
  if (!m) return undefined;
  const whole = new Date(`${m[1]}Z`);
  // Date rolls 2026-02-30 over to March 2: only a timestamp that reads back unchanged is a real one.
  return Number.isNaN(whole.getTime()) || !whole.toISOString().startsWith(m[1]!) ? undefined : { whole, fraction: m[2] ?? "" };
}

/** "09:30:00" or "09:30:00.25": every recorded digit up to the last non-zero one. Unparsable text is returned as is. */
export function etClock(iso: string): string {
  const t = splitTimestamp(iso);
  if (!t) return iso;
  const fraction = t.fraction.replace(/0+$/, "");
  return clockFmt.format(t.whole) + (fraction ? `.${fraction}` : "");
}

/** The ET calendar date as "2026-01-05" (to tell whether a dataset spans several days). */
export function etDay(iso: string): string {
  const t = splitTimestamp(iso);
  return t ? dayFmt.format(t.whole) : iso.slice(0, 10);
}

/** "Jan 5, 2026, 09:30:00.000000000 ET": the full recorded precision, labelled with its zone. */
export function etFull(iso: string): string {
  const t = splitTimestamp(iso);
  if (!t) return iso;
  return `${longDayFmt.format(t.whole)}, ${clockFmt.format(t.whole)}${t.fraction ? `.${t.fraction}` : ""} ET`;
}

/** "Jan 5, 2026, 09:30:00 to 09:38:00 ET" (one ET day) or both ends in full: the range of a dataset, with every recorded digit that is not a trailing zero. */
export function etRange(first: string, last: string): string {
  const a = splitTimestamp(first);
  const b = splitTimestamp(last);
  if (!a || !b) return `${first} to ${last}`;
  const day = (d: Date) => longDayFmt.format(d);
  if (day(a.whole) === day(b.whole)) return `${day(a.whole)}, ${etClock(first)} to ${etClock(last)} ET`;
  return `${day(a.whole)}, ${etClock(first)} to ${day(b.whole)}, ${etClock(last)} ET`;
}

/** Whole seconds since the epoch of a timestamp (for ordering a handful of axis labels only; identity stays the text). */
export const epochSeconds = (iso: string): number | undefined => {
  const t = splitTimestamp(iso);
  return t ? Math.floor(t.whole.getTime() / 1000) : undefined;
};
