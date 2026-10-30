import { etRange } from "../../../lib/exactTime";
import { LabError } from "../../../api/lab";
import { MEMBERS, type Member } from "../../../strategies/compare";
import { compareStatus, memberSpec, type Attempt, type CompareState } from "../../../strategies/compareState";
import { SIGNAL_ONLY_NOTICE } from "../../../strategies/copy";
import { summarize } from "../../../strategies/draft";
import { compareStore } from "../../../strategies/useCompare";
import { IconAlert, IconCheck } from "../../icons";
import { Badge, Notice, Panel } from "../../ui";

const short = (hex: string | undefined, n = 12): string => (hex ? `${hex.slice(0, n)}…` : "not reported");

function StatusBadge({ state }: { state: CompareState }) {
  const status = compareStatus(state);
  const outcome = state.attempt?.outcome;
  if (status.phase === "running") return <Badge tone="accent">Running…</Badge>;
  if (outcome && outcome !== "complete") return <Badge tone="down" icon={<IconAlert size={12} />}>Last launch did not produce a comparison</Badge>;
  if (status.relation === "current") return <Badge tone="up" icon={<IconCheck size={12} />}>Comparison complete: settings match</Badge>;
  if (status.relation === "stale") return <Badge tone="warn" icon={<IconAlert size={12} />}>Inputs changed: rerun to update</Badge>;
  return <Badge>No comparison yet</Badge>;
}

/** What produced the comparison on screen. It always describes the SNAPSHOT's own inputs, never the editors'. */
export function CompareHeader({ state }: { state: CompareState }) {
  const done = state.comparison;
  const status = compareStatus(state);
  const doc = done?.snapshots.A.doc;
  const ds = doc?.dataset;
  const upload = done?.snapshots.A.meta.dataset.kind === "upload";
  return (
    <Panel>
      <div className="lab-status">
        <div className="row" style={{ flexWrap: "wrap", gap: 10 }}>
          <StatusBadge state={state} />
          {done && <span className="faint">comparison {done.id} · inputs <span className="id">{short(done.comparisonId, 10)}</span></span>}
          {done && <Badge tone={upload ? "warn" : undefined}>{upload ? "Uploaded: provenance unknown" : "Synthetic data"}</Badge>}
          <Badge>Real engine · signal replay</Badge>
        </div>
        {done && MEMBERS.map((m) => {
          const snap = done.snapshots[m];
          const spec = memberSpec(state, m);
          return (
            <div className="cmp-ident" key={m}>
              <span className="cmp-chip" data-member={m}>{m}</span>
              <span><b>{snap.strategyTitle}</b> <span className="muted">· {summarize(spec, snap.request.params)}</span></span>
              <span className="faint">run <span className="id">{snap.doc.runId}</span> · configuration <span className="id">{short(done.configIds[m])}</span> · result <span className="id">{short(snap.doc.resultSha256)}</span></span>
            </div>
          );
        })}
        {done && ds && (
          <div className="lab-status-sub faint">
            {done.snapshots.A.meta.dataset.name} · {ds.symbols.map((s) => s.symbol).join(", ")} · {ds.rows.toLocaleString("en-US")} rows · {etRange(ds.firstTime, ds.lastTime)} · dataset SHA-256 <span className="id">{short(ds.sha256)}</span> · engine <span className="id">{short(done.runnerSha256)}</span>
          </div>
        )}
        {status.relation === "stale" && done && (
          <div className="lab-stale">
            <p><b>This comparison belongs to the settings it ran with, not to what you have edited since.</b></p>
            {status.reasons.shared.length > 0 && <p>Shared: {status.reasons.shared.join("; ")}.</p>}
            {MEMBERS.filter((m) => status.reasons[m].length > 0).map((m) => <p key={m}>{m}: {status.reasons[m].join("; ")}.</p>)}
            <p className="faint">It is kept so you can keep looking at it. Press Run comparison to produce one for the current settings.</p>
          </div>
        )}
        {state.run.cancelled && state.run.phase === "idle" && <p className="faint">The last comparison was cancelled; nothing from it was used.</p>}
      </div>
      <div className="panel-body" style={{ paddingTop: 0 }}>
        <Notice>{SIGNAL_ONLY_NOTICE}</Notice>
      </div>
    </Panel>
  );
}

