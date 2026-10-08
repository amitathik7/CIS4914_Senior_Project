import { useMemo } from "react";
import { Link, useParams } from "react-router";
import type { RunDetail, StrategyConfig, Trade } from "../api/contract";
import { BarChart } from "../components/charts/BarChart";
import { TimeChart } from "../components/charts/TimeChart";
import { DataTable, type Column } from "../components/DataTable";
import { IconDownload } from "../components/icons";
import { useCurrentRun } from "../components/Shell";
import { Empty, Figure, Loading, Notice, Panel, toneOf, toneOfNumber } from "../components/ui";
import { formatDecimal, money, price, toNumber } from "../lib/decimal";
import { date, dateTime, int, percent, period, ratioText, time } from "../lib/format";
import { useEngine, useRunResource } from "../state/engine";
import { drawdownLine, equityLine } from "./chartData";

const moneyAxis = (v: number) => `$${v.toLocaleString("en-US", { maximumFractionDigits: 0 })}`;
const pctAxis = (v: number) => `${v.toFixed(2)}%`;

const tradeColumns = (multiDay: boolean): Column<Trade>[] => [
  { key: "symbol", header: "Symbol", render: (t) => <b>{t.symbol}</b>, sort: (t) => t.symbol },
  { key: "strategy", header: "Strategy", render: (t) => <span className="muted">{t.strategy_id}</span>, sort: (t) => t.strategy_id },
  { key: "entry", header: "Opened (ET)", render: (t) => (multiDay ? `${date(t.entry_time)} ${time(t.entry_time)}` : time(t.entry_time)), sort: (t) => t.entry_time },
  { key: "exit", header: "Closed (ET)", render: (t) => (multiDay ? `${date(t.exit_time)} ${time(t.exit_time)}` : time(t.exit_time)), sort: (t) => t.exit_time },
  { key: "qty", header: "Size", align: "right", render: (t) => int(t.quantity), sort: (t) => t.quantity },
  { key: "in", header: "Entry", align: "right", render: (t) => price(t.entry_price) },
  { key: "out", header: "Exit", align: "right", render: (t) => price(t.exit_price) },
  { key: "fees", header: "Fees", align: "right", render: (t) => money(t.fees) },
  { key: "pnl", header: "P&L", align: "right", render: (t) => <span className={toneOf(t.pnl)}>{money(t.pnl, true)}</span>, sort: (t) => toNumber(t.pnl) },
];

function pnlBins(trades: Trade[]) {
  const values = trades.map((t) => toNumber(t.pnl));
  if (values.length === 0) return [];
  const lo = Math.min(...values);
  const hi = Math.max(...values);
  const bound = Math.max(Math.abs(lo), Math.abs(hi), 1);
  const step = (2 * bound) / 12;
  const bins = Array.from({ length: 12 }, (_, i) => ({ from: -bound + i * step, count: 0 }));
  for (const v of values) bins[Math.min(11, Math.floor((v + bound) / step))]!.count++;
  const fmt = (v: number) => `${v < 0 ? "−" : ""}$${Math.abs(v).toFixed(Math.abs(v) >= 100 ? 0 : 1)}`;
  return bins.map((b) => {
    const mid = b.from + step / 2;
    return {
      label: fmt(mid),
      value: b.count,
      tip: `${b.count} trades between ${fmt(b.from)} and ${fmt(b.from + step)}`,
      tone: (mid < 0 ? "down" : "up") as "down" | "up",
    };
  });
}

function strategyParams(s: StrategyConfig): [string, string][] {
  return s.kind === "sma_crossover"
    ? [["Short window", `${s.short_window} bars`], ["Long window", `${s.long_window} bars`]]
    : [["Lookback", `${s.lookback} bars`], ["Entry |z|", String(s.entry_threshold)], ["Rearm |z|", String(s.rearm_threshold)]];
}

