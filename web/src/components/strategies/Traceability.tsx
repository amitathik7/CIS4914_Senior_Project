import { MEMBERS } from "../../strategies/compare";
import { useCompare, useValidation } from "../../strategies/useCompare";
import { Badge, Panel } from "../ui";

const short = (hex: string | undefined, n = 16): string => (hex ? `${hex.slice(0, n)}…` : "not reported");

/** What produced what: the engine in use, the comparison on screen and the last validation, with the identities that tie them together. */
export function Traceability() {
  const compare = useCompare();
  const validation = useValidation();
  const status = compare.lab.service.status;
  const runner = status?.runner;
  const done = compare.comparison;
  const report = validation.result?.report;
  return (
    <Panel title="Traceability" hint="Which engine, data and settings produced what you are looking at">
      <dl className="kv">
        <dt>Service</dt><dd>{status ? <>gateway {status.gateway.version} · API {status.gateway.api}</> : <span className="faint">not connected</span>}</dd>
        <dt>Replay engine</dt>
        <dd className="id wrap">
          {runner?.state === "ready"
            ? <>{runner.name} · {runner.tool} {runner.project_version} · SHA-256 {runner.sha256} · {runner.size.toLocaleString("en-US")} bytes · result schema {runner.schema_version}</>
            : <span className="faint">{runner ? "not available: " + runner.message : "unknown"}</span>}
        </dd>
        <dt>Comparison on screen</dt>
        <dd className="wrap">
          {done ? (
            <>
              <span className="id">number {done.id} · inputs {short(done.comparisonId)}</span> · dataset <span className="id">{done.snapshots.A.meta.dataset.name} {short(done.datasetSha256)}</span>
              {MEMBERS.map((m) => <div key={m}><span className="cmp-chip" data-member={m}>{m}</span> <span className="id">run {done.snapshots[m].doc.runId} · configuration {short(done.configIds[m])} · result {short(done.snapshots[m].doc.resultSha256)}</span></div>)}
              <div>{runner?.state === "ready" && runner.sha256 !== done.runnerSha256 ? <Badge tone="warn">Engine rebuilt since</Badge> : <Badge>Engine unchanged since</Badge>}</div>
            </>
          ) : <span className="faint">none yet</span>}
        </dd>
        <dt>Last validation</dt>
        <dd className="wrap">
          {report ? (
            <>
              <span className="id">number {report.number} · {report.startedAt} · executable {short(report.executable.sha256)} · files {short(report.scenarioFiles.identity)}</span>
              <div>{runner?.state === "ready" && runner.sha256 === report.executable.sha256 ? <Badge tone="up">Same executable as now</Badge> : <Badge tone="warn">Not the executable in use now</Badge>}</div>
            </>
          ) : <span className="faint">none yet</span>}
        </dd>
        <dt>Engine launches</dt><dd>{compare.lab.launches} replays from Explore · {compare.launches} comparisons (two replays each) · {validation.launches} validations, in this tab. Only their buttons start the engine.</dd>
      </dl>
    </Panel>
  );
}
