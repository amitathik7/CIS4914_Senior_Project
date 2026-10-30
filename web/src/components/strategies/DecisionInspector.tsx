import type { StrategySpec } from "../../strategies/catalog";
import { unavailableText } from "../../strategies/copy";
import { resultOf, selectedEvent, signalsOn } from "../../strategies/cursor";
import { ACTION_TEXT, explainEvent, sharesText, windowText } from "../../strategies/explain";
import type { Snapshot } from "../../strategies/snapshot";
import { etFull } from "../../lib/exactTime";
import { Badge, Empty, Notice, Panel } from "../ui";

export function DecisionInspector({ snapshot, cursor, symbol, spec }: { snapshot: Snapshot; cursor: number; symbol: string | null; spec: StrategySpec | undefined }) {
  const { model } = snapshot;
  const event = selectedEvent(model, cursor);
  if (!event) {
    return (
      <Panel title="Decision inspector" hint="Why a row did or did not produce a request">
        <Empty title="Nothing revealed yet">At position 0 no row has been revealed. Press Next event or drag the slider to inspect the first recorded decision.</Empty>
      </Panel>
    );
  }
  const result = resultOf(model, event);
  const signals = signalsOn(model, event.index);
  const why = explainEvent(spec, event, result, signals);
  const names = [...new Set([...(spec?.indicators ?? []), ...Object.keys(result.indicators), ...Object.keys(result.unavailable)])];
  const changes = Object.entries(result.states);

  return (
    <Panel title="Decision inspector" hint={`Event ${event.index + 1} of ${model.total} · ${event.symbol} · ${event.type}`}>
      <div className="stack" style={{ gap: 12 }}>
        {symbol !== null && event.symbol !== symbol && (
          <Notice>This event belongs to {event.symbol}, not the displayed symbol ({symbol}). The chart and tables show {symbol} only; the position follows the recorded order of all symbols.</Notice>
        )}
        <div className="inspect-why" data-tone={why.tone}>
          <div className="row" style={{ flexWrap: "wrap" }}>
            <strong>{why.title}</strong>
            <Badge tone={why.badge.tone}>{why.badge.text}</Badge>
          </div>
          <p>{why.why}</p>
          {why.notes.map((note) => <p key={note} className="faint">{note}</p>)}
        </div>

        <dl className="kv">
          <dt>Reason code</dt><dd><span className="id">{result.reason ?? "none recorded"}</span></dd>
          <dt>Outcome</dt><dd>{result.action ? ACTION_TEXT[result.action] ?? result.action : "not recorded"}</dd>
          <dt>Window</dt><dd>{result.available ? windowText(result) : "not reported"}</dd>
          <dt>{event.type === "bar" ? "Close" : "Price"}</dt><dd className="id">{event.price ?? "no price"}</dd>
          {signals.map((s) => (
            <div key={s.ref} style={{ display: "contents" }}>
              <dt>{s.side === "buy" ? "Buy" : "Sell"} request</dt>
              <dd>{s.quantity ? sharesText(s.quantity) : "no quantity"} · {s.orderType ?? "no order type"} · signal <span className="id">{s.ref}</span></dd>
            </div>
          ))}
        </dl>

        {names.length > 0 && result.available && (
          <table className="table lab-mini" aria-label="Indicator values at the selected event">
            <thead><tr><th scope="col">Indicator</th><th scope="col" className="r">Value</th><th scope="col">Why unavailable</th></tr></thead>
            <tbody>
              {names.map((name) => {
                const value = result.indicators[name];
                return (
                  <tr key={name}>
                    <td><span className="id">{name}</span></td>
                    <td className="r id">{value ? value.text : <span className="faint">unavailable</span>}</td>
                    <td className="muted wrap">{value ? "" : unavailableText(result.unavailable[name] ?? "not reported")}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}

        {changes.length > 0 && <p className="muted" style={{ fontSize: "var(--fs-sm)" }}>State: {changes.map(([k, v]) => `${k.replace(/_/g, " ")} = ${v}`).join("; ")}</p>}

        <details className="lab-details">
          <summary>Identifiers and timestamps</summary>
          <dl className="kv" style={{ marginTop: 8 }}>
            <dt>Exchange time (ET)</dt><dd className="id">{etFull(event.time)}</dd>
            <dt>Exchange time (UTC)</dt><dd className="id">{event.time}</dd>
            <dt>Event index</dt><dd className="id">{event.index} (0-based) · event {event.index + 1} of {model.total}</dd>
            <dt>Source line</dt><dd className="id">{event.sourceLine ?? "unknown"}</dd>
            <dt>Bus sequence</dt><dd className="id">{event.busSequence}</dd>
            <dt>Strategy id</dt><dd className="id">{result.strategyId}</dd>
            <dt>Run id</dt><dd className="id">{snapshot.doc.runId}</dd>
            {signals.map((s) => <div key={s.ref} style={{ display: "contents" }}><dt>Signal {s.id}</dt><dd className="id">created {s.createdAt}{Object.keys(s.metadata).length ? ` · ${Object.entries(s.metadata).map(([k, v]) => `${k}=${v}`).join("; ")}` : ""}</dd></div>)}
            {event.open !== undefined && <><dt>Open / high / low</dt><dd className="id">{event.open} / {event.high ?? "?"} / {event.low ?? "?"}</dd></>}
            {event.volume !== undefined && <><dt>Volume</dt><dd className="id">{event.volume}</dd></>}
          </dl>
        </details>
      </div>
    </Panel>
  );
}