function Configuration({ run }: { run: RunDetail }) {
  const c = run.config;
  const sim = c.simulation;
  return (
    <div className="grid grid-2" style={{ gap: 24 }}>
      <div className="stack">
        {c.strategies.map((s) => (
          <div key={s.strategy_id}>
            <h3 style={{ fontSize: "var(--fs-base)", fontWeight: 600, marginBottom: 8 }}>
              {s.strategy_id} <span className="faint" style={{ fontWeight: 400 }}>{s.kind === "sma_crossover" ? "Moving-average crossover" : "Mean reversion (z-score)"}</span>
            </h3>
            <dl className="kv">
              <dt>Symbols</dt><dd>{s.symbols.join(", ")}</dd>
              {strategyParams(s).map(([k, v]) => <FragmentKV key={k} k={k} v={v} />)}
              <dt>Order size</dt><dd>{int(s.requested_quantity)} shares</dd>
            </dl>
          </div>
        ))}
      </div>
      <dl className="kv" style={{ alignContent: "start" }}>
        <dt>Starting cash</dt><dd>{money(sim.starting_cash)}</dd>
        <dt>Slippage</dt><dd>{sim.slippage_bps} bps</dd>
        <dt>Fees</dt><dd>{formatDecimal(sim.fees.per_share, { dp: 4, currency: true })} per share{sim.fees.bps ? `, ${sim.fees.bps} bps` : ""}</dd>
        <dt>Latency</dt><dd>{sim.latency_ms.signal_to_order} ms to order, {sim.latency_ms.order_to_fill} ms to fill</dd>
        <dt>Participation</dt><dd>{percent(sim.participation_cap, 1, false)} of bar volume per fill</dd>
        <dt>Position limit</dt><dd>{money(c.risk.max_position_notional)}</dd>
        <dt>Gross limit</dt><dd>{money(c.risk.max_gross_exposure)}</dd>
        <dt>Daily loss stop</dt><dd>{money(c.risk.max_daily_loss)}</dd>
        <dt>Max order</dt><dd>{int(c.risk.max_order_quantity)} shares</dd>
        <dt>Run id</dt><dd className="id">{run.run_id}</dd>
      </dl>
    </div>
  );
}

function FragmentKV({ k, v }: { k: string; v: string }) {
  return (
    <>
      <dt>{k}</dt>
      <dd>{v}</dd>
    </>
  );
}

