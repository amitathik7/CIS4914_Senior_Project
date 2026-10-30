import type { LabState } from "../../strategies/store";
import { labStore } from "../../strategies/useLab";
import { IconAlert, IconCheck } from "../icons";
import { Badge, Notice } from "../ui";

export function ServiceBadge({ state }: { state: LabState }) {
  const { service } = state;
  if (service.phase === "online" && service.status?.runner.state === "ready") {
    const r = service.status.runner;
    return (
      <span title={`Replays run on the real C++ strategy engine: ${r.name}, ${r.tool} ${r.project_version}, SHA-256 ${r.sha256.slice(0, 12)}…, schema ${r.schema_version}.`}>
        <Badge tone="up" icon={<IconCheck size={12} />}>Real engine</Badge>
      </span>
    );
  }
  if (service.phase === "unknown" || service.phase === "checking") return <Badge>Connecting…</Badge>;
  return <Badge tone="warn" icon={<IconAlert size={12} />}>Engine unavailable</Badge>;
}

/** Shown only while the service cannot be used: what is wrong, and the exact commands that fix it. */
export function ServiceNotice({ state }: { state: LabState }) {
  const { service } = state;
  if (service.phase === "online" || service.phase === "unknown" || service.phase === "checking") return null;
  const retry = <button type="button" className="btn btn-sm" onClick={() => void labStore.connect()}>Retry</button>;

  if (service.phase === "no_runner" && service.status && service.status.runner.state !== "ready") {
    const r = service.status.runner;
    return (
      <Notice tone="warn">
        <div className="stack" style={{ gap: 8 }}>
          <div><b>The service is running, but the replay engine is not available.</b> {r.message}</div>
          {r.setup && <><span className="faint">Build it once from the repository root, then retry:</span><pre className="lab-code">{r.setup}</pre></>}
          <div>{retry}</div>
        </div>
      </Notice>
    );
  }
  if (service.phase === "error") {
    return <Notice tone="down"><div className="stack" style={{ gap: 8 }}><div><b>The service answered, but the console could not use its answer.</b> {service.error?.message}</div><div>{retry}</div></div></Notice>;
  }
  return (
    <Notice tone="warn">
      <div className="stack" style={{ gap: 8 }}>
        <div>
          <b>The Strategy Lab service is not running.</b> Strategies runs the real C++ strategy code through a small local service, because a browser cannot start a native program. Until it is running nothing can be replayed or checked; the rest of the console is unaffected.
        </div>
        <span className="faint">Start it in a terminal (it needs only Python and the built engine), then retry:</span>
        <pre className="lab-code">python python/strategy_lab/lab_gateway.py</pre>
        <div>{retry}</div>
      </div>
    </Notice>
  );
}
