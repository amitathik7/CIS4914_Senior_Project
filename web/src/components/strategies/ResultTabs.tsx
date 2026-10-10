import type { ReactNode } from "react";
import { etClock, etDay, etFull } from "../../lib/exactTime";
import type { StrategySpec } from "../../strategies/catalog";
import { counts, eventsThrough, resultOf, signalsThrough } from "../../strategies/cursor";
import { ACTION_TEXT } from "../../strategies/explain";
import { diagnosticsCsv, download, exportName, runDetailsJson, signalsCsv, type ExportScope } from "../../strategies/exports";
import type { ReplayEvent, ReplaySignal } from "../../strategies/replay";
import type { Snapshot } from "../../strategies/snapshot";
import { decimalKey, intKey, timeKey } from "../../strategies/sortKeys";
import type { ResultTab } from "../../strategies/store";
import { labStore } from "../../strategies/useLab";
import { DataTable, type Column } from "../DataTable";
import { IconDownload } from "../icons";
import { Badge, Empty, Panel, Segmented, Side } from "../ui";
import { RunDetails } from "./RunDetails";

const VERDICT_TONE = { evaluated: undefined, warming_up: "accent", ignored: "warn" } as const;

function ExportBar({ children }: { children: ReactNode }) {
  return <div className="lab-exports">{children}</div>;
}

function ExportButton({ label, scope, onClick }: { label: string; scope: string; onClick: () => void }) {
  return (
    <button type="button" className="btn btn-sm" onClick={onClick}>
      <IconDownload size={13} /> {label} <span className="faint">· {scope}</span>
    </button>
  );
}

