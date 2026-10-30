import { useMemo } from "react";
import { MEMBERS, type Member } from "../../../strategies/compare";
import type { ComparisonSnapshot } from "../../../strategies/compareState";
import {
  AGREEMENT_DEFINITION, agreementPercent, COUNTS_ARE_NOT_PERFORMANCE, readiness, readinessText, summarize, summarizeFull, type CompareSummary as Summary,
} from "../../../strategies/compareSummary";
import { Panel } from "../../ui";

const n = (v: number): string => v.toLocaleString("en-US");

function Counts({ summary, caption }: { summary: Summary; caption: string }) {
  const { members, agreement: a } = summary;
  const rows: [string, (m: Member) => number][] = [
    ["Rows covered", (m) => members[m].events],
    ["Evaluated", (m) => members[m].verdicts.evaluated],
    ["Warming up", (m) => members[m].verdicts.warming_up],
    ["Ignored", (m) => members[m].verdicts.ignored],
    ...(MEMBERS.some((m) => members[m].verdicts.unavailable > 0) ? [["No diagnostics", (m: Member) => members[m].verdicts.unavailable] as [string, (m: Member) => number]] : []),
    ["Buy requests", (m) => members[m].buys],
    ["Sell requests", (m) => members[m].sells],
  ];
  const percent = agreementPercent(a.agree, a.comparable);
  return (
    <div className="stack" style={{ gap: 8 }}>
      <div className="table-wrap"><table className="table lab-mini cmp-counts" aria-label={caption}>
        <thead><tr><th scope="col">{caption}</th><th scope="col" className="r"><span className="cmp-chip" data-member="A">A</span></th><th scope="col" className="r"><span className="cmp-chip" data-member="B">B</span></th></tr></thead>
        <tbody>
          {rows.map(([label, pick]) => <tr key={label}><th scope="row">{label}</th><td className="r num">{n(pick("A"))}</td><td className="r num">{n(pick("B"))}</td></tr>)}
        </tbody>
      </table></div>
      <p className="cmp-agree" data-empty={a.comparable === 0 ? "true" : undefined}>
        {a.comparable === 0
          ? <>Agreement: <b>not available</b>. No row has been evaluated by both configurations yet, so there is nothing to compare.</>
          : <>Of <b>{n(a.comparable)}</b> comparable row{a.comparable === 1 ? "" : "s"} (both evaluated), <b>{n(a.agree)}</b> agree and <b>{n(a.differ)}</b> differ{percent ? <> · agreement <b>{percent}</b> of comparable rows</> : null}.</>}
        {" "}<span className="faint">{n(a.notComparable)} of {n(summary.events)} row{summary.events === 1 ? "" : "s"} left out (at least one side did not evaluate it){a.excluded.length ? `: ${a.excluded.map((e) => `${e.label} ${n(e.count)}`).join("; ")}` : ""}.</span>
      </p>
    </div>
  );
}

/** Signal-level summaries. "Replay so far" follows the shared position and the display symbol; "Full run" never changes. */
export function CompareSummary({ done, cursor, symbol }: { done: ComparisonSnapshot; cursor: number; symbol: string | null }) {
  const { model } = done;
  const prefix = useMemo(() => summarize(model, cursor, symbol), [model, cursor, symbol]);
  const full = useMemo(() => summarizeFull(model), [model]);
  const ready = useMemo(() => ({ A: readiness(model, "A", cursor, symbol), B: readiness(model, "B", cursor, symbol) }), [model, cursor, symbol]);
  const scope = cursor === 0 ? "nothing revealed yet" : `events 1–${cursor} of ${model.total}, ${symbol ?? "all symbols"}`;
  return (
    <div className="stack">
      <Panel title="Replay so far" hint={scope}>
        <div className="stack" style={{ gap: 10 }}>
          <Counts summary={prefix} caption="Visible prefix" />
          <details className="lab-details">
            <summary>What "agree" means</summary>
            <p className="faint lab-help" style={{ marginTop: 6 }}>{AGREEMENT_DEFINITION} {COUNTS_ARE_NOT_PERFORMANCE}</p>
          </details>
        </div>
      </Panel>
      <Panel title="Warm-up by symbol" hint={scope}>
        <div className="table-wrap"><table className="table lab-mini" aria-label="Warm-up progress by symbol">
          <thead><tr><th scope="col">Symbol</th><th scope="col"><span className="cmp-chip" data-member="A">A</span></th><th scope="col"><span className="cmp-chip" data-member="B">B</span></th></tr></thead>
          <tbody>
            {ready.A.map((r, i) => <tr key={r.symbol}><th scope="row">{r.symbol}</th><td className="wrap">{readinessText(r)}</td><td className="wrap">{readinessText(ready.B[i]!)}</td></tr>)}
          </tbody>
        </table></div>
        <p className="faint lab-help" style={{ marginTop: 8 }}>Read from each side's recorded window, as of the last revealed row of that symbol. Not inferred from requests: a ready strategy may request nothing.</p>
      </Panel>
      <Panel title="Full run" hint={`all ${n(model.total)} events, all symbols · does not change with the position`}>
        <Counts summary={full} caption="Whole result" />
      </Panel>
    </div>
  );
}
