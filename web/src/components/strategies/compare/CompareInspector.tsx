import { etFull } from "../../../lib/exactTime";
import { MEMBERS, signalsAt, type Member } from "../../../strategies/compare";
import { agreementSentence, compareEvent, type FieldRow } from "../../../strategies/compareEvent";
import type { ComparisonSnapshot } from "../../../strategies/compareState";
import { specOf, type Catalog } from "../../../strategies/catalog";
import { resultOf, selectedEvent, signalsOn } from "../../../strategies/cursor";
import { explainEvent, sharesText } from "../../../strategies/explain";
import { Badge, Empty, Notice, Panel, Side } from "../../ui";

const STATUS: Record<FieldRow["status"], { text: string; label: string }> = {
  same: { text: "same", label: "Same in A and B" },
  differs: { text: "differs", label: "Differs between A and B" },
  not_comparable: { text: "n/a", label: "Only one configuration reports this" },
};
const GROUP_TEXT: Record<FieldRow["group"], string> = { decision: "Decision", readiness: "Readiness", indicator: "Indicators", state: "State" };

function RelationBadge({ relation }: { relation: "agree" | "differ" | "not_comparable" }) {
  return relation === "agree" ? <Badge>Agree</Badge> : relation === "differ" ? <Badge tone="warn">Requests differ</Badge> : <Badge tone="accent">Not comparable</Badge>;
}

/** One recorded event with both configurations' decisions side by side. Everything is read from each side's own result. */
export function CompareInspector({ done, cursor, symbol, catalog }: { done: ComparisonSnapshot; cursor: number; symbol: string | null; catalog: Catalog | undefined }) {
  const { model } = done;
  const event = selectedEvent(model.members.A, cursor);
  if (!event) {
    return (
      <Panel title="Event inspector" hint="What A and B decided on each row">
        <Empty title="Nothing revealed yet">At position 0 no row has been revealed. Press Next event, Next difference or drag the slider to inspect the first recorded row.</Empty>
      </Panel>
    );
  }
  const row = model.rows[event.index]!;
  const fields = compareEvent(model, event.index);
  const requests = signalsAt(model, event.index);
  const other = (m: Member) => model.members[m].events[event.index]!;
  const why = Object.fromEntries(MEMBERS.map((m) => {
    const side = model.members[m];
    return [m, explainEvent(specOf(catalog, side.kind), other(m), resultOf(side, other(m)), signalsOn(side, event.index))];
  })) as Record<Member, ReturnType<typeof explainEvent>>;
  const groups = (Object.keys(GROUP_TEXT) as FieldRow["group"][]).filter((g) => fields.some((f) => f.group === g));

  return (
    <Panel title="Event inspector" hint={`Event ${event.index + 1} of ${model.total} · ${event.symbol} · ${event.type}`}>
      <div className="stack" style={{ gap: 12 }}>
        {symbol !== null && event.symbol !== symbol && (
          <Notice>This event belongs to {event.symbol}, not the displayed symbol ({symbol}). The position follows the recorded order of all symbols.</Notice>
        )}
        <div className="inspect-why" data-tone={row.relation === "differ" ? "warn" : row.relation === "agree" ? undefined : "accent"}>
          <div className="row" style={{ flexWrap: "wrap" }}><strong>{event.symbol} at {event.price ?? "no price"}</strong><RelationBadge relation={row.relation} /></div>
          <p>{agreementSentence(model, event.index)}</p>
        </div>

        <div className="cmp-why">
          {MEMBERS.map((m) => (
            <div key={m} className="inspect-why" data-tone={why[m].tone}>
              <div className="row" style={{ flexWrap: "wrap" }}><span className="cmp-chip" data-member={m}>{m}</span><strong>{why[m].title}</strong><Badge tone={why[m].badge.tone}>{why[m].badge.text}</Badge></div>
              <p>{why[m].why}</p>
            </div>
          ))}
        </div>

        <div className="table-wrap"><table className="table lab-mini cmp-fields" aria-label="A and B on the selected event">
          <thead>
            <tr>
              <th scope="col">Field</th>
              <th scope="col"><span className="cmp-chip" data-member="A">A</span></th>
              <th scope="col"><span className="cmp-chip" data-member="B">B</span></th>
              <th scope="col">A vs B</th>
            </tr>
          </thead>
          {groups.map((g) => (
            <tbody key={g}>
              <tr className="cmp-group"><th scope="rowgroup" colSpan={4}>{GROUP_TEXT[g]}</th></tr>
              {fields.filter((f) => f.group === g).map((f) => (
                <tr key={`${g}.${f.label}`} data-diff={f.status === "differs" ? "true" : undefined}>
                  <th scope="row"><span className={g === "indicator" || f.label === "Reason code" ? "id" : undefined}>{f.label}</span></th>
                  <td className="wrap id">{f.a}</td>
                  <td className="wrap id">{f.b}</td>
                  <td><span className="cmp-status" data-status={f.status} aria-label={STATUS[f.status].label}>{f.status === "differs" ? "≠ " : f.status === "same" ? "= " : ""}{STATUS[f.status].text}</span></td>
                </tr>
              ))}
            </tbody>
          ))}
        </table></div>

        {requests.length > 0 ? (
          <div className="table-wrap"><table className="table lab-mini" aria-label="Requests on the selected event">
            <thead><tr><th scope="col">Configuration</th><th scope="col">Request</th><th scope="col" className="r">Quantity</th><th scope="col">Scoped signal</th></tr></thead>
            <tbody>
              {requests.map((r) => (
                <tr key={r.key}>
                  <td><span className="cmp-chip" data-member={r.member}>{r.member}</span></td>
                  <td><Side side={r.signal.side as "buy" | "sell"} /></td>
                  <td className="r">{r.signal.quantity ? sharesText(r.signal.quantity) : "—"}</td>
                  <td className="id">{r.key}</td>
                </tr>
              ))}
            </tbody>
          </table></div>
        ) : <p className="muted" style={{ fontSize: "var(--fs-sm)" }}>Neither configuration made a request on this row. A silent row is not a Hold signal.</p>}

        <details className="lab-details">
          <summary>Identifiers and timestamps</summary>
          <dl className="kv" style={{ marginTop: 8 }}>
            <dt>Exchange time (ET)</dt><dd className="id">{etFull(event.time)}</dd>
            <dt>Exchange time (UTC)</dt><dd className="id">{event.time}</dd>
            <dt>Event identity</dt><dd className="id">index {event.index} (0-based) · event {event.index + 1} of {model.total} · source line {event.sourceLine ?? "unknown"} · the same row in A and B</dd>
            {MEMBERS.map((m) => <div key={m} style={{ display: "contents" }}><dt>Bus sequence {m}</dt><dd className="id">{other(m).busSequence} <span className="faint">(this run's own numbering; not used to join A and B)</span></dd></div>)}
            {MEMBERS.map((m) => <div key={m} style={{ display: "contents" }}><dt>Run {m}</dt><dd className="id">{done.snapshots[m].doc.runId} · strategy {resultOf(model.members[m], other(m)).strategyId}</dd></div>)}
            {event.open !== undefined && <><dt>Open / high / low</dt><dd className="id">{event.open} / {event.high ?? "?"} / {event.low ?? "?"}</dd></>}
            {event.volume !== undefined && <><dt>Volume</dt><dd className="id">{event.volume}</dd></>}
          </dl>
        </details>
      </div>
    </Panel>
  );
}
