import { useEffect } from "react";
import { etFull } from "../../lib/exactTime";
import { dateTime } from "../../lib/format";
import { download } from "../../strategies/exports";
import { STATUS_TEXT, type ScenarioOutcome, type ScenarioStatus } from "../../strategies/validation";
import { validationBlocker, validationStatus } from "../../strategies/validationStore";
import { useValidation, validationStore } from "../../strategies/useCompare";
import { IconAlert, IconCheck, IconDownload, IconPlay } from "../icons";
import { Badge, Empty, Notice, Panel } from "../ui";

const TONE: Record<ScenarioStatus, "up" | "down" | "warn" | undefined> = { passed: "up", failed: "down", error: "warn", not_run: undefined };
const short = (hex: string, n = 12): string => `${hex.slice(0, n)}…`;
/** The service writes local time with its UTC offset; show the same instant in exchange time (the offset text is kept beside it). */
const atET = (stamp: string): string => (Number.isNaN(Date.parse(stamp)) ? stamp : dateTime(stamp));

function Scenario({ s }: { s: ScenarioOutcome }) {
  return (
    <details className="cmp-scn" open={s.status === "failed" || s.status === "error" || s.demonstration} data-status={s.status}>
      <summary>
        <Badge tone={TONE[s.status]} icon={s.status === "passed" ? <IconCheck size={12} /> : s.status === "not_run" ? undefined : <IconAlert size={12} />}>{STATUS_TEXT[s.status]}</Badge>
        <b>{s.title}</b> <span className="faint id">{s.id}</span>
        <span className="muted"> · {s.summary}</span>
        {s.demonstration && <Badge tone="accent">Demonstration: expected to fail</Badge>}
      </summary>
      <div className="stack" style={{ gap: 8, padding: "8px 0 4px 4px" }}>
        <p className="muted wrap" style={{ fontSize: "var(--fs-sm)" }}>{s.purpose}</p>
        {s.error && (
          <Notice tone="warn"><b>{s.error.title}.</b> {s.error.message}{s.error.exitCode ? <> <span className="faint">(exit status {s.error.exitCode})</span></> : null}
            <span className="faint"> This check could not be carried out, so it is neither a pass nor a failure of the strategies.</span></Notice>
        )}
        {s.mismatches.length > 0 && (
          <div className="table-wrap"><table className="table lab-mini" aria-label={`Expected against actual for ${s.id}`}>
            <thead><tr><th scope="col">Where</th><th scope="col">Expected (by hand)</th><th scope="col">Actual (the executable)</th><th scope="col">Note</th></tr></thead>
            <tbody>{s.mismatches.map((m, i) => <tr key={i}><th scope="row" className="wrap">{m.where}</th><td className="wrap id">{m.expected}</td><td className="wrap id">{m.actual}</td><td className="wrap muted">{m.note}</td></tr>)}</tbody>
          </table></div>
        )}
        <dl className="kv">
          <dt>Checks made</dt><dd>{s.checks}</dd>
          <dt>Dataset</dt><dd className="id">{s.datasetPath} · SHA-256 given to the tool {short(s.datasetSha256GivenToTool, 16)} · expected (line endings normalized) {short(s.datasetSha256Expected, 16)}</dd>
          <dt>Configuration</dt><dd className="id wrap">{s.configuration.map((c) => `${c.kind}: ${c.parameters.map(([k, v]) => `${k}=${v}`).join(", ")}`).join(" | ")}</dd>
          <dt>Run</dt><dd className="id">{s.runId ?? "no replay result"}{s.resultSha256 ? ` · result ${short(s.resultSha256, 16)}` : ""} · {s.durationS} s · finished {s.finishedAt}</dd>
          <dt>Expectation</dt><dd className="wrap">{s.derivedBy}. Sources: {s.sources.join("; ")}</dd>
        </dl>
      </div>
    </details>
  );
}

