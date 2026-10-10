import { useMemo, useState } from "react";
import { etClock, etDay, etFull } from "../../../lib/exactTime";
import { compareSignalsThrough, MEMBERS, type CompareSignal, type EventRow, type Member } from "../../../strategies/compare";
import { bundleName, bundleZip, csvName, agreementCsv, eventsCsv, signalsCsv, summaryCsv } from "../../../strategies/compareExports";
import type { CompareTab, ComparisonSnapshot } from "../../../strategies/compareState";
import { requestText } from "../../../strategies/compareEvent";
import { rowsThrough } from "../../../strategies/compareSummary";
import { resultOf } from "../../../strategies/cursor";
import { ACTION_TEXT } from "../../../strategies/explain";
import { download, type ExportScope } from "../../../strategies/exports";
import { decimalKey, intKey, timeKey } from "../../../strategies/sortKeys";
import { compareStore } from "../../../strategies/useCompare";
import { DataTable, type Column } from "../../DataTable";
import { IconDownload } from "../../icons";
import { Badge, Empty, Panel, Segmented, Side } from "../../ui";
import { CompareDetails } from "./CompareDetails";

const RELATION: Record<EventRow["relation"], { text: string; tone: "accent" | "warn" | undefined }> = {
  agree: { text: "agree", tone: undefined }, differ: { text: "requests differ", tone: "warn" }, not_comparable: { text: "not comparable", tone: "accent" },
};

const Chip = ({ m }: { m: Member }) => <span className="cmp-chip" data-member={m}>{m}</span>;

function ExportButton({ label, scope, onClick }: { label: string; scope: string; onClick: () => void }) {
  return (
    <button type="button" className="btn btn-sm" onClick={onClick}>
      <IconDownload size={13} /> {label} <span className="faint">· {scope}</span>
    </button>
  );
}

function Exports({ done, cursor, symbol }: { done: ComparisonSnapshot; cursor: number; symbol: string | null }) {
  const save = (kind: "signals" | "events" | "summary" | "agreement", scope: ExportScope) => {
    const build = { signals: signalsCsv, events: eventsCsv, summary: summaryCsv, agreement: agreementCsv }[kind];
    download(csvName(kind, scope, done, cursor, symbol), build(done, scope, cursor, symbol), "text/csv;charset=utf-8");
  };
  const rows = rowsThrough(done.model, cursor, symbol).length;
  const group = (scope: ExportScope, caption: string) => (
    <div className="lab-exports" role="group" aria-label={caption}>
      <b>{caption}</b>
      {(["signals", "events", "summary", "agreement"] as const).map((k) => <ExportButton key={k} label={`${k[0]!.toUpperCase()}${k.slice(1)} CSV`} scope={scope === "full_run" ? "full run" : "visible prefix"} onClick={() => save(k, scope)} />)}
    </div>
  );
  return (
    <>
      {group("visible_prefix", `Replay so far (events 1–${cursor}${symbol ? `, ${symbol}` : ""}; ${rows} rows)`)}
      {group("full_run", `Full run (all ${done.model.total} events)`)}
      <div className="lab-exports">
        <button type="button" className="btn btn-sm btn-primary" onClick={() => download(bundleName(done), bundleZip(done), "application/zip")}>
          <IconDownload size={13} /> Comparison bundle (ZIP) <span className="faint">· full run</span>
        </button>
        <span className="faint">Both configurations, the dataset and engine identities, both runs exactly as the engine wrote them, and the full-run CSVs. Exporting twice gives identical bytes. Values are exact; a spreadsheet may read a leading =, +, - or @ as a formula.</span>
      </div>
    </>
  );
}