function MemberFailure({ member, error }: { member: Member; error: LabError }) {
  const more = Math.max(0, (error.details.totalProblems ?? 0) - error.problems.length);
  return (
    <div className="stack" style={{ gap: 6 }}>
      <div><span className="cmp-chip" data-member={member}>{member}</span> <b>{error.details.title ?? "The replay failed"}.</b> {error.message}</div>
      {error.problems.length > 0 && (
        <div className="table-wrap"><table className="table lab-mini" aria-label={`Problems found by the replay tool for ${member}`}>
          <thead><tr><th scope="col" className="r">Line</th><th scope="col">Column</th><th scope="col">Problem</th></tr></thead>
          <tbody>{error.problems.map((p, i) => <tr key={i}><td className="r">{p.line ?? "—"}</td><td className="id">{p.column ?? p.where ?? "—"}</td><td className="wrap">{p.message}</td></tr>)}</tbody>
        </table></div>
      )}
      {more > 0 && <span className="faint">{more} more problem{more === 1 ? "" : "s"} not listed.</span>}
      {(error.details.detail || error.details.stderr) && (
        <details className="lab-details">
          <summary>Technical details</summary>
          <pre className="lab-code">{[error.details.exitStatus != null ? `exit status ${error.details.exitStatus}` : "", error.details.detail ?? "", error.details.stderr ?? ""].filter(Boolean).join("\n")}</pre>
        </details>
      )}
    </div>
  );
}

function progress(attempt: Attempt, m: Member): string {
  const a = attempt.members[m];
  return a.phase === "running" ? "replaying…" : a.phase === "done" ? `finished (${a.snapshot!.doc.signals.length} signal request${a.snapshot!.doc.signals.length === 1 ? "" : "s"})` : "failed";
}

/** The latest launch while it runs, and honestly when it did not produce a comparison: which side finished, which did not, and why. */
export function CompareAttempt({ state }: { state: CompareState }) {
  const attempt = state.attempt;
  if (!attempt || attempt.outcome === "complete") return null;
  if (attempt.outcome === "running") {
    return (
      <Notice>
        <span role="status">Comparison {attempt.launch} is running: {MEMBERS.map((m) => `${m} ${progress(attempt, m)}`).join(" · ")}. Nothing is paired until both sides are in.</span>
      </Notice>
    );
  }
  const finished = MEMBERS.filter((m) => attempt.members[m].phase === "done");
  const failed = MEMBERS.filter((m) => attempt.members[m].phase === "failed");
  const headline =
    attempt.outcome === "partial" ? `Only ${finished[0]} completed; ${failed[0]} failed. There is no comparison from this launch.`
    : attempt.outcome === "mismatch" ? "Both sides completed, but their results cannot be compared."
    : "Neither side completed. There is no comparison from this launch.";
  return (
    <Notice tone="down">
      <div className="stack" style={{ gap: 10 }}>
        <div><b>{headline}</b></div>
        {attempt.outcome === "mismatch" && attempt.mismatch && <div>{attempt.mismatch.message}</div>}
        {failed.map((m) => <MemberFailure key={m} member={m} error={attempt.members[m].error ?? new LabError("internal", "The replay failed.")} />)}
        {attempt.outcome === "partial" && (
          <p className="faint">
            {finished[0]} finished on its own ({attempt.members[finished[0]!].snapshot!.doc.signals.length} signal requests, run <span className="id">{attempt.members[finished[0]!].snapshot!.doc.runId}</span>), but a single side is not a comparison and is not shown as one. Fix {failed[0]} and press Run comparison again: both sides are replayed together so they always describe the same engine and data.
          </p>
        )}
        {attempt.outcome === "mismatch" && <p className="faint">Both results are kept out of the comparison views: A and B were not paired.</p>}
        {state.comparison && <span className="faint">The last successful comparison is still shown below.</span>}
        <div><button type="button" className="btn btn-sm" onClick={() => compareStore.dismissAttempt()}>Dismiss</button></div>
      </div>
    </Notice>
  );
}
