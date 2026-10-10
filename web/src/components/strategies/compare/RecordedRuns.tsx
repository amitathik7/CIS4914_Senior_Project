import { useEffect, useMemo, useState } from "react";
import type { EquityPoint, RunDetail } from "../../../api/contract";
import { drawdownLine, equityLine } from "../../../pages/chartData";
import { period } from "../../../lib/format";
import { compareRecorded, metricRows, netPnl, VERDICT_TEXT } from "../../../strategies/recordedCompare";
import { useEngine } from "../../../state/engine";
import { TimeChart } from "../../charts/TimeChart";
import { Badge, Empty, Notice } from "../../ui";

interface RunRecord {
  detail?: RunDetail;
  equity?: EquityPoint[];
  error?: string;
}

/** One finished run's record and equity curve. A completed run no longer changes, so it is read once. */
function useRecord(runId: string): RunRecord {
  const { client } = useEngine();
  const [state, setState] = useState<{ id: string; record: RunRecord }>({ id: "", record: {} });
  useEffect(() => {
    if (!runId) return;
    let cancelled = false;
    Promise.all([client.getRun(runId), client.equity(runId)]).then(
      ([detail, equity]) => !cancelled && setState({ id: runId, record: { detail, equity } }),
      (error: Error) => !cancelled && setState({ id: runId, record: { error: error.message } }),
    );
    return () => { cancelled = true; };
  }, [client, runId]);
  return state.id === runId ? state.record : {};
}

const moneyAxis = (v: number) => `$${v.toLocaleString("en-US", { maximumFractionDigits: 0 })}`;
const pctAxis = (v: number) => `${v.toFixed(2)}%`;

/** Two EXISTING Console backtests side by side. They are inspected, never ranked: no winner is named and none is chosen. */
export function RecordedRuns() {
  const { runs, info } = useEngine();
  const finished = useMemo(() => (runs ?? []).filter((r) => r.mode === "backtest" && r.status === "completed"), [runs]);
  const [first, setFirst] = useState("");
  const [second, setSecond] = useState("");
  const one = useRecord(first);
  const two = useRecord(second);
  const ready = one.detail && two.detail && first !== second;
  const comparability = ready ? compareRecorded(one.detail!, two.detail!, info?.data_source) : undefined;
  const rows = ready ? metricRows(one.detail!, two.detail!, [netPnl(one.detail!), netPnl(two.detail!)]) : [];

  const select = (label: string, value: string, set: (v: string) => void) => (
    <label className="field">
      <span>{label}</span>
      <select className="select" value={value} onChange={(e) => set(e.target.value)}>
        <option value="">Choose a completed backtest…</option>
        {finished.map((r) => <option key={r.run_id} value={r.run_id}>{r.name} · {period(r.period_start, r.period_end)}</option>)}
      </select>
    </label>
  );

  if (finished.length < 2) {
    return <Empty title="Fewer than two completed backtests">Run each configuration in its own backtest (see above). When both have finished you can inspect them side by side here.</Empty>;
  }
  return (
    <div className="stack" style={{ gap: 12 }}>
      <div className="cmp-pick">
        {select("Run 1", first, setFirst)}
        {select("Run 2", second, setSecond)}
      </div>
      {first && first === second && <Notice tone="warn">Choose two different runs.</Notice>}
      {(one.error || two.error) && <Notice tone="down">Could not load a run: {one.error ?? two.error}</Notice>}
      {comparability && (
        <>
          <div className="row" style={{ flexWrap: "wrap" }}>
            <Badge tone={comparability.verdict === "not_comparable" ? "warn" : undefined}>{VERDICT_TEXT[comparability.verdict].badge}</Badge>
            <span className="muted">{VERDICT_TEXT[comparability.verdict].explain}</span>
          </div>
          {comparability.differences.length > 0 && <Notice tone="warn"><b>Different:</b> {comparability.differences.join(" · ")}</Notice>}
          <div className="table-wrap"><table className="table lab-mini" aria-label="Settings of the two runs">
            <thead><tr><th scope="col">Setting</th><th scope="col">Run 1</th><th scope="col">Run 2</th><th scope="col">Match</th></tr></thead>
            <tbody>
              {comparability.checks.map((c) => (
                <tr key={c.key} data-diff={c.ok ? undefined : "true"}>
                  <th scope="row">{c.label.replace(/^Same /, "")}</th><td className="wrap id">{c.one}</td><td className="wrap id">{c.two}</td>
                  <td><span className="cmp-status" data-status={c.ok ? "same" : "differs"}>{c.ok ? "= same" : "≠ differs"}</span></td>
                </tr>
              ))}
            </tbody>
          </table></div>
          <div className="table-wrap"><table className="table lab-mini" aria-label="Figures from the two runs' reports">
            <thead><tr><th scope="col">Figure</th><th scope="col" className="r">Run 1</th><th scope="col" className="r">Run 2</th></tr></thead>
            <tbody>
              {rows.map((r) => (
                <tr key={r.key}>
                  <th scope="row">{r.label}</th>
                  <td className="r num">{r.one}{r.noteOne && <div className="faint cmp-note">{r.noteOne}</div>}</td>
                  <td className="r num">{r.two}{r.noteTwo && <div className="faint cmp-note">{r.noteTwo}</div>}</td>
                </tr>
              ))}
            </tbody>
          </table></div>
          <p className="faint lab-help">
            Figures use the Report page's own definitions. Net P&L is ending equity minus starting capital, in exact decimal arithmetic. A dash means the figure does not exist for that run, with the reason under it; it is never zero. These are the runs' portfolio-wide figures: where a run traded several strategies, nothing is attributed to one of them.
          </p>
          <ul className="lab-list">{comparability.cautions.map((c) => <li key={c} className="muted">{c}</li>)}</ul>
          {one.equity && two.equity && one.equity.length > 1 && two.equity.length > 1 && (
            <div className="grid grid-2">
              <div>
                <h4 className="cmp-curve-title">Equity <span className="faint">· <span className="cmp-chip" data-member="A">1</span> solid, <span className="cmp-chip" data-member="B">2</span> dashed</span></h4>
                <TimeChart label="Equity of the two runs" height={220} format={moneyAxis} series={[
                  { id: "one", kind: "line", tone: "accent", data: equityLine(one.equity) }, { id: "two", kind: "line", tone: "second", dashed: true, data: equityLine(two.equity) },
                ]} />
              </div>
              <div>
                <h4 className="cmp-curve-title">Drawdown <span className="faint">· below the running peak</span></h4>
                <TimeChart label="Drawdown of the two runs" height={220} format={pctAxis} series={[
                  { id: "one", kind: "line", tone: "accent", data: drawdownLine(one.equity) }, { id: "two", kind: "line", tone: "second", dashed: true, data: drawdownLine(two.equity) },
                ]} />
              </div>
            </div>
          )}
        </>
      )}
      {!ready && first && second && first !== second && !one.error && !two.error && <p className="faint" aria-busy="true">Loading the two runs…</p>}
    </div>
  );
}
