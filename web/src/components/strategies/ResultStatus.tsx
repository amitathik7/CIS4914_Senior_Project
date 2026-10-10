import { etRange } from "../../lib/exactTime";
import { counts } from "../../strategies/cursor";
import { summarize } from "../../strategies/draft";
import { SIGNAL_ONLY_NOTICE } from "../../strategies/copy";
import { specOf } from "../../strategies/catalog";
import { resultStatus, type LabState } from "../../strategies/store";
import type { Snapshot } from "../../strategies/snapshot";
import { labStore } from "../../strategies/useLab";
import { IconAlert, IconCheck } from "../icons";
import { Badge, Figure, Notice, Panel } from "../ui";

function StatusBadge({ state }: { state: LabState }) {
  const status = resultStatus(state);
  if (status.phase === "running") return <Badge tone="accent">Running…</Badge>;
  if (status.phase === "failed") return <Badge tone="down" icon={<IconAlert size={12} />}>Last run failed</Badge>;
  if (status.relation === "current") return <Badge tone="up" icon={<IconCheck size={12} />}>Replay complete: settings match</Badge>;
  if (status.relation === "stale") return <Badge tone="warn" icon={<IconAlert size={12} />}>Inputs changed: rerun to update</Badge>;
  return <Badge>No replay yet</Badge>;
}

/** What produced the result on screen. It always describes the SNAPSHOT's own inputs, never the draft's. */
export function ResultHeader({ state, snapshot }: { state: LabState; snapshot: Snapshot | undefined }) {
  const status = resultStatus(state);
  const spec = snapshot ? specOf(state.catalog, snapshot.request.kind) : undefined;
  const ds = snapshot?.doc.dataset;
  const upload = snapshot?.meta.dataset.kind === "upload";
  return (
    <Panel>
      <div className="lab-status">
        <div className="row" style={{ flexWrap: "wrap", gap: 10 }}>
          <StatusBadge state={state} />
          {snapshot && ds && (
            <>
              <span><b>{snapshot.strategyTitle}</b> <span className="muted">· {summarize(spec, snapshot.request.params)}</span></span>
              <span className="faint">on {snapshot.meta.dataset.name}</span>
              <Badge tone={upload ? "warn" : undefined}>{upload ? "Uploaded: provenance unknown" : "Synthetic data"}</Badge>
            </>
          )}
        </div>
        {snapshot && ds && (
          <div className="lab-status-sub faint">
            {ds.symbols.map((s) => s.symbol).join(", ")} · {ds.rows.toLocaleString("en-US")} rows · {etRange(ds.firstTime, ds.lastTime)} · SHA-256 <span className="id">{ds.sha256.slice(0, 12)}…</span> · replay {snapshot.id} · run <span className="id">{snapshot.doc.runId}</span>
          </div>
        )}
        {status.relation === "stale" && snapshot && (
          <div className="lab-stale">
            <p><b>This result belongs to the settings above, not to what you have edited since.</b> {status.reasons.length > 0 ? "Changed: " : ""}{status.reasons.join("; ")}.</p>
            <p className="faint">It is kept so you can keep looking at it. Press Run replay to produce a result for the current settings.</p>
          </div>
        )}
        {state.run.cancelled && state.run.phase === "idle" && <p className="faint">The last replay was cancelled; nothing from it was used.</p>}
      </div>
      <div className="panel-body" style={{ paddingTop: 0 }}>
        <Notice>{SIGNAL_ONLY_NOTICE}</Notice>
      </div>
    </Panel>
  );
}

/** A failed launch, in the engine's own words, with located problems for a rejected file. The last good result stays below it. */
export function RunFailure({ state }: { state: LabState }) {
  const error = state.run.error;
  if (state.run.phase !== "failed" || !error) return null;
  const more = Math.max(0, (error.details.totalProblems ?? 0) - error.problems.length);
  return (
    <Notice tone="down">
      <div className="stack" style={{ gap: 8 }}>
        <div><b>{error.details.title ?? "The replay failed"}.</b> {error.message}</div>
        {state.run.datasetName && state.run.datasetName !== "dataset.csv" && /dataset\.csv/.test(error.message) && (
          <span className="faint">The replay tool calls every file it is given "dataset.csv"; here that is your file "{state.run.datasetName}".</span>
        )}
        {error.problems.length > 0 && (
          <table className="table lab-mini" aria-label="Problems found by the replay tool">
            <thead><tr><th scope="col" className="r">Line</th><th scope="col">Column</th><th scope="col">Problem</th></tr></thead>
            <tbody>
              {error.problems.map((p, i) => (
                <tr key={i}><td className="r">{p.line ?? "—"}</td><td className="id">{p.column ?? p.where ?? "—"}</td><td className="wrap">{p.message}</td></tr>
              ))}
            </tbody>
          </table>
        )}
        {more > 0 && <span className="faint">{more} more problem{more === 1 ? "" : "s"} not listed.</span>}
        {state.snapshot && <span className="faint">The last successful result is still shown below.</span>}
        {(error.details.detail || error.details.stderr) && (
          <details className="lab-details">
            <summary>Technical details</summary>
            <pre className="lab-code">{[error.details.exitStatus != null ? `exit status ${error.details.exitStatus}` : "", error.details.detail ?? "", error.details.stderr ?? ""].filter(Boolean).join("\n")}</pre>
          </details>
        )}
        <div><button type="button" className="btn btn-sm" onClick={() => labStore.dismissRunError()}>Dismiss</button></div>
      </div>
    </Notice>
  );
}

/** "Replay so far" (the visible prefix) and "Full run" (the whole result), kept visibly apart. */
export function PrefixPanels({ snapshot, cursor, symbol }: { snapshot: Snapshot; cursor: number; symbol: string | null }) {
  const { model, doc } = snapshot;
  const c = counts(model, cursor, symbol);
  const full = counts(model, model.total, null);
  const scope = cursor === 0 ? "nothing revealed yet" : `events 1–${cursor} of ${model.total}, ${symbol ?? "all symbols"}`;
  return (
    <div className="stack">
      <Panel title="Replay so far" hint={scope}>
        <div className="lab-figs">
          <Figure label="Events" value={c.events.toLocaleString("en-US")} />
          <Figure label="Evaluated" value={c.verdicts.evaluated.toLocaleString("en-US")} />
          <Figure label="Warming up" value={c.verdicts.warming_up.toLocaleString("en-US")} />
          <Figure label="Ignored" value={c.verdicts.ignored.toLocaleString("en-US")} />
          <Figure label="Buy requests" value={c.buys.toLocaleString("en-US")} tone={c.buys > 0 ? "up" : undefined} />
          <Figure label="Sell requests" value={c.sells.toLocaleString("en-US")} tone={c.sells > 0 ? "down" : undefined} />
        </div>
        {c.verdicts.unavailable > 0 && <p className="faint lab-help">{c.verdicts.unavailable} event(s) had no diagnostics.</p>}
      </Panel>
      <Panel title="Full run" hint="whole result">
        <p className="faint lab-help">Does not change with the replay position.</p>
        <p className="muted">
          {full.events.toLocaleString("en-US")} events · {model.symbols.length} symbol{model.symbols.length === 1 ? "" : "s"} · {model.signals.length} signal request{model.signals.length === 1 ? "" : "s"} ({full.buys} buy, {full.sells} sell) · {doc.warnings.length} warning{doc.warnings.length === 1 ? "" : "s"}
        </p>
      </Panel>
    </div>
  );
}
