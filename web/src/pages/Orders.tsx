import { useMemo, useState } from "react";
import { useParams } from "react-router";
import type { Fill, Order, OrderStatus, TradeSignal } from "../api/contract";
import { DataTable, type Column } from "../components/DataTable";
import { Empty, Loading, OrderStatusBadge, Panel, Segmented, Side } from "../components/ui";
import { money, mul, price, toNumber } from "../lib/decimal";
import { date, int, label, time } from "../lib/format";
import { useRunResource } from "../state/engine";
import { useCurrentRun } from "../components/Shell";
import { OrderDrawer } from "./OrderDrawer";

type View = "orders" | "fills" | "signals";
type StatusFilter = "all" | "open" | "filled" | "rejected";

const OPEN: OrderStatus[] = ["new", "pending_risk", "approved", "working", "partially_filled"];

const when = (iso: string, multiDay: boolean) => (multiDay ? `${date(iso)} ${time(iso)}` : time(iso));

export function Orders() {
  const { runId } = useParams();
  const run = useCurrentRun();
  const multiDay = run?.mode === "backtest";
  const orders = useRunResource(runId, (c, id) => c.orders(id));
  const fills = useRunResource(runId, (c, id) => c.fills(id));
  const signals = useRunResource(runId, (c, id) => c.signals(id));
  const [view, setView] = useState<View>("orders");
  const [status, setStatus] = useState<StatusFilter>("all");
  const [symbol, setSymbol] = useState("all");
  const [openOrder, setOpenOrder] = useState<number>();

  const symbols = run?.symbols ?? [];
  const bySymbol = <T extends { symbol: string }>(rows: T[]) => (symbol === "all" ? rows : rows.filter((r) => r.symbol === symbol));

  const symbolOrders = useMemo(
    () => (orders.data ?? []).filter((o) => symbol === "all" || o.symbol === symbol),
    [orders.data, symbol],
  );
  const counts = {
    all: symbolOrders.length,
    open: symbolOrders.filter((o) => OPEN.includes(o.status)).length,
    filled: symbolOrders.filter((o) => o.status === "filled").length,
    rejected: symbolOrders.filter((o) => o.status === "rejected").length,
  };
  const orderRows =
    status === "open" ? symbolOrders.filter((o) => OPEN.includes(o.status))
    : status === "all" ? symbolOrders
    : symbolOrders.filter((o) => o.status === status);

  const orderColumns: Column<Order>[] = [
    { key: "id", header: "Order", render: (o) => <span className="id">#{o.id}</span>, sort: (o) => o.id },
    { key: "time", header: "Created (ET)", render: (o) => when(o.created_at, multiDay), sort: (o) => o.created_at },
    { key: "symbol", header: "Symbol", render: (o) => <b>{o.symbol}</b>, sort: (o) => o.symbol },
    { key: "side", header: "Side", render: (o) => <Side side={o.side} />, sort: (o) => o.side },
    { key: "qty", header: "Quantity", align: "right", render: (o) => int(o.quantity), sort: (o) => o.quantity },
    { key: "filled", header: "Filled", align: "right", render: (o) => (o.filled_quantity ? int(o.filled_quantity) : <span className="faint">0</span>), sort: (o) => o.filled_quantity },
    { key: "avg", header: "Avg price", align: "right", render: (o) => (o.average_fill_price ? price(o.average_fill_price) : <span className="faint">—</span>), sort: (o) => (o.average_fill_price ? toNumber(o.average_fill_price) : 0) },
    { key: "status", header: "Status", render: (o) => <OrderStatusBadge status={o.status} />, sort: (o) => o.status },
    { key: "strategy", header: "Strategy", render: (o) => <span className="muted">{o.strategy_id}</span>, sort: (o) => o.strategy_id },
    { key: "reason", header: "Note", render: (o) => <span className="muted">{o.reject_reason ? label(o.reject_reason) : o.history.find((h) => h.note?.startsWith("resized"))?.note ?? ""}</span> },
  ];

  const fillColumns: Column<Fill>[] = [
    { key: "id", header: "Fill", render: (f) => <span className="id">#{f.id}</span>, sort: (f) => f.id },
    { key: "time", header: "Time (ET)", render: (f) => when(f.filled_at, multiDay), sort: (f) => f.filled_at },
    { key: "order", header: "Order", render: (f) => <span className="id">#{f.order_id}</span>, sort: (f) => f.order_id },
    { key: "symbol", header: "Symbol", render: (f) => <b>{f.symbol}</b>, sort: (f) => f.symbol },
    { key: "side", header: "Side", render: (f) => <Side side={f.side} /> },
    { key: "qty", header: "Quantity", align: "right", render: (f) => int(f.filled_quantity), sort: (f) => f.filled_quantity },
    { key: "price", header: "Price", align: "right", render: (f) => price(f.fill_price), sort: (f) => toNumber(f.fill_price) },
    { key: "notional", header: "Notional", align: "right", render: (f) => money(mul(f.fill_price, f.filled_quantity)), sort: (f) => toNumber(f.fill_price) * f.filled_quantity },
    { key: "slip", header: "Slippage", align: "right", render: (f) => price(f.slippage) },
    { key: "fees", header: "Fees", align: "right", render: (f) => money(f.fees), sort: (f) => toNumber(f.fees) },
    { key: "status", header: "Status", render: (f) => (f.status === "filled" ? "Completes order" : <span style={{ color: "var(--warn)" }}>Partial</span>) },
  ];

  const signalColumns: Column<TradeSignal>[] = [
    { key: "id", header: "Signal", render: (s) => <span className="id">#{s.id}</span>, sort: (s) => s.id },
    { key: "time", header: "Bar (ET)", render: (s) => when(s.created_at, multiDay), sort: (s) => s.created_at },
    { key: "strategy", header: "Strategy", render: (s) => s.strategy_id, sort: (s) => s.strategy_id },
    { key: "symbol", header: "Symbol", render: (s) => <b>{s.symbol}</b>, sort: (s) => s.symbol },
    { key: "side", header: "Side", render: (s) => <Side side={s.side} /> },
    { key: "qty", header: "Requested", align: "right", render: (s) => int(s.requested_quantity ?? 0) },
    { key: "trigger", header: "Trigger", render: (s) => <span className="muted">{label(s.metadata.trigger ?? "")}</span> },
    {
      key: "detail", header: "Indicators", render: (s) => (
        <span className="muted num">
          {s.metadata.short_sma ? `short ${Number(s.metadata.short_sma).toFixed(2)}, long ${Number(s.metadata.long_sma).toFixed(2)}` : `z ${Number(s.metadata.z_score).toFixed(2)}, mean ${Number(s.metadata.mean).toFixed(2)}`}
        </span>
      ),
    },
  ];

  const signalOrder = useMemo(() => new Map((orders.data ?? []).map((o) => [o.origin_signal, o.id])), [orders.data]);

  return (
    <>
      <div className="page-head">
        <div>
          <h1>Orders</h1>
          <p>Every signal becomes an order that passes through risk before execution. Select a row to trace it.</p>
        </div>
        <div className="page-actions">
          <label className="row">
            <span className="visually-hidden">Symbol</span>
            <select className="select" value={symbol} onChange={(e) => setSymbol(e.target.value)} style={{ width: 130 }}>
              <option value="all">All symbols</option>
              {symbols.map((s) => <option key={s} value={s}>{s}</option>)}
            </select>
          </label>
          <Segmented label="View" value={view} onChange={setView} options={[
            { value: "orders", label: `Orders ${orders.data ? int(counts.all) : ""}` },
            { value: "fills", label: `Fills ${fills.data ? int(bySymbol(fills.data).length) : ""}` },
            { value: "signals", label: `Signals ${signals.data ? int(bySymbol(signals.data).length) : ""}` },
          ]} />
        </div>
      </div>

      {view === "orders" && (
        <Panel
          title="Blotter"
          tools={<Segmented label="Status" value={status} onChange={setStatus} options={[
            { value: "all", label: "All" },
            { value: "open", label: `Open ${counts.open}` },
            { value: "filled", label: `Filled ${counts.filled}` },
            { value: "rejected", label: `Rejected ${counts.rejected}` },
          ]} />}
          flush
        >
          {orders.data ? (
            <DataTable
              label="Orders"
              rows={orderRows}
              columns={orderColumns}
              rowKey={(o) => o.id}
              initialSort={{ key: "id", dir: "desc" }}
              onRowClick={(o) => setOpenOrder(o.id)}
              selectedKey={openOrder}
              maxHeight={640}
              limit={500}
              empty={<Empty title="No orders match">Orders appear once a strategy emits a signal.</Empty>}
            />
          ) : <Loading height={400} />}
        </Panel>
      )}

      {view === "fills" && (
        <Panel title="Fills" hint="Simulated against one-minute bars, capped by participation" flush>
          {fills.data ? (
            <DataTable label="Fills" rows={bySymbol(fills.data)} columns={fillColumns} rowKey={(f) => f.id} initialSort={{ key: "id", dir: "desc" }} onRowClick={(f) => setOpenOrder(f.order_id)} maxHeight={640} limit={500} empty={<Empty title="No fills yet" />} />
          ) : <Loading height={400} />}
        </Panel>
      )}

      {view === "signals" && (
        <Panel title="Signals" hint="As emitted by the strategies, before risk" flush>
          {signals.data ? (
            <DataTable label="Signals" rows={bySymbol(signals.data)} columns={signalColumns} rowKey={(s) => s.id} initialSort={{ key: "id", dir: "desc" }} onRowClick={(s) => { const id = signalOrder.get(s.id); if (id !== undefined) setOpenOrder(id); }} maxHeight={640} limit={500} empty={<Empty title="No signals yet">Strategies are still warming up.</Empty>} />
          ) : <Loading height={400} />}
        </Panel>
      )}

      {openOrder !== undefined && runId && <OrderDrawer runId={runId} orderId={openOrder} onClose={() => setOpenOrder(undefined)} />}
    </>
  );
}
