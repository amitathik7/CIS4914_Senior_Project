import { useMemo, useState } from "react";
import { useParams } from "react-router";
import type { RiskDecision, RiskOutcome } from "../api/contract";
import { BarChart } from "../components/charts/BarChart";
import { DataTable, type Column } from "../components/DataTable";
import { IconPower } from "../components/icons";
import { useCurrentRun } from "../components/Shell";
import { Empty, Figure, Loading, Meter, Notice, OutcomeBadge, Panel, Segmented, Side } from "../components/ui";
import { formatDecimal, fromMicros, money, ratio, toMicros } from "../lib/decimal";
import { date, dateTime, int, label, percent, time } from "../lib/format";
import { useEngine, useRunResource } from "../state/engine";
import { OrderDrawer } from "./OrderDrawer";

const whole = (text: string) => formatDecimal(text, { dp: 0, currency: true });

function LimitRow({ name, used, limit, value, detail }: { name: string; used: number | null; limit: string; value: string; detail?: string }) {
  return (
    <div style={{ display: "grid", gap: 6 }}>
      <div className="row" style={{ justifyContent: "space-between" }}>
        <span>{name}</span>
        <span className="num muted">
          <b style={{ color: "var(--text)", fontWeight: 500 }}>{value}</b> of {limit}
        </span>
      </div>
      <Meter value={used} label={`${name} usage`} />
      {detail && <small className="faint">{detail}</small>}
    </div>
  );
}

