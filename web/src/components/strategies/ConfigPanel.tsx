import { useNavigate } from "react-router";
import { destinationText, handoffBlocker, toCarried, type HandoffState } from "../../strategies/backtestHandoff";
import { activeCheck, datasetIdentity, draftIssues, runBlocker, specFor, type LabState } from "../../strategies/store";
import { labStore } from "../../strategies/useLab";
import { examplesFor } from "../../strategies/presets";
import { useEngine } from "../../state/engine";
import { IconPlay } from "../icons";
import { Panel } from "../ui";
import { DatasetPicker, type DatasetHost } from "./DatasetPicker";
import { EngineCheck } from "./EngineCheck";
import { ParamFields } from "./ParamFields";
import { PresetMenu, type PresetHost } from "./PresetMenu";
import { StrategyKinds } from "./StrategyKinds";
import { SymbolChips } from "./SymbolChips";

/** The editable draft: strategy, dataset, parameters, the engine's check, and the two actions. Used by Configure and Explore. */
export function ConfigPanel({ state, onRun }: { state: LabState; onRun: () => void }) {
  const navigate = useNavigate();
  const { info } = useEngine();
  const spec = specFor(state);
  const issues = draftIssues(state);
  const check = activeCheck(state);
  const flagged = check.phase === "rejected" ? check.error.fields : [];
  const blocker = runBlocker(state);
  const running = state.run.phase === "running";

  const rejected = check.phase === "rejected" ? check.error.message : undefined;
  const dataset = datasetIdentity(state);
  const carry = spec ? handoffBlocker(state.draft, spec, issues.length, rejected, info?.market_data.symbols) : state.service.phase === "online" ? "Choose a strategy first." : "The strategies come from the engine, which is not connected yet.";
  const toBacktest = () => {
    const carried = toCarried(state.draft);
    if (!carried || carry) return;
    const handoff: HandoffState = { fromStrategies: carried, label: `${spec?.title ?? carried.kind} from Strategies`, basis: "the settings in the editor now", dataset: dataset?.name };
    navigate("/backtests/new", { state: handoff });
  };
  const builtin = state.dataset.kind === "builtin" ? state.datasets.find((d) => d.key === (state.dataset as { key: string }).key) : undefined;
  const example = spec ? examplesFor(builtin, [spec])[0] : undefined;
  const datasetSymbols = state.dataset.kind === "upload" ? state.upload?.symbols ?? [] : builtin?.symbols ?? [];

  const symbolChips = (value: string) => <SymbolChips symbols={datasetSymbols} value={value} onChange={(text) => labStore.setValue("symbols", text)} />;

  const datasetHost: DatasetHost = {
    datasets: state.datasets, dataset: state.dataset, upload: state.upload, uploadError: state.uploadError, selectBuiltin: labStore.selectBuiltin,
    selectUpload: labStore.selectUpload, uploadFile: labStore.uploadFile, removeUpload: labStore.removeUpload,
  };

  const kinds = state.catalog?.strategies ?? [];
  const presets: PresetHost = {
    presets: state.presets, catalog: state.catalog, examples: examplesFor(builtin, kinds),
    save: labStore.savePreset, load: labStore.loadPreset, remove: labStore.deletePreset, useExample: (draft, label) => void labStore.loadDraft(draft, label),
  };

  return (
    <Panel title="Configuration" tools={<PresetMenu host={presets} />}>
      <div className="stack" style={{ gap: 0 }}>
        <section className="lab-section">
          <h3 className="lab-section-title">Strategy</h3>
          <StrategyKinds kinds={kinds} value={state.draft.kind} onSelect={labStore.selectKind} label="Strategy" />
        </section>

        <section className="lab-section">
          <h3 className="lab-section-title">Data</h3>
          <DatasetPicker host={datasetHost} />
          {example && (
            <button type="button" className="btn btn-ghost btn-sm" style={{ justifySelf: "start" }} onClick={() => labStore.loadDraft(example.draft, `Example: ${example.name}`)}>
              Use the example parameters for this dataset
            </button>
          )}
        </section>

        {spec && (
          <section className="lab-section">
            <h3 className="lab-section-title">Parameters</h3>
            {state.origin && <p className="faint lab-help">Loaded from: {state.origin.label}{state.origin.edited ? " (edited since)" : ""}</p>}
            <ParamFields spec={spec} values={state.draft.values} issues={issues} flagged={flagged} onChange={labStore.setValue} symbolChips={symbolChips} />
            <EngineCheck check={check} issueCount={issues.length} />
          </section>
        )}

        <section className="lab-section">
          <h3 className="lab-section-title">Actions</h3>
          <div className="stack" style={{ gap: 6 }}>
            <div className="row" style={{ flexWrap: "wrap" }}>
              <button type="button" className="btn btn-primary" disabled={!!blocker} onClick={onRun} aria-describedby="lab-run-help">
                <IconPlay size={13} /> {running ? "Running…" : "Run replay"}
              </button>
              {running && <button type="button" className="btn" onClick={() => labStore.cancelRun()}>Cancel</button>}
              <button type="button" className="btn btn-ghost" disabled={!spec} onClick={() => labStore.restoreDefaults()} title="Reset every parameter to the engine's default. Your symbols stay.">Restore defaults</button>
            </div>
            <p id="lab-run-help" className="lab-help" data-tone={blocker && !running ? "warn" : undefined}>
              {running
                ? "Replaying on the real engine. Cancel stops waiting for it and discards its answer."
                : blocker ?? `Runs the real strategy over ${dataset?.name ?? "the dataset"}. Nothing is executed or traded: this produces Buy and Sell requests only.`}
            </p>
          </div>
          <div className="stack" style={{ gap: 6, marginTop: 6 }}>
            <button type="button" className="btn" disabled={!!carry} onClick={toBacktest} aria-describedby="lab-backtest-help">Use in new backtest</button>
            <p id="lab-backtest-help" className="lab-help" data-tone={carry ? "warn" : undefined}>
              {carry ?? `Pre-fills the New backtest form with the settings in this editor now (not necessarily those of the replay on screen). ${destinationText(info?.data_source, dataset?.name)} Nothing starts until you press Run backtest there.`}
            </p>
          </div>
        </section>
      </div>
    </Panel>
  );
}
