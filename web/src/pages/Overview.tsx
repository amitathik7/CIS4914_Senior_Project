import { useMemo, useState } from "react";
import { useParams } from "react-router";
import type { Position } from "../api/contract";
import { DataTable, type Column } from "../components/DataTable";
import { Pipeline } from "../components/Pipeline";
import { TimeChart } from "../components/charts/TimeChart";
import { useCurrentRun } from "../components/Shell";
import { Empty, Figure, Loading, Panel, Segmented, Side, toneOf } from "../components/ui";
import { add, formatDecimal, money, price, ratio, sub, toNumber } from "../lib/decimal";
import { int, label, minute, percent, signedInt } from "../lib/format";
import { useRunResource } from "../state/engine";
import { candles, equityLine, fillMarkers, lastSession } from "./chartData";
import { OrderDrawer } from "./OrderDrawer";

const moneyAxis = (v: number) => `$${v.toLocaleString("en-US", { maximumFractionDigits: 0 })}`;
const priceAxis = (v: number) => v.toFixed(2);

const positionColumns: Column<Position>[] = [
  { key: "symbol", header: "Symbol", render: (p) => <b>{p.symbol}</b>, sort: (p) => p.symbol },
  { key: "qty", header: "Quantity", align: "right", render: (p) => signedInt(p.quantity), sort: (p) => p.quantity },
  { key: "avg", header: "Avg cost", align: "right", render: (p) => (p.quantity ? price(p.average_cost) : "—") },
  { key: "mark", header: "Mark", align: "right", render: (p) => price(p.mark_price) },
  { key: "value", header: "Value", align: "right", render: (p) => money(p.market_value), sort: (p) => toNumber(p.market_value) },
  { key: "upnl", header: "Unrealized", align: "right", render: (p) => <span className={toneOf(p.unrealized_pnl)}>{money(p.unrealized_pnl, true)}</span>, sort: (p) => toNumber(p.unrealized_pnl) },
  { key: "rpnl", header: "Realized", align: "right", render: (p) => <span className={toneOf(p.realized_pnl)}>{money(p.realized_pnl, true)}</span>, sort: (p) => toNumber(p.realized_pnl) },
];

const compactColumns: Column<Position>[] = [
  positionColumns[0]!,
  positionColumns[1]!,
  positionColumns[4]!,
  {
    key: "pnl",
    header: "P&L",
    align: "right",
    render: (p) => {
      const total = add(p.realized_pnl, p.unrealized_pnl);
      return <span className={toneOf(total)}>{money(total, true)}</span>;
    },
    sort: (p) => toNumber(p.realized_pnl) + toNumber(p.unrealized_pnl),
  },
];