export function Risk() {
  const { runId } = useParams();
  const run = useCurrentRun();
  const { client, refreshRuns } = useEngine();
  const detail = useRunResource(runId, (c, id) => c.getRun(id));
  const portfolio = useRunResource(runId, (c, id) => c.portfolio(id));
  const decisions = useRunResource(runId, (c, id) => c.riskDecisions(id));
  const [filter, setFilter] = useState<"all" | RiskOutcome>("all");
  const [reason, setReason] = useState("");
  const [busy, setBusy] = useState(false);
  const [openOrder, setOpenOrder] = useState<number>();
  const multiDay = run?.mode === "backtest";

  const all = decisions.data ?? [];
  const counts = useMemo(() => {
    const c = { approved: 0, resized: 0, rejected: 0 };
    for (const d of all) c[d.outcome]++;
    return c;
  }, [all]);

  const byPolicy = useMemo(() => {
    const map = new Map<string, { rejected: number; resized: number }>();
    for (const d of all) {
      if (d.outcome === "approved" || !d.reason) continue;
      const m = map.get(d.reason) ?? { rejected: 0, resized: 0 };
      m[d.outcome]++;
      map.set(d.reason, m);
    }
    return [...map.entries()]
      .map(([policy, m]) => ({ label: label(policy), value: m.rejected + m.resized, tip: `${m.rejected} rejected, ${m.resized} resized` }))
      .sort((a, b) => b.value - a.value);
  }, [all]);

  const rows = filter === "all" ? all : all.filter((d) => d.outcome === filter);

  const columns: Column<RiskDecision>[] = [
    { key: "time", header: "Decided (ET)", render: (d) => (multiDay ? `${date(d.decided_at)} ${time(d.decided_at)}` : time(d.decided_at)), sort: (d) => d.decided_at },
    { key: "signal", header: "Signal", render: (d) => <span className="id">#{d.signal_id}</span>, sort: (d) => d.signal_id },
    { key: "strategy", header: "Strategy", render: (d) => <span className="muted">{d.strategy_id}</span> },
    { key: "symbol", header: "Symbol", render: (d) => <b>{d.symbol}</b>, sort: (d) => d.symbol },
    { key: "side", header: "Side", render: (d) => <Side side={d.side} /> },
    { key: "req", header: "Requested", align: "right", render: (d) => int(d.requested_quantity) },
    { key: "ok", header: "Approved", align: "right", render: (d) => (d.approved_quantity ? int(d.approved_quantity) : <span className="faint">0</span>) },
    { key: "outcome", header: "Outcome", render: (d) => <OutcomeBadge outcome={d.outcome} />, sort: (d) => d.outcome },
    { key: "reason", header: "Policy", render: (d) => <span className="muted">{d.reason ? label(d.reason) : ""}</span>, sort: (d) => d.reason ?? "" },
  ];

  const limits = detail.data?.config.risk;
  const p = portfolio.data;
  const kill = detail.data?.kill_switch;
  const canToggle = run?.status === "running";

  const toggleKill = async () => {
    if (!runId || !kill) return;
    setBusy(true);
    try {
      await client.setKillSwitch(runId, !kill.engaged, kill.engaged ? undefined : reason.trim() || "Engaged from the console");
      setReason("");
      refreshRuns();
    } finally {
      setBusy(false);
    }
  };

  const largest = p?.positions.reduce((m, x) => {
    const v = toMicros(x.market_value);
    const abs = v < 0n ? -v : v;
    return abs > m.value ? { symbol: x.symbol, value: abs } : m;
  }, { symbol: "", value: 0n });

  const total = all.length;

  return (
    <>
      <div className="page-head">
        <div>
          <h1>Risk</h1>
          <p>Each signal is checked against current portfolio state before it can become a working order.</p>
        </div>
      </div>

      {kill?.engaged && (
        <div style={{ marginBottom: 14 }}>
          <Notice tone="down">
            <b>Kill switch engaged</b>{kill.changed_at ? ` at ${time(kill.changed_at)} ET` : ""}. Every new signal is rejected until it is released.
            {kill.reason ? ` Reason: ${kill.reason}` : ""}
          </Notice>
        </div>
      )}

      <div className="figures">
        <Figure lead label="Decisions" value={int(total)} sub={run?.mode === "live" ? "this run" : "over the replay"} />
        <Figure label="Approved" value={int(counts.approved)} sub={total ? percent(counts.approved / total, 0, false) : "—"} />
        <Figure label="Resized" value={int(counts.resized)} sub={total ? percent(counts.resized / total, 0, false) : "—"} />
        <Figure label="Rejected" value={int(counts.rejected)} sub={total ? percent(counts.rejected / total, 0, false) : "—"} />
      </div>

      <div className="grid grid-main">
        <Panel title="Limits" hint="Current usage against the configured limits">
          {limits && p ? (
            <div className="stack">
              <LimitRow name="Gross exposure" used={ratio(p.gross_exposure, limits.max_gross_exposure)} value={whole(p.gross_exposure)} limit={whole(limits.max_gross_exposure)} />
              <LimitRow
                name="Largest position"
                used={largest && largest.value > 0n ? ratio(fromMicros(largest.value), limits.max_position_notional) : 0}
                value={largest && largest.value > 0n ? whole(fromMicros(largest.value)) : "$0"}
                limit={whole(limits.max_position_notional)}
                detail={largest?.symbol ? `${largest.symbol}; the limit applies to each symbol` : "The limit applies to each symbol"}
              />
              <LimitRow
                name="Session loss"
                used={toMicros(p.session_pnl) < 0n ? ratio(p.session_pnl.replace("-", ""), limits.max_daily_loss) : 0}
                value={toMicros(p.session_pnl) < 0n ? whole(p.session_pnl.replace("-", "")) : "$0"}
                limit={whole(limits.max_daily_loss)}
                detail="At the limit, only orders that reduce a position pass"
              />
              <LimitRow name="Open orders" used={p.pending_orders.length / limits.max_open_orders} value={int(p.pending_orders.length)} limit={int(limits.max_open_orders)} />
              <dl className="kv" style={{ marginTop: 4 }}>
                <dt>Max order size</dt><dd>{int(limits.max_order_quantity)} shares</dd>
                <dt>Short selling</dt><dd>{limits.allow_short ? "Allowed" : "Disabled; sells are capped at the long held"}</dd>
                <dt>Buying power</dt><dd>{money(p.buying_power)} after {money(p.reserved_cash)} reserved for working buys</dd>
              </dl>
            </div>
          ) : <Loading height={220} />}
        </Panel>

        <div className="stack">
          <Panel title="Kill switch">
            {kill ? (
              <div className="stack" style={{ gap: 12 }}>
                <p className="muted">
                  {kill.engaged
                    ? "New signals are being rejected. Working orders keep filling."
                    : "Off. Engaging it rejects every new signal at the risk stage; working orders keep filling."}
                </p>
                {!kill.engaged && canToggle && (
                  <label className="field">
                    <span>Reason (optional)</span>
                    <input className="input" value={reason} onChange={(e) => setReason(e.target.value)} placeholder="Recorded with the change" maxLength={120} />
                  </label>
                )}
                <button type="button" className={kill.engaged ? "btn" : "btn btn-danger"} disabled={!canToggle || busy} onClick={toggleKill}>
                  <IconPower />
                  {kill.engaged ? "Release kill switch" : "Engage kill switch"}
                </button>
                {!canToggle && <small className="faint">Available while a run is running.</small>}
                {kill.changed_at && <small className="faint">Last changed {dateTime(kill.changed_at)} ET</small>}
              </div>
            ) : <Loading height={120} />}
          </Panel>

          <Panel title="Interventions by policy" hint="Resized or rejected">
            {decisions.data ? (
              byPolicy.length ? (
                <BarChart label="Risk interventions by policy" data={byPolicy} height={200} format={(v) => int(Math.round(v))} tone="warn" />
              ) : <Empty title="No interventions">Every signal so far passed unchanged.</Empty>
            ) : <Loading height={200} />}
          </Panel>
        </div>

        <Panel
          title="Decision log"
          className="span-all"
          tools={<Segmented label="Outcome" value={filter} onChange={setFilter} options={[
            { value: "all", label: "All" },
            { value: "approved", label: "Approved" },
            { value: "resized", label: "Resized" },
            { value: "rejected", label: "Rejected" },
          ]} />}
          flush
        >
          {decisions.data ? (
            <DataTable
              label="Risk decisions"
              rows={rows}
              columns={columns}
              rowKey={(d) => d.id}
              initialSort={{ key: "time", dir: "desc" }}
              onRowClick={(d) => d.order_id !== undefined && setOpenOrder(d.order_id)}
              maxHeight={520}
              limit={400}
              empty={<Empty title="No decisions">Decisions appear when a strategy emits a signal.</Empty>}
            />
          ) : <Loading height={300} />}
        </Panel>
      </div>

      {openOrder !== undefined && runId && <OrderDrawer runId={runId} orderId={openOrder} onClose={() => setOpenOrder(undefined)} />}
    </>
  );
}