export function CompareTables({ done, cursor, symbol, tab, multiDay }: { done: ComparisonSnapshot; cursor: number; symbol: string | null; tab: CompareTab; multiDay: boolean }) {
  const { model } = done;
  const [onlyDifferences, setOnlyDifferences] = useState(false);
  const signals = useMemo(() => compareSignalsThrough(model, cursor, symbol), [model, cursor, symbol]);
  const allRows = useMemo(() => rowsThrough(model, cursor, symbol), [model, cursor, symbol]);
  const rows = useMemo(() => (onlyDifferences ? allRows.filter((r) => r.requestsDiffer || r.standingsDiffer) : allRows), [allRows, onlyDifferences]);
  const when = (iso: string) => (multiDay ? `${etDay(iso).slice(5)} ${etClock(iso)}` : etClock(iso));
  const selectedIndex = cursor > 0 ? cursor - 1 : undefined;
  const select = (eventIndex: number) => compareStore.setCursor(eventIndex + 1);

  const signalColumns: Column<CompareSignal>[] = [
    { key: "member", header: "Config", render: (s) => <Chip m={s.member} />, sort: (s) => s.member },
    { key: "id", header: "Signal", render: (s) => <span className="id">#{s.signal.id}</span>, sort: (s) => intKey(s.signal.id) },
    { key: "key", header: "Scoped key", render: (s) => <span className="id muted">{s.key}</span>, sort: (s) => s.key },
    { key: "event", header: "Event", align: "right", render: (s) => s.signal.eventIndex + 1, sort: (s) => s.signal.eventIndex },
    { key: "time", header: "Time (ET)", render: (s) => <span title={etFull(s.signal.createdAt)}>{when(s.signal.createdAt)}</span>, sort: (s) => timeKey(s.signal.createdAt) },
    { key: "symbol", header: "Symbol", render: (s) => <b>{s.signal.symbol}</b>, sort: (s) => s.signal.symbol },
    { key: "side", header: "Request", render: (s) => <Side side={s.signal.side as "buy" | "sell"} />, sort: (s) => s.signal.side },
    { key: "qty", header: "Quantity", align: "right", render: (s) => <span className="id">{s.signal.quantity ?? "—"}</span>, sort: (s) => intKey(s.signal.quantity) },
    { key: "trigger", header: "Trigger", render: (s) => <span className="muted">{(s.signal.metadata.trigger ?? "").replace(/_/g, " ")}</span> },
  ];

  const verdictCell = (m: Member, index: number) => {
    const r = resultOf(model.members[m], model.members[m].events[index]!);
    return r.available ? (r.verdict ?? "").replace(/_/g, " ") : "no diagnostics";
  };
  const eventColumns: Column<EventRow>[] = [
    { key: "event", header: "Event", align: "right", render: (r) => r.index + 1, sort: (r) => r.index },
    { key: "time", header: "Time (ET)", render: (r) => <span title={etFull(model.events[r.index]!.time)}>{when(model.events[r.index]!.time)}</span>, sort: (r) => timeKey(model.events[r.index]!.time) },
    { key: "symbol", header: "Symbol", render: (r) => <b>{model.events[r.index]!.symbol}</b>, sort: (r) => model.events[r.index]!.symbol },
    { key: "price", header: "Price", align: "right", render: (r) => <span className="id">{model.events[r.index]!.price ?? "—"}</span>, sort: (r) => decimalKey(model.events[r.index]!.price) },
    ...MEMBERS.flatMap((m): Column<EventRow>[] => [
      { key: `${m}v`, header: `${m} verdict`, render: (r) => <span className="muted">{verdictCell(m, r.index)}</span>, sort: (r) => verdictCell(m, r.index) },
      { key: `${m}r`, header: `${m} request`, render: (r) => { const s = r.sides[m]; return s === "buy" || s === "sell" ? <Side side={s} /> : <span className="faint">{s ? requestText(s) : ACTION_TEXT.none}</span>; }, sort: (r) => r.sides[m] },
    ]),
    { key: "rel", header: "A vs B", render: (r) => <Badge tone={RELATION[r.relation].tone}>{RELATION[r.relation].text}</Badge>, sort: (r) => r.relation },
  ];

  return (
    <Panel
      title="Details"
      hint={tab === "details" ? "The two runs behind this comparison" : `Events 1–${cursor} of ${model.total}${symbol ? ` · ${symbol} only` : ""}`}
      tools={
        <Segmented<CompareTab> label="Details view" value={tab} onChange={(t) => compareStore.setTab(t)} options={[
          { value: "signals", label: `Signals ${signals.length}` },
          { value: "events", label: `Events ${allRows.length}` },
          { value: "details", label: "Run details" },
        ]} />
      }
      flush={tab !== "details"}
    >
      {tab === "signals" && (
        <DataTable
          label="Requests so far, both configurations" rows={signals} columns={signalColumns} rowKey={(s) => s.key} initialSort={{ key: "event", dir: "asc" }}
          onRowClick={(s) => select(s.signal.eventIndex)} selectedKey={signals.find((s) => s.signal.eventIndex === selectedIndex)?.key} maxHeight={360} limit={500}
          empty={<Empty title={cursor === 0 ? "Nothing revealed yet" : "No requests so far"}>{cursor === 0 ? "Step forward to reveal rows." : "Neither configuration has made a request yet. Most rows are silent, and a silent row is not a Hold signal."}</Empty>}
        />
      )}
      {tab === "events" && (
        <>
          <div className="lab-reasons">
            <label className="row"><input type="checkbox" checked={onlyDifferences} onChange={(e) => setOnlyDifferences(e.target.checked)} /> <span>Only rows where requests or verdicts differ</span></label>
            <span className="faint">{rows.length} of {allRows.length} rows</span>
          </div>
          <DataTable
            label="Both configurations on every revealed row" rows={rows} columns={eventColumns} rowKey={(r) => r.index} initialSort={{ key: "event", dir: "desc" }}
            onRowClick={(r) => select(r.index)} selectedKey={selectedIndex} maxHeight={420} limit={500}
            empty={<Empty title={cursor === 0 ? "Nothing revealed yet" : "No rows to show"}>{cursor === 0 ? "Step forward to reveal rows." : "No revealed row differs."}</Empty>}
          />
        </>
      )}
      {tab === "details" && <CompareDetails done={done} />}
      <Exports done={done} cursor={cursor} symbol={symbol} />
    </Panel>
  );
}
