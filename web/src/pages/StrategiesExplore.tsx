import { useEffect, useMemo, useRef } from "react";
import { ConfigPanel } from "../components/strategies/ConfigPanel";
import { DecisionInspector } from "../components/strategies/DecisionInspector";
import { ReplayChart } from "../components/strategies/ReplayChart";
import { ReplayControls } from "../components/strategies/ReplayControls";
import { PrefixPanels, ResultHeader, RunFailure } from "../components/strategies/ResultStatus";
import { ResultTabs } from "../components/strategies/ResultTabs";
import { Empty, Panel } from "../components/ui";
import { etDay } from "../lib/exactTime";
import { specOf } from "../strategies/catalog";
import { labStore, useLab } from "../strategies/useLab";

export function StrategiesExplore() {
  const state = useLab();
  const snap = state.snapshot;
  const spec = snap ? specOf(state.catalog, snap.request.kind) : undefined;
  // The configuration panel can be far taller than the window, so when a run finishes (or fails) bring the outcome into view.
  const top = useRef<HTMLDivElement>(null);
  const first = useRef(true);
  const outcome = `${snap?.id ?? 0}:${state.run.phase === "failed" ? state.run.token : 0}`;
  useEffect(() => {
    if (first.current) {
      first.current = false;
      return;
    }
    const el = top.current;
    if (el && el.getBoundingClientRect().top < 0) el.scrollIntoView({ behavior: "smooth", block: "start" });
  }, [outcome]);
  const multiDay = useMemo(() => (snap ? new Set(snap.model.events.map((e) => etDay(e.time))).size > 1 : false), [snap]);

  return (
    <div className="lab">
      <aside className="lab-side"><ConfigPanel state={state} onRun={() => void labStore.runReplay()} /></aside>
      <div className="lab-main" ref={top}>
        <RunFailure state={state} />
        <ResultHeader state={state} snapshot={snap} />
        {snap ? (
          <>
            <Panel
              title="Price and decisions"
              hint={`${snap.strategyTitle} · ${snap.model.symbols.join(", ")}`}
              tools={
                <label className="row" title="Filters what is shown from this result. It never reruns the engine or changes a decision.">
                  <span className="muted" style={{ fontSize: "var(--fs-sm)" }}>Display symbol</span>
                  <select className="select" style={{ width: 150 }} value={state.symbol ?? ""} onChange={(e) => labStore.setSymbol(e.target.value || null)} aria-label="Display symbol">
                    <option value="">All symbols</option>
                    {snap.model.symbols.map((s) => <option key={s} value={s}>{s}</option>)}
                  </select>
                </label>
              }
            >
              <div className="stack" style={{ gap: 14 }}>
                <ReplayChart model={snap.model} cursor={state.cursor} symbol={state.symbol} onSelect={labStore.setCursor} onStep={(d) => labStore.step(d)} />
                <ReplayControls model={snap.model} cursor={state.cursor} symbol={state.symbol} onStep={labStore.step} onCursor={labStore.setCursor} />
              </div>
            </Panel>
            <div className="lab-split">
              <DecisionInspector snapshot={snap} cursor={state.cursor} symbol={state.symbol} spec={spec} />
              <PrefixPanels snapshot={snap} cursor={state.cursor} symbol={state.symbol} />
            </div>
            <ResultTabs snapshot={snap} cursor={state.cursor} symbol={state.symbol} spec={spec} tab={state.tab} multiDay={multiDay} />
          </>
        ) : (
          state.run.phase !== "running" && (
            <Panel>
              <Empty title="No replay yet">
                Choose a dataset and a strategy, then press Run replay. The result opens here, with the chart, a decision inspector for every row, and the signals the strategy requested.
              </Empty>
            </Panel>
          )
        )}
      </div>
    </div>
  );
}
