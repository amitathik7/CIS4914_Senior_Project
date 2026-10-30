import { etFull } from "../../../lib/exactTime";
import { MEMBERS } from "../../../strategies/compare";
import type { ComparisonSnapshot } from "../../../strategies/compareState";
import type { Entry } from "../../../strategies/replay";
import { Badge, Notice } from "../../ui";

const KV = ({ rows }: { rows: readonly Entry[] }) => (
  <dl className="kv">{rows.map((r) => <div key={r.name} style={{ display: "contents" }}><dt>{r.name}</dt><dd className="id">{r.text}</dd></div>)}</dl>
);

/** The two runs behind a comparison, side by side: identities first, then the configuration each one ran with. */
export function CompareDetails({ done }: { done: ComparisonSnapshot }) {
  const a = done.snapshots.A;
  const ds = a.doc.dataset;
  const upload = a.meta.dataset.kind === "upload";
  return (
    <div className="stack" style={{ gap: 14 }}>
      <section className="lab-section">
        <h3 className="lab-section-title">Shared by both runs</h3>
        <dl className="kv">
          <dt>Dataset</dt><dd>{a.meta.dataset.name} <Badge tone={upload ? "warn" : undefined}>{upload ? "Uploaded: provenance unknown" : "Synthetic fixture"}</Badge></dd>
          <dt>Provenance</dt><dd className="muted wrap">{a.meta.dataset.provenance}</dd>
          <dt>SHA-256</dt><dd className="id">{ds.sha256}</dd>
          <dt>Size</dt><dd>{ds.bytes.toLocaleString("en-US")} bytes · {ds.rows.toLocaleString("en-US")} rows · format {ds.format}</dd>
          <dt>Range</dt><dd>{etFull(ds.firstTime)} to {etFull(ds.lastTime)}</dd>
          <dt>Engine file</dt><dd className="id">{a.meta.runner.name} · SHA-256 {a.meta.runner.sha256} · {a.meta.runner.size.toLocaleString("en-US")} bytes</dd>
          <dt>Comparison</dt><dd className="id">number {done.id} of this tab · inputs {done.comparisonId} · finished {etFull(done.finishedAt)}</dd>
        </dl>
        <p className="muted wrap" style={{ fontSize: "var(--fs-sm)" }}>{a.doc.notice}</p>
      </section>
      <div className="cmp-columns">
        {MEMBERS.map((m) => {
          const snap = done.snapshots[m];
          const strategy = snap.doc.strategies[0]!;
          return (
            <section className="lab-section" key={m} aria-label={`Run ${m}`}>
              <h3 className="lab-section-title"><span className="cmp-chip" data-member={m}>{m}</span> {snap.strategyTitle} <span className="faint">(<span className="id">{strategy.kind}</span>)</span></h3>
              <dl className="kv">
                <dt>Run id</dt><dd className="id">{snap.doc.runId}</dd>
                <dt>Result SHA-256</dt><dd className="id">{snap.doc.resultSha256 ?? "not reported"}</dd>
                <dt>Configuration</dt><dd className="id">{done.configIds[m]}</dd>
                <dt>Strategy id</dt><dd className="id">{strategy.strategyId}</dd>
                <dt>Launch</dt><dd>{snap.meta.process.duration_ms} ms · finished {etFull(snap.finishedAt)}</dd>
                <dt>Requests</dt><dd>{snap.doc.signals.length} in the full run</dd>
              </dl>
              <p className="faint" style={{ fontSize: "var(--fs-sm)" }}>Parameters as the engine ran them (defaults filled in):</p>
              <KV rows={strategy.parameters} />
              {strategy.derived.length > 0 && <><p className="faint" style={{ fontSize: "var(--fs-sm)" }}>Facts the engine derived from it:</p><KV rows={strategy.derived} /></>}
              <p className="faint" style={{ fontSize: "var(--fs-sm)" }}>Sent by the console: {snap.request.params.map(([n, t]) => `${n}=${t}`).join(" · ")}</p>
              {snap.doc.warnings.length === 0 ? <p className="muted">No runner warnings.</p> : <ul className="lab-list">{snap.doc.warnings.map((w, i) => <li key={i}><span className="id">{w.code}</span> {w.message}</li>)}</ul>}
              {snap.doc.publicationFailures.length > 0 && <Notice tone="warn">The engine's bus refused {snap.doc.publicationFailures.length} signal(s); they are listed in this run's JSON in the bundle.</Notice>}
            </section>
          );
        })}
      </div>
    </div>
  );
}
