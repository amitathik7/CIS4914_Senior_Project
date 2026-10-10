import { destinationText } from "../../../strategies/backtestHandoff";
import type { Member } from "../../../strategies/compare";
import { compareDataset, memberCheck, memberIssues, memberSpec, type CompareState } from "../../../strategies/compareState";
import { examplesFor } from "../../../strategies/presets";
import { compareStore } from "../../../strategies/useCompare";
import { useEngine } from "../../../state/engine";
import { EngineCheck } from "../EngineCheck";
import { ParamFields } from "../ParamFields";
import { PresetMenu, type PresetHost } from "../PresetMenu";
import { StrategyKinds } from "../StrategyKinds";
import { useHandoff } from "./useHandoff";

/** The editable configuration of ONE side of the comparison. Editing it changes nothing else: not the other side, not a result. */
export function MemberEditor({ state, member }: { state: CompareState; member: Member }) {
  const { lab } = state;
  const side = state.members[member];
  const spec = memberSpec(state, member);
  const issues = memberIssues(state, member);
  const check = memberCheck(state, member);
  const flagged = check.phase === "rejected" ? check.error.fields : [];
  const kinds = lab.catalog?.strategies ?? [];
  const builtin = state.dataset.kind === "builtin" ? lab.datasets.find((d) => d.key === (state.dataset as { key: string }).key) : undefined;

  const presets: PresetHost = {
    presets: lab.presets, catalog: lab.catalog, examples: examplesFor(builtin, kinds),
    save: (name) => compareStore.savePreset(member, name), load: (id) => compareStore.loadPreset(member, id), remove: compareStore.deletePreset,
    useExample: (draft, label) => void compareStore.loadDraft(member, draft, label),
  };

  const { info } = useEngine();
  const { blocked, go: toBacktest } = useHandoff(state, member).edited;

  return (
    <section className="cmp-member" data-member={member} aria-label={`Configuration ${member}`}>
      <header className="cmp-member-head">
        <span className="cmp-chip" data-member={member}>{member}</span>
        <h3>Configuration {member}</h3>
        <PresetMenu host={presets} side={member} />
      </header>
      <StrategyKinds kinds={kinds} value={side.draft.kind} onSelect={(kind) => compareStore.selectKind(member, kind)} label={`Strategy for ${member}`} />
      {spec && (
        <>
          {side.origin && <p className="faint lab-help">Loaded from: {side.origin.label}{side.origin.edited ? " (edited since)" : ""}</p>}
          <ParamFields spec={spec} values={side.draft.values} issues={issues} flagged={flagged} skip={["symbols"]} onChange={(name, text) => compareStore.setValue(member, name, text)} />
          <p className="faint lab-help">Symbols: <span className="id">{side.draft.values.symbols || "none yet"}</span> (shared with {member === "A" ? "B" : "A"}; set above).</p>
          <EngineCheck check={check} issueCount={issues.length} side={member} />
        </>
      )}
      <div className="stack" style={{ gap: 6 }}>
        <div className="row" style={{ flexWrap: "wrap" }}>
          <button type="button" className="btn btn-ghost btn-sm" disabled={!spec} onClick={() => compareStore.restoreDefaults(member)} title="Reset every parameter of this configuration to the engine's default. The shared symbols stay.">Restore defaults</button>
          <button type="button" className="btn btn-sm" disabled={!!blocked} onClick={toBacktest} aria-describedby={`cmp-bt-${member}`}>Use {member} in new backtest</button>
        </div>
        <p id={`cmp-bt-${member}`} className="lab-help" data-tone={blocked ? "warn" : undefined}>
          {blocked ?? `Pre-fills the New backtest form with configuration ${member} as edited here (not necessarily the one the comparison on screen ran). ${destinationText(info?.data_source, compareDataset(state)?.name)} Nothing starts until you press Run backtest there.`}
        </p>
      </div>
    </section>
  );
}
