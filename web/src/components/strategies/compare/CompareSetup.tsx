import { useEffect, useRef, useState } from "react";
import { MEMBERS, memberLabel } from "../../../strategies/compare";
import { QUICK_STARTS, type QuickStartId } from "../../../strategies/compareStarts";
import { compareBlocker, compareDataset, memberIssues, memberSpec, sharedSymbols, type CompareState } from "../../../strategies/compareState";
import { compareStore } from "../../../strategies/useCompare";
import { buildRequest } from "../../../strategies/draft";
import { IconPlay } from "../../icons";
import { Notice, Panel } from "../../ui";
import { DatasetPicker, type DatasetHost } from "../DatasetPicker";
import { ParamField } from "../ParamField";
import { SymbolChips } from "../SymbolChips";
import { MemberEditor } from "./MemberEditor";

/** The exact settings each side will send, for the collapsed summary ("A: Moving-average crossover (short_window 2, long_window 3)"). */
function labels(state: CompareState): string[] {
  return MEMBERS.map((m) => {
    const spec = memberSpec(state, m);
    return spec ? memberLabel(m, spec.title, buildRequest(spec, state.members[m].draft).params) : `${m}: no strategy yet`;
  });
}

/** Two configurations over one shared dataset and symbol list, and the one button that runs both. */
export function CompareSetup({ state }: { state: CompareState }) {
  const [open, setOpen] = useState(true);
  const [message, setMessage] = useState<string>();
  const seen = useRef<number | undefined>(undefined);
  const blocker = compareBlocker(state);
  const running = state.run.phase === "running";
  const dataset = compareDataset(state);

  // The first time a comparison arrives the setup folds away so the result has room; it stays one click away.
  const finished = state.comparison?.id;
  useEffect(() => {
    if (finished !== undefined && seen.current !== finished) setOpen(false);
    seen.current = finished;
  }, [finished]);

  const host: DatasetHost = {
    datasets: state.lab.datasets, dataset: state.dataset, upload: state.upload, uploadError: state.uploadError, selectBuiltin: compareStore.selectBuiltin,
    selectUpload: compareStore.selectUpload, uploadFile: compareStore.uploadFile, removeUpload: compareStore.removeUpload,
  };
  const builtin = state.dataset.kind === "builtin" ? state.lab.datasets.find((d) => d.key === (state.dataset as { key: string }).key) : undefined;
  const datasetSymbols = state.dataset.kind === "upload" ? state.upload?.symbols ?? [] : builtin?.symbols ?? [];
  const symbolsSpec = memberSpec(state, "A")?.params.find((p) => p.name === "symbols");
  const symbolsIssue = memberIssues(state, "A").find((i) => i.name === "symbols")?.message;

  const quick = (id: QuickStartId) => {
    const result = compareStore.quickStart(id);
    setMessage(result.ok ? undefined : result.message);
  };

  const runButton = (
    <button type="button" className="btn btn-primary" disabled={!!blocker} onClick={() => void compareStore.runComparison()} aria-describedby="cmp-run-help">
      <IconPlay size={13} /> {running ? "Running…" : "Run comparison"}
    </button>
  );
  const help = (
    <p id="cmp-run-help" className="lab-help" data-tone={blocker && !running ? "warn" : undefined}>
      {running
        ? "Replaying A and B on the real engine, each in its own fresh run. Cancel stops waiting for them and discards their answers."
        : blocker ?? `Runs the real strategies once each over ${dataset?.name ?? "the dataset"}. Nothing is executed or traded: this produces Buy and Sell requests only, and only this button starts the engine.`}
    </p>
  );

  if (!open) {
    return (
      <Panel title="Configurations" hint="A and B over one dataset" tools={<><button type="button" className="btn btn-sm" onClick={() => setOpen(true)} aria-expanded={false}>Edit configurations</button>{runButton}</>}>
        <div className="stack" style={{ gap: 6 }}>
          <dl className="kv">
            {labels(state).map((label, i) => <div key={i} style={{ display: "contents" }}><dt><span className="cmp-chip" data-member={MEMBERS[i]}>{MEMBERS[i]}</span></dt><dd className="wrap">{label.replace(/^[AB]: /, "")}</dd></div>)}
            <dt>Dataset</dt><dd>{dataset?.name ?? "none"} <span className="faint">· symbols {sharedSymbols(state) || "none"}</span></dd>
          </dl>
          {help}
        </div>
      </Panel>
    );
  }

  return (
    <Panel title="Set up the comparison" hint="Two configurations, one shared dataset" tools={<button type="button" className="btn btn-sm" onClick={() => setOpen(false)} aria-expanded>Hide setup</button>}>
      <div className="stack" style={{ gap: 14 }}>
        <div className="cmp-setup">
          <section className="cmp-shared" aria-label="Shared by A and B">
            <h3 className="lab-section-title">Shared by A and B</h3>
            <p className="faint lab-help">Both configurations replay exactly the same bytes, symbols and recorded event order. Each one keeps its own strategy state.</p>
            <DatasetPicker host={host} />
            {symbolsSpec && (
              <ParamField
                spec={symbolsSpec} value={sharedSymbols(state)} issue={symbolsIssue} wide onChange={(t) => compareStore.setSymbols(t)}
                extra={<SymbolChips symbols={datasetSymbols} value={sharedSymbols(state)} onChange={(t) => compareStore.setSymbols(t)} />}
              />
            )}
            <div className="stack" style={{ gap: 6 }}>
              <h3 className="lab-section-title">Quick starts</h3>
              <div className="cmp-quick" role="group" aria-label="Quick starts">
                {QUICK_STARTS.map((q) => (
                  <button key={q.id} type="button" className="kind-option" onClick={() => quick(q.id)}>
                    <strong>{q.title}</strong>
                    <small>{q.summary}</small>
                  </button>
                ))}
              </div>
              <div className="row" style={{ flexWrap: "wrap" }}>
                <button type="button" className="btn btn-sm" onClick={() => compareStore.copyAToB()} title="Replace B with a copy of A. Afterwards they are independent again.">Copy A to B</button>
                <span className="faint lab-help">Quick starts and Copy only fill the editors. They run nothing.</span>
              </div>
              {message && <Notice tone="warn">{message}</Notice>}
            </div>
          </section>
          <MemberEditor state={state} member="A" />
          <MemberEditor state={state} member="B" />
        </div>
        <div className="stack" style={{ gap: 6 }}>
          <div className="row" style={{ flexWrap: "wrap" }}>
            {runButton}
            {running && <button type="button" className="btn" onClick={() => compareStore.cancelRun()}>Cancel</button>}
          </div>
          {help}
        </div>
      </div>
    </Panel>
  );
}
