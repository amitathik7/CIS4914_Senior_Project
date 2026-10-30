import { etFull } from "../../lib/exactTime";
import type { Entry } from "../../strategies/replay";
import type { Snapshot } from "../../strategies/snapshot";
import { useLab } from "../../strategies/useLab";
import { IconDownload } from "../icons";
import { Badge, Notice } from "../ui";

const KV = ({ rows }: { rows: readonly Entry[] }) => (
  <dl className="kv">
    {rows.map((r) => <div key={r.name} style={{ display: "contents" }}><dt>{r.name}</dt><dd className="id">{r.text}</dd></div>)}
  </dl>
);

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section className="lab-section">
      <h3 className="lab-section-title">{title}</h3>
      {children}
    </section>
  );
}

/** The run behind the result: dataset, the configuration that was run, runner warnings, identities, and the whole-run counters. */
export function RunDetails({ snapshot, onJson, onMetadata }: { snapshot: Snapshot; onJson: () => void; onMetadata: () => void }) {
  const { launches } = useLab();
  const { doc, meta, request } = snapshot;
  const ds = doc.dataset;
  const strategy = doc.strategies[0]!;
  const upload = meta.dataset.kind === "upload";
  return (
    <div className="stack" style={{ gap: 4 }}>
      <Section title="Dataset">
        <dl className="kv">
          <dt>File</dt><dd>{meta.dataset.name} <Badge tone={upload ? "warn" : undefined}>{upload ? "Uploaded: provenance unknown" : "Synthetic fixture"}</Badge></dd>
          <dt>Provenance</dt><dd className="muted wrap">{meta.dataset.provenance}</dd>
          <dt>SHA-256</dt><dd className="id">{ds.sha256}</dd>
          <dt>Size</dt><dd>{ds.bytes.toLocaleString("en-US")} bytes · {ds.rows.toLocaleString("en-US")} rows · format {ds.format}</dd>
          <dt>Range</dt><dd>{etFull(ds.firstTime)} to {etFull(ds.lastTime)}</dd>
        </dl>
        <table className="table lab-mini" aria-label="Symbols in the dataset">
          <thead><tr><th scope="col">Symbol</th><th scope="col" className="r">Bars</th><th scope="col" className="r">Trade rows</th><th scope="col">First (ET)</th><th scope="col">Last (ET)</th></tr></thead>
          <tbody>
            {ds.symbols.map((s) => (
              <tr key={s.symbol}><td><b>{s.symbol}</b></td><td className="r">{s.barRows}</td><td className="r">{s.tradeRows}</td><td className="id">{etFull(s.firstTime)}</td><td className="id">{etFull(s.lastTime)}</td></tr>
            ))}
          </tbody>
        </table>
      </Section>

      <Section title="Configuration that was run">
        <p className="muted" style={{ fontSize: "var(--fs-sm)" }}>{snapshot.strategyTitle} (<span className="id">{strategy.kind}</span>). Every value is the engine's own record, with defaults filled in. Editing the draft never changes this.</p>
        <KV rows={strategy.parameters} />
        {strategy.derived.length > 0 && <><p className="faint" style={{ fontSize: "var(--fs-sm)" }}>Facts the engine derived from it:</p><KV rows={strategy.derived} /></>}
        <p className="faint" style={{ fontSize: "var(--fs-sm)" }}>Sent by the console: {request.params.map(([n, t]) => `${n}=${t}`).join(" · ")}</p>
      </Section>

      <Section title="Runner warnings">
        {doc.warnings.length === 0 ? <p className="muted">None.</p> : (
          <ul className="lab-list">{doc.warnings.map((w, i) => <li key={i}><span className="id">{w.code}</span> {w.message}</li>)}</ul>
        )}
        {doc.publicationFailures.length > 0 && <Notice tone="warn">The engine's bus refused {doc.publicationFailures.length} signal(s); they are listed in the Run JSON export.</Notice>}
      </Section>

      <Section title="Run identity">
        <dl className="kv">
          <dt>Run id</dt><dd className="id">{doc.runId}</dd>
          <dt>Result SHA-256</dt><dd className="id">{doc.resultSha256 ?? "not reported"}</dd>
          <dt>Mode</dt><dd>Signal replay (no risk, execution or portfolio stage)</dd>
          <dt>Engine</dt><dd className="id">{meta.runner.name} · {doc.provenance.find((p) => p.name === "project_version")?.text} · {doc.provenance.find((p) => p.name === "compiler")?.text} · {doc.provenance.find((p) => p.name === "build")?.text}</dd>
          <dt>Engine file</dt><dd className="id">SHA-256 {meta.runner.sha256} · {meta.runner.size.toLocaleString("en-US")} bytes</dd>
          <dt>Schema</dt><dd className="id">strategy_lab.replay {doc.schemaVersion}</dd>
          <dt>This launch</dt><dd>Launch {snapshot.id} of this tab · {meta.process.duration_ms} ms · finished {etFull(snapshot.finishedAt)}</dd>
          <dt>Engine launches</dt><dd>{launches} in this tab. Only <b>Run replay</b> starts the engine; stepping, filtering, tabs and downloads never do.</dd>
        </dl>
        <p className="muted wrap" style={{ fontSize: "var(--fs-sm)" }}>{doc.notice}</p>
        <details className="lab-details">
          <summary>Technical details</summary>
          <p className="faint" style={{ margin: "8px 0 4px" }}>Arguments file given to the tool (one per line):</p>
          <pre className="lab-code">{meta.process.args_file}</pre>
          <p className="faint" style={{ margin: "8px 0 4px" }}>Context:</p>
          <KV rows={doc.context} />
          {meta.process.stderr && <><p className="faint" style={{ margin: "8px 0 4px" }}>Tool stderr{meta.process.stderr_truncated ? " (truncated)" : ""}:</p><pre className="lab-code">{meta.process.stderr}</pre></>}
          <p className="faint" style={{ margin: "8px 0 4px" }}>Provenance:</p>
          <KV rows={doc.provenance} />
        </details>
      </Section>

      <Section title="Whole run">
        <p className="faint" style={{ fontSize: "var(--fs-sm)" }}>Counts for the full result. They do not change with the replay position.</p>
        <KV rows={doc.summary} />
        <details className="lab-details"><summary>Engine and bus counters</summary><KV rows={doc.engine} /></details>
      </Section>

      <Section title="Exports">
        <div className="lab-exports" style={{ border: 0, padding: 0 }}>
          <button type="button" className="btn btn-sm" onClick={onJson}><IconDownload size={13} /> Run JSON <span className="faint">· full run, exactly as the engine wrote it</span></button>
          <button type="button" className="btn btn-sm" onClick={onMetadata}><IconDownload size={13} /> Configuration and metadata <span className="faint">· full run, small JSON</span></button>
        </div>
      </Section>
    </div>
  );
}