export function ResultTabs({ snapshot, cursor, symbol, spec, tab, multiDay }: { snapshot: Snapshot; cursor: number; symbol: string | null; spec: StrategySpec | undefined; tab: ResultTab; multiDay: boolean }) {
  const { model } = snapshot;
  const signals = signalsThrough(model, cursor, symbol);
  const events = eventsThrough(model, cursor, symbol);
  const when = (iso: string) => (multiDay ? `${etDay(iso).slice(5)} ${etClock(iso)}` : etClock(iso));
  const selectedEventIndex = cursor > 0 ? cursor - 1 : undefined;
  const select = (eventIndex: number) => labStore.setCursor(eventIndex + 1);

  const save = (kind: "signals" | "diagnostics", scope: ExportScope) =>
    download(
      exportName(kind, scope, snapshot, cursor),
      kind === "signals" ? signalsCsv(model, scope, cursor, symbol) : diagnosticsCsv(model, spec, scope, cursor, symbol),
      "text/csv;charset=utf-8",
    );

  const signalColumns: Column<ReplaySignal>[] = [
    { key: "id", header: "Signal", render: (s) => <span className="id">#{s.id}</span>, sort: (s) => intKey(s.id) },
    { key: "event", header: "Event", align: "right", render: (s) => s.eventIndex + 1, sort: (s) => s.eventIndex },
    { key: "time", header: "Time (ET)", render: (s) => <span title={etFull(s.createdAt)}>{when(s.createdAt)}</span>, sort: (s) => timeKey(s.createdAt) },
    { key: "symbol", header: "Symbol", render: (s) => <b>{s.symbol}</b>, sort: (s) => s.symbol },
    { key: "side", header: "Request", render: (s) => <Side side={s.side as "buy" | "sell"} />, sort: (s) => s.side },
    { key: "qty", header: "Quantity", align: "right", render: (s) => <span className="id">{s.quantity ?? "—"}</span>, sort: (s) => intKey(s.quantity) },
    { key: "strategy", header: "Strategy", render: (s) => <span className="muted">{s.strategyId}</span>, sort: (s) => s.strategyId },
    { key: "trigger", header: "Trigger", render: (s) => <span className="muted">{(s.metadata.trigger ?? "").replace(/_/g, " ")}</span> },
  ];

  const diagColumns: Column<ReplayEvent>[] = [
    { key: "event", header: "Event", align: "right", render: (e) => e.index + 1, sort: (e) => e.index },
    { key: "time", header: "Time (ET)", render: (e) => <span title={etFull(e.time)}>{when(e.time)}</span>, sort: (e) => timeKey(e.time) },
    { key: "symbol", header: "Symbol", render: (e) => <b>{e.symbol}</b>, sort: (e) => e.symbol },
    { key: "price", header: "Price", align: "right", render: (e) => <span className="id">{e.price ?? "—"}</span>, sort: (e) => decimalKey(e.price) },
    {
      key: "verdict", header: "Verdict", sort: (e) => resultOf(model, e).verdict ?? "",
      render: (e) => { const r = resultOf(model, e); return r.available ? <Badge tone={VERDICT_TONE[r.verdict as keyof typeof VERDICT_TONE]}>{r.verdict?.replace(/_/g, " ")}</Badge> : <Badge tone="warn">no diagnostics</Badge>; },
    },
    { key: "reason", header: "Reason", render: (e) => <span className="id">{resultOf(model, e).reason ?? "—"}</span>, sort: (e) => resultOf(model, e).reason ?? "" },
    { key: "action", header: "Request", render: (e) => { const a = resultOf(model, e).action; return a === "buy" || a === "sell" ? <Side side={a} /> : <span className="faint">{a ? ACTION_TEXT[a] ?? a : "—"}</span>; } },
    { key: "window", header: "Window", render: (e) => { const w = resultOf(model, e).window; return <span className="muted">{w?.fill === undefined ? "—" : `${w.fill}/${w.size}`}</span>; } },
    {
      key: "indicators", header: "Indicators",
      render: (e) => {
        const r = resultOf(model, e);
        const shown = Object.entries(r.indicators).map(([k, v]) => `${k} ${v.text}`);
        return <span className="muted id">{shown.length ? shown.join(" · ") : Object.keys(r.unavailable).length ? "unavailable" : "—"}</span>;
      },
    },
  ];

  const prefix = counts(model, cursor, symbol);
  const reasons = [...prefix.reasons.entries()].sort((a, b) => b[1] - a[1]);

  return (
    <Panel
      title="Details"
      hint={tab === "details" ? "The run that produced this result" : `Events 1–${cursor} of ${model.total}${symbol ? ` · ${symbol} only` : ""}`}
      tools={
        <Segmented<ResultTab> label="Details view" value={tab} onChange={(t) => labStore.setTab(t)} options={[
          { value: "signals", label: `Signals ${signals.length}` },
          { value: "diagnostics", label: `Diagnostics ${events.length}` },
          { value: "details", label: "Run details" },
        ]} />
      }
      flush={tab !== "details"}
    >
      {tab === "signals" && (
        <>
          <DataTable
            label="Signal requests so far" rows={signals} columns={signalColumns} rowKey={(s) => s.ref} initialSort={{ key: "event", dir: "asc" }}
            onRowClick={(s) => select(s.eventIndex)} selectedKey={signals.find((s) => s.eventIndex === selectedEventIndex)?.ref} maxHeight={360} limit={500}
            empty={<Empty title={cursor === 0 ? "Nothing revealed yet" : "No signal requests so far"}>{cursor === 0 ? "Step forward to reveal rows." : "A strategy makes a request only when its rule fires; most rows are silent, and a silent row is not a Hold signal."}</Empty>}
          />
          <ExportBar>
            <ExportButton label="Signals CSV" scope={`visible prefix (${signals.length} rows)`} onClick={() => save("signals", "visible_prefix")} />
            <ExportButton label="Signals CSV" scope={`full run (${model.signals.length} rows)`} onClick={() => save("signals", "full_run")} />
            <span className="faint">Values are written exactly as recorded; a spreadsheet may read a leading =, +, - or @ as a formula.</span>
          </ExportBar>
        </>
      )}
      {tab === "diagnostics" && (
        <>
          {reasons.length > 0 && (
            <div className="lab-reasons" aria-label="Reason counts so far">
              {reasons.map(([code, n]) => <span key={code} className="badge"><span className="id">{code}</span> {n}</span>)}
            </div>
          )}
          <DataTable
            label="Diagnostics so far" rows={events} columns={diagColumns} rowKey={(e) => e.index} initialSort={{ key: "event", dir: "desc" }}
            onRowClick={(e) => select(e.index)} selectedKey={selectedEventIndex} maxHeight={420} limit={500}
            empty={<Empty title="Nothing revealed yet">Step forward to reveal rows.</Empty>}
          />
          <ExportBar>
            <ExportButton label="Diagnostics CSV" scope={`visible prefix (${events.length} rows)`} onClick={() => save("diagnostics", "visible_prefix")} />
            <ExportButton label="Diagnostics CSV" scope={`full run (${model.total} rows)`} onClick={() => save("diagnostics", "full_run")} />
            {events.length > 500 && <span className="faint">The table shows 500 rows; the CSV has all {events.length}.</span>}
          </ExportBar>
        </>
      )}
      {tab === "details" && (
        <RunDetails
          snapshot={snapshot}
          onJson={() => download(`strategy-lab-run_${snapshot.doc.runId}.json`, snapshot.raw, "application/json;charset=utf-8")}
          onMetadata={() => download(`run-details_${snapshot.doc.runId}.json`, runDetailsJson(snapshot, new Date().toISOString()), "application/json;charset=utf-8")}
        />
      )}
    </Panel>
  );
}