export function Report() {
  const { runId } = useParams();
  const run = useCurrentRun();
  const { client } = useEngine();
  const detail = useRunResource(runId, (c, id) => c.getRun(id));
  const equity = useRunResource(runId, (c, id) => c.equity(id));
  const trades = useRunResource(runId, (c, id) => c.trades(id));
  const r = detail.data?.report;
  const multiDay = run?.mode === "backtest";

  const equitySeries = useMemo(() => equityLine(equity.data ?? []), [equity.data]);
  const drawdownSeries = useMemo(() => drawdownLine(equity.data ?? []), [equity.data]);
  const bins = useMemo(() => pnlBins(trades.data ?? []), [trades.data]);

  const download = async () => {
    if (!runId || !detail.data) return;
    const [eq, tr, fills] = await Promise.all([client.equity(runId), client.trades(runId), client.fills(runId)]);
    const doc = { schema: "trading_engine.run_report", schema_version: "1.0", run: detail.data, equity: eq, trades: tr, fills };
    const url = URL.createObjectURL(new Blob([JSON.stringify(doc, null, 2)], { type: "application/json" }));
    const a = document.createElement("a");
    a.href = url;
    a.download = `run-${runId}.json`;
    a.click();
    URL.revokeObjectURL(url);
  };

  if (detail.data && (detail.data.status === "failed" || detail.data.status === "queued" || (detail.data.status === "running" && detail.data.mode === "backtest"))) {
    const d = detail.data;
    return (
      <>
        <div className="page-head"><div><h1>{d.name}</h1><p>{period(d.period_start, d.period_end)}</p></div></div>
        {d.status === "failed" ? (
          <Notice tone="down"><b>This run failed.</b> {d.error} <Link to="/backtests/new">Start a new backtest</Link></Notice>
        ) : (
          <Panel title="Replaying">
            <div className="stack" style={{ gap: 10 }}>
              <div className="progress-line" style={{ width: "100%", height: 6 }}><span style={{ width: `${Math.round((d.progress ?? 0) * 100)}%` }} /></div>
              <p className="muted num">{Math.round((d.progress ?? 0) * 100)}% of bars replayed{d.clock ? `, now at ${dateTime(d.clock)} ET` : ""}. The report appears when the replay completes.</p>
            </div>
          </Panel>
        )}
        <div style={{ marginTop: 14 }}><Panel title="Configuration"><Configuration run={d} /></Panel></div>
      </>
    );
  }

  return (
    <>
      <div className="page-head">
        <div>
          <h1>Report</h1>
          <p>{run ? `${run.name}, ${period(run.period_start, run.period_end ?? run.clock)}` : " "}</p>
        </div>
        <div className="page-actions">
          <button type="button" className="btn" onClick={download} disabled={!detail.data}>
            <IconDownload />
            Export JSON
          </button>
        </div>
      </div>

      {run?.mode === "live" && run.status === "running" && (
        <div style={{ marginBottom: 14 }}>
          <Notice>Figures to date. The run is still live, so they change with every bar.</Notice>
        </div>
      )}

      {r ? (
        <div className="figures">
          <Figure lead label="Total return" value={<span className={toneOfNumber(r.total_return)}>{percent(r.total_return)}</span>} sub={`ending ${money(r.ending_equity)}`} />
          <Figure label="Max drawdown" value={percent(r.max_drawdown)} sub="peak to trough" />
          <Figure label="Volatility" value={percent(r.volatility, 1, false)} sub="annualised" />
          <Figure label="Sharpe" value={ratioText(r.sharpe)} sub={r.sharpe !== undefined ? "from daily returns" : "needs 3+ sessions"} />
          <Figure
            label="Profit factor"
            value={r.profit_factor !== undefined ? ratioText(r.profit_factor) : "—"}
            sub={r.profit_factor_status === "no_losing_trades" ? "no losing trades" : r.profit_factor_status === "no_trades" ? "no closed trades" : "gross win / gross loss"}
          />
          <Figure label="Trades" value={int(r.trade_count)} sub={r.win_rate !== undefined ? `${percent(r.win_rate, 0, false)} winners` : "closed round trips"} />
          <Figure label="Fees" value={money(r.fees_paid)} />
        </div>
      ) : (
        <div className="figures"><Loading height={58} /></div>
      )}

      <div className="grid grid-2">
        <Panel title="Equity" flush>
          {equitySeries.length > 1 ? (
            <div style={{ padding: "8px 4px 4px 0" }}>
              <TimeChart label="Equity over the run" height={260} format={moneyAxis} series={[{ id: "equity", kind: "line", tone: "accent", data: equitySeries }]} />
            </div>
          ) : equity.data ? <Empty title="No equity history" /> : <Loading height={260} />}
        </Panel>
        <Panel title="Drawdown" hint="Below the running peak" flush>
          {drawdownSeries.length > 1 ? (
            <div style={{ padding: "8px 4px 4px 0" }}>
              <TimeChart label="Drawdown over the run" height={260} format={pctAxis} series={[{ id: "dd", kind: "area", tone: "down", data: drawdownSeries }]} />
            </div>
          ) : equity.data ? <Empty title="No equity history" /> : <Loading height={260} />}
        </Panel>

        <Panel title="Trade P&L" hint="Closed round trips, after fees" flush>
          {trades.data ? (
            bins.length ? (
              <div style={{ padding: "12px 12px 4px" }}>
                <BarChart label="Distribution of trade P&L" data={bins} height={240} format={(v) => int(Math.round(v))} xLabel="P&L per trade" />
              </div>
            ) : <Empty title="No closed trades">A trade closes when a position returns to flat.</Empty>
          ) : <Loading height={240} />}
        </Panel>

        <Panel title="Configuration">
          {detail.data ? <Configuration run={detail.data} /> : <Loading height={240} />}
        </Panel>

        <Panel title="Trades" className="span-all" hint={trades.data ? `${int(trades.data.length)} closed` : undefined} flush>
          {trades.data ? (
            <DataTable label="Closed trades" rows={trades.data} columns={tradeColumns(multiDay)} rowKey={(t) => `${t.symbol}-${t.entry_time}`} initialSort={{ key: "exit", dir: "desc" }} maxHeight={480} limit={500} empty={<Empty title="No closed trades" />} />
          ) : <Loading height={240} />}
        </Panel>
      </div>
    </>
  );
}
