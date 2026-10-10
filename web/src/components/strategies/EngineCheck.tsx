import { paramCopy } from "../../strategies/copy";
import type { CheckState } from "../../strategies/store";
import { IconCheck } from "../icons";
import { Notice } from "../ui";

function derivedLine(name: string, value: unknown): string | undefined {
  if (name === "earliest_signal_accepted_bar" && typeof value === "number") return `The first request can come on accepted bar ${value}.`;
  if (name === "max_abs_z" && typeof value === "number") return `|z| can reach at most ${value.toFixed(2)} with this lookback.`;
  return undefined;
}

/**
 * The real strategy constructors' verdict on a draft, in their own words. Cross-field rules are not duplicated in the console.
 * `check` is the verdict for the draft on screen; `issueCount` is how many problems the console's own field checks found (the engine is
 * asked only once those are fixed). `side` names the configuration in Compare ("A" or "B").
 */
export function EngineCheck({ check, issueCount, side }: { check: CheckState; issueCount: number; side?: string }) {
  const who = side ? ` (${side})` : "";
  if (issueCount > 0) {
    return <p className="faint lab-help">Fix the highlighted settings, then the engine will check the rest (for example that the long window is longer than the short one).</p>;
  }
  if (check.phase === "idle") return null;
  if (check.phase === "checking") return <p className="faint lab-help" aria-live="polite">Checking with the engine…</p>;
  if (check.phase === "rejected") {
    const fields = check.error.fields.map((f) => paramCopy(f).label);
    return (
      <Notice tone="down">
        <b>The engine rejected this configuration{who}.</b> {check.error.message}
        {fields.length > 0 && <span className="faint"> Check: {fields.join(", ")}.</span>}
      </Notice>
    );
  }
  if (check.phase === "unavailable") {
    return <Notice tone="warn"><b>Not checked by the engine{who}.</b> {check.error.message}</Notice>;
  }
  const facts = Object.entries(check.result.derived).map(([k, v]) => derivedLine(k, v)).filter((x): x is string => !!x);
  return (
    <div className="stack" style={{ gap: 6 }} aria-live="polite">
      <p className="lab-ok">
        <IconCheck size={13} />
        <span>Accepted by the engine's strategy code.{facts.length > 0 && <span className="faint"> {facts.join(" ")}</span>}</span>
      </p>
      {check.result.warnings.map((w) => <Notice key={w.code} tone="warn"><b>Engine warning.</b> {w.message}</Notice>)}
    </div>
  );
}
