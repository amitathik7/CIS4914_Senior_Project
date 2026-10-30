import { REQUEST_CAVEATS, STRATEGY_COPY } from "../../strategies/copy";
import { specFor, type LabState } from "../../strategies/store";
import { Empty, Notice, Panel } from "../ui";

/** What the selected strategy does and, as importantly, what its requests are not. Wording is the console's; the reason texts are the engine's. */
export function AboutStrategy({ state }: { state: LabState }) {
  const spec = specFor(state);
  if (!spec) {
    return (
      <Panel title="About the strategy">
        <Empty title="No strategy loaded">Once the Strategy Lab service is running, the engine's strategies and their rules appear here.</Empty>
      </Panel>
    );
  }
  const copy = STRATEGY_COPY[spec.kind];
  return (
    <Panel title={spec.title} hint={spec.kind}>
      <div className="stack" style={{ gap: 14 }}>
        <p>{copy?.short ?? spec.summary}</p>
        {copy ? (
          <div>
            <h3 className="lab-section-title">How it decides</h3>
            <ol className="lab-list lab-rules">{copy.rules.map((rule) => <li key={rule}>{rule}</li>)}</ol>
          </div>
        ) : (
          <p className="muted">{spec.summary}</p>
        )}
        <Notice>
          <div className="stack" style={{ gap: 6 }}>{REQUEST_CAVEATS.map((c) => <span key={c}>{c}</span>)}</div>
        </Notice>
        <details className="lab-details">
          <summary>What the engine can record for each row ({spec.reasons.length} reasons)</summary>
          <table className="table lab-mini" aria-label="Decision reasons" style={{ marginTop: 8 }}>
            <thead><tr><th scope="col">Reason code</th><th scope="col">Verdict</th><th scope="col">Meaning (the engine's own text)</th></tr></thead>
            <tbody>
              {spec.reasons.map((r) => <tr key={r.code}><td><span className="id">{r.code}</span></td><td>{r.verdict.replace(/_/g, " ")}</td><td className="wrap muted">{r.text}</td></tr>)}
            </tbody>
          </table>
        </details>
        <p className="faint" style={{ fontSize: "var(--fs-sm)" }}>
          Defaults shown in the form are the strategy's own placeholders, not tuned or recommended trading parameters. Full rules: <span className="id">{spec.docs}</span>.
        </p>
      </div>
    </Panel>
  );
}