export function Overview() {
  const { runId } = useParams();
  const run = useCurrentRun();
  const live = run?.mode === "live" && run.status === "running";
  const portfolio = useRunResource(runId, (c, id) => c.portfolio(id));
  const detail = useRunResource(runId, (c, id) => c.getRun(id));
  const equity = useRunResource(runId, (c, id) => c.equity(id));
  const metrics = useRunResource(runId, (c, id) => c.metrics(id));
  const fills = useRunResource(runId, (c, id) => c.fills(id));
  const decisions = useRunResource(runId, (c, id) => c.riskDecisions(id));
  const [range, setRange] = useState<"session" | "all">("session");
  const symbols = run?.symbols ?? [];
  const [chosen, setSymbol] = useState<string>();
  const symbol = chosen && symbols.includes(chosen) ? chosen : symbols[0];
  const bars = useRunResource(runId, (c, id) => (symbol ? c.bars(id, symbol) : Promise.resolve([])), [symbol]);
  const [openOrder, setOpenOrder] = useState<number>();

  const equitySeries = useMemo(() => {
    const points = equity.data ?? [];
    return equityLine(range === "session" ? lastSession(points) : points);
  }, [equity.data, range]);

  const priceSeries = useMemo(() => candles(bars.data ?? []), [bars.data]);
  const markers = useMemo(() => (symbol ? fillMarkers(fills.data ?? [], symbol) : []), [fills.data, symbol]);

  const activity = useMemo(() => {
    const items: { at: string; key: string; orderId?: number; node: React.ReactNode }[] = [];
    for (const f of (fills.data ?? []).slice(-30)) {
      items.push({
        at: f.filled_at,
        key: `f${f.id}`,
        orderId: f.order_id,
        node: <><Side side={f.side} /> <b>{int(f.filled_quantity)} {f.symbol}</b> at {price(f.fill_price)}{f.status === "partially_filled" ? ", partial" : ""}</>,
      });
    }
    for (const d of (decisions.data ?? []).slice(-30)) {
      if (d.outcome === "approved") continue;
      items.push({
        at: d.decided_at,
        key: `d${d.id}`,
        orderId: d.order_id,
        node: d.outcome === "rejected"
          ? <><b className="down">Rejected</b> {d.side} {d.requested_quantity} {d.symbol}: {label(d.reason ?? "")}</>
          : <><b className="warn">Resized</b> {d.side} {d.symbol} {d.requested_quantity} → {d.approved_quantity}: {label(d.reason ?? "")}</>,
      });
    }
    return items.sort((a, b) => b.at.localeCompare(a.at)).slice(0, 14);
  }, [fills.data, decisions.data]);

  const p = portfolio.data;
  const limits = detail.data?.config.risk;
  const start = detail.data?.config.simulation.starting_cash;

  return (
    <>
      <div className="page-head">
        <div>
          <h1>Overview</h1>
          <p>{run ? `${run.strategies.join(", ")} on ${run.symbols.join(", ")}` : " "}</p>
        </div>
      </div>

      {p && start ? (
        <div className="figures">
          <Figure lead label="Equity" value={money(p.total_equity)} sub={<span className={toneOf(sub(p.total_equity, start))}>{money(sub(p.total_equity, start), true)} since start</span>} />
          <Figure label={live ? "Today" : "Last session"} value={<span className={toneOf(p.session_pnl)}>{money(p.session_pnl, true)}</span>} sub={percent(toNumber(p.session_pnl) / Math.max(1, toNumber(p.total_equity) - toNumber(p.session_pnl)))} />
          <Figure label="Cash" value={money(p.cash)} sub={`${money(p.reserved_cash)} reserved`} />
          <Figure label="Buying power" value={money(p.buying_power)} />
          <Figure
            label="Gross exposure"
            value={money(p.gross_exposure)}
            sub={limits ? `${percent(ratio(p.gross_exposure, limits.max_gross_exposure) ?? 0, 0, false)} of ${formatDecimal(limits.max_gross_exposure, { dp: 0, currency: true })} limit` : undefined}
          />
          <Figure label="Open orders" value={int(p.pending_orders.length)} sub={`${p.positions.filter((x) => x.quantity !== 0).length} open positions`} />
        </div>
      ) : (
        <div className="figures"><Loading height={58} /></div>
      )}

      {metrics.data && <Pipeline metrics={metrics.data} live={live} />}

      <div className="grid grid-main">
        <div className="stack">
          <Panel
            title="Equity"
            hint={range === "session" ? "Current session, one point per bar" : "Whole run"}
            tools={<Segmented label="Equity range" value={range} onChange={setRange} options={[{ value: "session", label: "Session" }, { value: "all", label: "All" }]} />}
            flush
          >
            {equity.data ? (
              equitySeries.length > 1
                ? <div style={{ padding: "8px 4px 4px 0" }}><TimeChart key={range} label="Equity over time" height={280} format={moneyAxis} series={[{ id: "equity", kind: "area", tone: "accent", data: equitySeries }]} /></div>
                : <Empty title="No equity history yet">The first bar has not arrived.</Empty>
            ) : <Loading height={280} />}
          </Panel>
          <Panel
            title="Price and fills"
            hint={symbol ? `${symbol}, one-minute bars` : undefined}
            tools={symbols.length > 1 ? <Segmented label="Symbol" value={symbol ?? ""} onChange={setSymbol} options={symbols.map((s) => ({ value: s, label: s }))} /> : undefined}
            flush
          >
            {bars.data ? (
              <div style={{ padding: "8px 4px 4px 0" }}>
                <TimeChart
                  key={symbol}
                  label={`${symbol} price with fills`}
                  height={300}
                  format={priceAxis}
                  initialBars={live ? 120 : 390}
                  series={[{ id: "price", kind: "candles", data: priceSeries }]}
                  markers={{ seriesId: "price", items: markers }}
                />
              </div>
            ) : <Loading height={300} />}
          </Panel>
        </div>
        <div className="stack">
          <Panel title="Positions" hint={p ? `as of ${minute(p.as_of)} ET` : undefined} flush>
            {p ? (
              <DataTable
                label="Positions"
                rows={p.positions}
                columns={compactColumns}
                rowKey={(r) => r.symbol}
                initialSort={{ key: "value", dir: "desc" }}
                empty={<Empty title="No positions">Positions appear after the first fill.</Empty>}
              />
            ) : <Loading height={160} />}
          </Panel>
          <Panel title="Recent activity" hint="Fills and risk interventions" className="fill-panel" flush>
            {activity.length ? (
              <ul className="feed">
                {activity.map((a) => (
                  <li key={a.key}>
                    <time dateTime={a.at}>{minute(a.at)}</time>
                    <p>
                      {a.orderId ? (
                        <button type="button" className="linkish" onClick={() => setOpenOrder(a.orderId)}>{a.node}</button>
                      ) : a.node}
                    </p>
                  </li>
                ))}
              </ul>
            ) : <Empty title="Nothing yet">Fills and risk decisions will show here.</Empty>}
          </Panel>
        </div>
      </div>

      {openOrder !== undefined && runId && <OrderDrawer runId={runId} orderId={openOrder} onClose={() => setOpenOrder(undefined)} />}
    </>
  );
}
