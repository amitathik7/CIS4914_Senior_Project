import { CARRIES_TEXT, destinationText, NOT_CARRIED_TEXT } from "../../../strategies/backtestHandoff";
import { MEMBERS, type Member } from "../../../strategies/compare";
import { compareDataset, compareStatus, type CompareState } from "../../../strategies/compareState";
import { useEngine } from "../../../state/engine";
import { Badge, Notice, Panel } from "../../ui";
import { RecordedRuns } from "./RecordedRuns";
import { useHandoff } from "./useHandoff";

/** "Use A / B in new backtest": which of the side's settings the button carries is stated beside it, and both are offered once they differ. */
function Handoff({ state, member }: { state: CompareState; member: Member }) {
  const { edited, compared } = useHandoff(state, member);
  const changes = compared ? compareStatus(state).reasons[member] : [];
  const differs = changes.length > 0;
  const sentence = !compared
    ? `Carries ${member}'s settings as currently edited. No comparison has run yet.`
    : differs
      ? `${member} was edited after the comparison ran (${changes.join("; ")}). “As edited” carries the settings in the editor now; “As compared” carries the ones the comparison on screen actually ran.`
      : `Carries ${member}'s settings, which are the ones the comparison on screen ran.`;
  const refusals = differs
    ? [edited.blocked && `“As edited” is unavailable: ${edited.blocked}`, compared?.blocked && `“As compared” is unavailable: ${compared.blocked}`].filter(Boolean)
    : edited.blocked ? [edited.blocked] : [];
  const note = differs || refusals.length === 0 ? [sentence, ...refusals].join(" ") : refusals.join(" ");
  return (
    <div className="stack" style={{ gap: 4 }}>
      <div className="row" style={{ flexWrap: "wrap", gap: 8 }}>
        <button type="button" className="btn btn-sm" disabled={!!edited.blocked} onClick={edited.go} aria-describedby={`cmp-fin-${member} cmp-fin-where`}>
          <span className="cmp-chip" data-member={member}>{member}</span> Use {member}{differs ? " as edited" : ""} in new backtest
        </button>
        {differs && compared && (
          <button type="button" className="btn btn-sm" disabled={!!compared.blocked} onClick={compared.go} aria-describedby={`cmp-fin-${member} cmp-fin-where`}>
            <span className="cmp-chip" data-member={member}>{member}</span> Use {member} as compared in new backtest
          </button>
        )}
      </div>
      <p id={`cmp-fin-${member}`} className="lab-help" data-tone={refusals.length ? "warn" : undefined}>{note}</p>
    </div>
  );
}

/**
 * Returns, drawdown and trades cannot come from a signal replay: it has no risk, execution or portfolio stage. This panel says so, hands each
 * side to the Console's Backtests, and lets two EXISTING backtests be inspected side by side, flagged when they are not comparable.
 */
export function FinancialPanel({ state }: { state: CompareState }) {
  const { info } = useEngine();
  const done = state.comparison;
  const source = info?.data_source;
  const datasetName = done?.snapshots.A.request.dataset.name ?? compareDataset(state)?.name;
  return (
    <Panel title="Financial comparison" hint="Needs the full backtest pipeline" tools={<Badge tone="warn">Not available from a signal replay</Badge>}>
      <div className="stack" style={{ gap: 16 }}>
        <Notice>
          <b>No returns, P&L, drawdown, fees or trade counts are shown for A and B, because none exist yet.</b> A signal replay stops at the Buy and Sell requests: it has no risk approvals, orders, fills, positions or portfolio, so any number computed from the requests would be invented. Request counts are not profitability, and agreement between A and B is not a measure of either.
        </Notice>

        <section className="lab-section">
          <h3 className="lab-section-title">What the engine can do today</h3>
          <ul className="lab-list lab-rules">
            <li>The C++ engine's backtest wiring (the replay driver, the risk manager and the run loop) is not implemented yet, and no engine API is served from this repository. {source === "demo" ? <>The console's Backtests therefore run on its <b>in-browser demo simulator</b> over its own market data (<Badge tone="warn">Demo data</Badge>), not over the Lab dataset used here.</> : source === "engine" ? <>This console is connected to an engine API; whatever market data it replays is its own, and the console cannot verify it against the Lab dataset used here.</> : <>The console's Backtests use whatever engine it is connected to (its in-browser demo simulator when none is configured); that engine has not answered yet.</>}</li>
            <li>So the same market-data snapshot cannot be shared between this signal comparison and a backtest. Treat a backtest of A and of B as a separate experiment, not as the money result of the replay above.</li>
            <li>The replay does not model fees, slippage, latency, position limits or starting capital. Those exist only in a backtest.</li>
          </ul>
        </section>

        <section className="lab-section">
          <h3 className="lab-section-title">To compare them financially</h3>
          <ol className="lab-list">
            <li>Run <b>A</b> and <b>B</b> as <b>two separate backtests</b>, one strategy each, so neither competes with the other for capital or shares a portfolio. Use the same evaluation period, starting capital, fee and slippage model, risk limits and execution settings in both.</li>
            <li>Allow for warm-up: a strategy decides nothing until its window is full{done ? <> (here A needs {done.snapshots.A.doc.strategies[0]!.windowSize} accepted bars and B needs {done.snapshots.B.doc.strategies[0]!.windowSize} per symbol)</> : null}. Bars before that earn nothing, so state the scored period and any pre-roll when you compare. Each run may only use information available at each event.</li>
            <li>Then choose both runs below. The console checks that their settings match and says so when they do not.</li>
          </ol>
          <p id="cmp-fin-where" className="lab-help"><b>{destinationText(source, datasetName)}</b> {CARRIES_TEXT} {NOT_CARRIED_TEXT}</p>
          <div className="cmp-handoff">{MEMBERS.map((m) => <Handoff key={m} state={state} member={m} />)}</div>
        </section>

        <section className="lab-section">
          <h3 className="lab-section-title">Recorded backtests, side by side</h3>
          <p className="faint lab-help">Any two completed backtests can be inspected here, even ones started elsewhere. If their settings differ they are flagged as not comparable. Nothing is ranked and no winner is chosen.</p>
          <RecordedRuns />
        </section>
      </div>
    </Panel>
  );
}