/** The Lab's scenario checks of the real strategy executable. They run only when the button is pressed, and a result keeps naming the build and files it ran. */
export function ValidationPanel() {
  const state = useValidation();
  const phase = state.lab.service.phase;
  useEffect(() => {
    if (phase === "online" && state.infoPhase === "idle") void validationStore.loadInfo();
  }, [phase, state.infoPhase]);

  const blocker = validationBlocker(state);
  const running = state.run.phase === "running";
  const result = state.result;
  const report = result?.report;
  const status = validationStatus(state);
  const info = state.info;
  const normal = report?.scenarios.filter((s) => !s.demonstration) ?? [];
  const demo = report?.scenarios.filter((s) => s.demonstration) ?? [];

  return (
    <Panel title="Scenario validation" hint="Checks of the real strategy executable against expectations derived by hand">
      <div className="stack" style={{ gap: 14 }}>
        <Notice>
          <b>These are scenario checks, not the repository's test suite.</b> A few checked-in demo scenarios are run through the real strategy_lab_replay executable and compared with expected values worked out by hand from the strategy documents. They are <b>not</b> the C++ CTest/GoogleTest suite, and passing them says nothing about whether that suite passes.
        </Notice>

        {state.infoError && <Notice tone="down"><b>The scenario files could not be read.</b> {state.infoError.message} <button type="button" className="btn btn-sm" onClick={() => void validationStore.loadInfo()}>Try again</button></Notice>}

        <section className="lab-section">
          <h3 className="lab-section-title">Run</h3>
          <div className="row" style={{ flexWrap: "wrap" }}>
            <button type="button" className="btn btn-primary" disabled={!!blocker} onClick={() => void validationStore.run()} aria-describedby="val-help">
              <IconPlay size={13} /> {running ? "Running…" : "Run validation"}
            </button>
            {running && <button type="button" className="btn" onClick={() => validationStore.cancel()}>Cancel</button>}
            <label className="row" title="An in-memory copy of one scenario with two deliberately wrong expectations. It must fail; it shows what a mismatch looks like.">
              <input type="checkbox" checked={state.includeDemonstration} disabled={running || !info?.demonstrationAvailable} onChange={(e) => validationStore.setIncludeDemonstration(e.target.checked)} />
              <span>Include the mismatch demonstration (expected to fail)</span>
            </label>
          </div>
          <p id="val-help" className="lab-help" data-tone={blocker && !running ? "warn" : undefined}>
            {running ? "Running every scenario through the real executable. Cancel stops waiting and discards the answer." : blocker ?? "Nothing runs until you press this. It starts one short process per scenario; the repository's CTest suite is not touched."}
          </p>
          {state.run.cancelled && state.run.phase === "idle" && <p className="faint">The last validation was cancelled; nothing from it was used.</p>}
          {state.run.phase === "failed" && state.run.error && (
            <Notice tone="down">
              <b>{state.run.error.details.title ?? "The validation could not be carried out"}.</b> {state.run.error.message}
              {result && <span className="faint"> The previous result is still shown below.</span>}{" "}
              <button type="button" className="btn btn-sm" onClick={() => validationStore.dismissError()}>Dismiss</button>
            </Notice>
          )}
        </section>

        {result && report ? (
          <section className="lab-section" aria-label="Validation result">
            <h3 className="lab-section-title">Result of validation {report.number}</h3>
            <div className="row" style={{ flexWrap: "wrap" }}>
              {report.summary.error > 0 ? <Badge tone="warn" icon={<IconAlert size={12} />}>Some checks could not be carried out</Badge>
                : report.summary.failed > 0 ? <Badge tone="down" icon={<IconAlert size={12} />}>Some results differ from the expectations</Badge>
                : <Badge tone="up" icon={<IconCheck size={12} />}>All {report.summary.scenarios} scenarios passed</Badge>}
              <span className="muted">{report.summary.passed} passed · {report.summary.failed} failed · {report.summary.error} error{report.summary.notRun ? ` · ${report.summary.notRun} not run` : ""} of {report.summary.scenarios}</span>
              {status.relation === "current" && <Badge tone="up">Describes the engine and files in use now</Badge>}
              {status.relation === "stale" && <Badge tone="warn" icon={<IconAlert size={12} />}>Does not describe the current build or files</Badge>}
              {status.relation === "unknown" && <Badge>Cannot yet tell whether this is current</Badge>}
            </div>
            {status.reasons.length > 0 && <Notice tone="warn">{status.reasons.join(" ")} It is kept as a record of that run only; press Run validation to check what is in use now.</Notice>}
            <dl className="kv">
              <dt>Run at</dt><dd>{atET(report.startedAt)} to {atET(report.finishedAt)} ET <span className="faint id">(written by the service as {report.startedAt} to {report.finishedAt})</span></dd>
              <dt>Executable</dt><dd className="id">{report.executable.fileName} · SHA-256 {report.executable.sha256} · {report.executable.sizeBytes.toLocaleString("en-US")} bytes{report.executable.tool ? ` · ${report.executable.tool} ${report.executable.version ?? ""}` : ""}{report.executable.compiler ? ` · ${report.executable.compiler} / ${report.executable.build ?? ""}` : ""}</dd>
              <dt>Scenario files</dt><dd className="id wrap">{report.scenarioFiles.directory} · {report.scenarioFiles.files.length} files · identity {report.scenarioFiles.identity}</dd>
              <dt>Received</dt><dd>{etFull(result.receivedAt)} · launch {result.launch} of this tab</dd>
            </dl>
            <p className="faint lab-help">{report.scopeNote}</p>
            <div className="stack" style={{ gap: 6 }}>{normal.map((s) => <Scenario key={s.id} s={s} />)}</div>
            {report.demonstration.included && demo.length > 0 && (
              <div className="stack" style={{ gap: 6 }}>
                <h4 className="cmp-curve-title">Demonstration <span className="faint">· deliberately wrong expectations; not counted above</span></h4>
                {demo.map((s) => <Scenario key={s.id} s={s} />)}
                {report.demonstration.results.map((r) => <p key={r.id} className="faint lab-help">{r.failedAsDesigned ? "It failed, as designed: the comparison can see a wrong expectation." : "It did NOT fail as designed, so the comparison itself cannot be trusted."}</p>)}
              </div>
            )}
            <div className="row">
              <button type="button" className="btn btn-sm" onClick={() => download(`strategy-lab-validation_${report.number}_${report.executable.sha256.slice(0, 8)}.json`, result.raw, "application/json;charset=utf-8")}>
                <IconDownload size={13} /> Validation report (JSON) <span className="faint">· exactly as the service sent it</span>
              </button>
            </div>
          </section>
        ) : (
          !running && <Empty title="No validation has been run in this tab">Press Run validation to check the executable against the scenarios. Earlier results are never shown as a check of a newer build.</Empty>
        )}
        <details className="lab-details">
          <summary>{info ? `What would run: ${info.scenarios.length} scenario file${info.scenarios.length === 1 ? "" : "s"}` : "What would run"}</summary>
          <div className="stack" style={{ gap: 6, marginTop: 8 }}>
            {info ? (
              <>
                <p className="muted" style={{ fontSize: "var(--fs-sm)" }}>
                  In <span className="id">{info.directory}</span> · identity SHA-256 <span className="id" title={info.identity}>{short(info.identity, 16)}</span>
                  {" "}<button type="button" className="btn btn-ghost btn-sm" onClick={() => void validationStore.loadInfo()}>Check the files again</button>
                </p>
                <ul className="lab-list">
                  {info.scenarios.map((s) => <li key={s.id}><b>{s.title}</b> <span className="faint id">{s.id}</span> <span className="muted">· {s.strategies.map((c) => c.kind).join(" + ")} on <span className="id">{s.datasetPath}</span>{s.outcome === "rejected" ? " · expects a refusal" : ""}</span></li>)}
                </ul>
              </>
            ) : state.infoPhase === "loading" ? <p className="faint" aria-busy="true">Reading the scenario files…</p> : <p className="faint">The list appears once the service is running.</p>}
          </div>
        </details>
      </div>
    </Panel>
  );
}
