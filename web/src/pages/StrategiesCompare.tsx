import { useEffect, useMemo, useRef } from "react";
import { CompareChart } from "../components/strategies/compare/CompareChart";
import { CompareControls } from "../components/strategies/compare/CompareControls";
import { CompareInspector } from "../components/strategies/compare/CompareInspector";
import { CompareSetup } from "../components/strategies/compare/CompareSetup";
import { CompareAttempt, CompareHeader } from "../components/strategies/compare/CompareStatus";
import { CompareSummary } from "../components/strategies/compare/CompareSummary";
import { CompareTables } from "../components/strategies/compare/CompareTables";
import { FinancialPanel } from "../components/strategies/compare/FinancialPanel";
import { Empty, Panel } from "../components/ui";
import { etDay } from "../lib/exactTime";
import { compareStore, useCompare } from "../strategies/useCompare";

/** Compare: two configurations, one dataset, one position. Results come only from the button; everything below it is a view of finished results. */
export function StrategiesCompare() {
  const state = useCompare();
  const done = state.comparison;
  // The setup can be far taller than the window, so when a comparison finishes (or a launch fails) bring the outcome into view.
  const top = useRef<HTMLDivElement>(null);
  const first = useRef(true);
  const outcome = `${done?.id ?? 0}:${state.attempt?.launch ?? 0}:${state.attempt?.outcome ?? ""}`;
  useEffect(() => {
    if (first.current) {
      first.current = false;
      return;
    }
    const el = top.current;
    if (el && el.getBoundingClientRect().top < 0) el.scrollIntoView({ behavior: "smooth", block: "start" });
  }, [outcome]);
  const multiDay = useMemo(() => (done ? new Set(done.model.events.map((e) => etDay(e.time))).size > 1 : false), [done]);

  return (
    <div className="stack" style={{ gap: 14 }}>
      <CompareSetup state={state} />
      <div className="stack" style={{ gap: 14 }} ref={top}>
        <CompareAttempt state={state} />
        <CompareHeader state={state} />
        {done ? (
          <>
            <Panel
              title="Price and decisions"
              hint={`A and B · ${done.model.symbols.join(", ")}`}
              tools={
                <label className="row" title="Filters what is shown from this result. It never reruns either strategy or changes a decision.">
                  <span className="muted" style={{ fontSize: "var(--fs-sm)" }}>Display symbol</span>
                  <select className="select" style={{ width: 150 }} value={state.symbol ?? ""} onChange={(e) => compareStore.setSymbol(e.target.value || null)} aria-label="Display symbol">
                    <option value="">All symbols</option>
                    {done.model.symbols.map((s) => <option key={s} value={s}>{s}</option>)}
                  </select>
                </label>
              }
            >
              <div className="stack" style={{ gap: 14 }}>
                <CompareChart model={done.model} cursor={state.cursor} filter={state.symbol} onSelect={compareStore.setCursor} onStep={(d) => compareStore.step(d)} />
                <CompareControls model={done.model} cursor={state.cursor} symbol={state.symbol} mode={state.diffMode} onStep={compareStore.step} onCursor={compareStore.setCursor} onMode={compareStore.setDiffMode} />
              </div>
            </Panel>
            <div className="lab-split">
              <CompareInspector done={done} cursor={state.cursor} symbol={state.symbol} catalog={state.lab.catalog} />
              <CompareSummary done={done} cursor={state.cursor} symbol={state.symbol} />
            </div>
            <CompareTables done={done} cursor={state.cursor} symbol={state.symbol} tab={state.tab} multiDay={multiDay} />
          </>
        ) : (
          state.run.phase !== "running" && (
            <Panel>
              <Empty title="No comparison yet">
                Choose a dataset and two configurations, then press Run comparison. The result opens here: aligned charts for A and B, an inspector for every row, and the requests each made.
              </Empty>
            </Panel>
          )
        )}
      </div>
      <FinancialPanel state={state} />
    </div>
  );
}
