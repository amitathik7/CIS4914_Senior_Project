import { Link, useParams } from "react-router";
import type { StrategyConfig } from "../../api/contract";
import { draftFromRecorded } from "../../strategies/backtestHandoff";
import { specOf } from "../../strategies/catalog";
import { paramCopy } from "../../strategies/copy";
import { defaultValues } from "../../strategies/draft";
import type { LabState } from "../../strategies/store";
import { labStore } from "../../strategies/useLab";
import { useEngine, useRunResource } from "../../state/engine";
import { Badge, Empty, Loading, Notice, Panel } from "../ui";

const SHOWN = ["short_window", "long_window", "lookback", "entry_threshold", "rearm_threshold", "requested_quantity"] as const;

/** The strategies the selected Console run actually used. A record, not a form: it can be copied into the draft, never edited. */
export function RecordedSettings({ state }: { state: LabState }) {
  const { runId } = useParams();
  const { info } = useEngine();
  const run = useRunResource(runId, (c, id) => c.getRun(id));
  const demo = info?.data_source === "demo";

  const body = () => {
    if (!runId) return <Empty title="No run selected">Choose a run in the selector above to see the settings it used.</Empty>;
    if (run.error && !run.data) return <Empty title="Could not load this run">{run.error.message}</Empty>;
    if (!run.data) return <Loading height={120} />;
    const { config } = run.data;
    return (
      <div className="stack" style={{ gap: 10 }}>
        <Notice>
          These are the settings <b>{run.data.name}</b> ({run.data.mode === "live" ? "a live run" : "a backtest"}) was started with. They are an immutable record: selecting one here or editing your draft never changes a run, running or finished. Load a copy into your draft if you want to build on it.
        </Notice>
        {config.strategies.map((s: StrategyConfig) => {
          const spec = specOf(state.catalog, s.kind);
          const load = () => spec && labStore.loadDraft(draftFromRecorded(spec, s, defaultValues(spec)), `Recorded in ${run.data!.name}: ${s.strategy_id}`);
          return (
            <div className="strategy-card" key={s.strategy_id}>
              <div className="strategy-card-head" style={{ flexWrap: "wrap" }}>
                <strong>{spec?.title ?? s.kind}</strong>
                <span className="id">{s.strategy_id}</span>
                <Badge>Recorded · read-only</Badge>
                {demo && <span title="No engine API is configured, so this run comes from the console's in-browser demo engine, not the C++ engine."><Badge tone="warn">Demo data</Badge></span>}
              </div>
              <dl className="kv">
                <dt>Symbols</dt><dd>{s.symbols.join(", ")}</dd>
                {SHOWN.filter((k) => s[k] !== undefined).map((k) => <div key={k} style={{ display: "contents" }}><dt>{paramCopy(k).label}</dt><dd>{String(s[k])}{paramCopy(k).unit ? ` ${paramCopy(k).unit}` : ""}</dd></div>)}
              </dl>
              <div className="row" style={{ marginTop: 12, flexWrap: "wrap" }}>
                <button type="button" className="btn btn-sm" disabled={!spec?.available} onClick={load} title={spec?.available ? "Copy these settings into the editable draft" : "The engine does not offer this strategy"}>Load as draft</button>
                <Link className="btn btn-ghost btn-sm" to={`/runs/${runId}/orders`}>This run's orders</Link>
                <Link className="btn btn-ghost btn-sm" to={`/runs/${runId}/risk`}>Risk decisions</Link>
              </div>
            </div>
          );
        })}
      </div>
    );
  };

  return (
    <Panel title="Recorded in this run" hint={run.data ? run.data.name : undefined}>
      {body()}
    </Panel>
  );
}
