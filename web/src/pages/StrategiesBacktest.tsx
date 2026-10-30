import { useMemo } from "react";
import { TimeChart, chartTime, type MarkerSpec } from "../components/charts/TimeChart";
import { DataTable, type Column } from "../components/DataTable";
import { IconPlay } from "../components/icons";
import { ConfigPanel } from "../components/strategies/ConfigPanel";
import { Badge, Empty, Figure, Notice, Panel, toneOf } from "../components/ui";
import { money, price, sub, toNumber } from "../lib/decimal";
import { dateTime, int, percent } from "../lib/format";
import { SETTING_FIELDS, settingIssues, type BacktestDecision, type BacktestDoc, type BacktestFill, type BacktestPosition } from "../strategies/backtest";
import { backtestBlocker, backtestRelation, type BacktestState } from "../strategies/backtestStore";
import { datasetIdentity, type LabState } from "../strategies/store";
import { backtestStore, labStore, useBacktest, useLab } from "../strategies/useLab";

// Backtest: the configuration from Configure/Explore, run end to end through the real C++ engine (strategy -> risk ->
// simulated execution -> portfolio -> analytics) over the SAME dataset, via strategy_lab_backtest. Unlike the Backtests page,
// which runs the in-browser demo simulator over synthetic data, every number here comes from the engine.

const moneyAxis = (v: number) => `$${v.toLocaleString("en-US", { maximumFractionDigits: 0 })}`;
const pctAxis = (v: number) => `${v.toFixed(2)}%`;

function Settings({ lab, bt }: { lab: LabState; bt: BacktestState }) {
  const issues = settingIssues(bt.settings);
  const blocker = backtestBlocker(lab, bt);
  const running = bt.run.phase === "running";
  const group = (g: "account" | "costs" | "risk") =>
    SETTING_FIELDS.filter((f) => f.group === g).map((f) => {
      const issue = issues.find((i) => i.name === f.name)?.message;
      return (
        <label key={f.name} className="field" data-invalid={issue ? "true" : undefined}>
          <span>{f.label}</span>
          <input className="input" value={bt.settings[f.name] ?? ""} placeholder={f.placeholder} inputMode="decimal" autoComplete="off" spellCheck={false}
            aria-invalid={issue ? true : undefined} aria-label={f.label} disabled={g === "risk" && !bt.settings.risk_policies}
            onChange={(e) => backtestStore.setSetting(f.name, e.target.value)} />
          <small>{issue ?? f.help}</small>
        </label>
      );
    });
  return (
    <>
      <section className="lab-section">
        <h3 className="lab-section-title">Account and costs</h3>
        <p className="faint lab-help">Empty fields use the engine's defaults (shown greyed). Orders fill at the next bar's open, moved by slippage.</p>
        <div className="lab-fields">{group("account")}{group("costs")}</div>
      </section>
      <section className="lab-section">
        <h3 className="lab-section-title">Risk limits</h3>
        <label className="row" style={{ gap: 8 }}>
          <input type="checkbox" checked={bt.settings.risk_policies} onChange={(e) => backtestStore.setRiskPolicies(e.target.checked)} />
          <span>Apply the risk policies</span>
        </label>
        <p className="faint lab-help">Buying power and the shares held are always checked: the account is long only, so a Sell with nothing to sell is rejected.</p>
        <div className="lab-fields">{group("risk")}</div>
      </section>
      <section className="lab-section">
        <h3 className="lab-section-title">Actions</h3>
        <div className="stack" style={{ gap: 6 }}>
          <div className="row" style={{ flexWrap: "wrap" }}>
            <button type="button" className="btn btn-primary" disabled={!!blocker} onClick={() => void backtestStore.run(labStore.getState())} aria-describedby="bt-run-help">
              <IconPlay size={13} /> {running ? "Running…" : "Run backtest"}
            </button>
            {running && <button type="button" className="btn" onClick={backtestStore.cancel}>Cancel</button>}
            <button type="button" className="btn btn-ghost" onClick={backtestStore.resetSettings}>Default settings</button>
          </div>
          <p id="bt-run-help" className="lab-help" data-tone={blocker && !running ? "warn" : undefined}>
            {running ? "Backtesting on the real engine." : blocker ?? `Runs the strategy through risk, simulated execution and the portfolio over ${datasetIdentity(lab)?.name ?? "the dataset"}. Simulated only: no real orders.`}
          </p>
        </div>
      </section>
    </>
  );
}

const pf = (r: BacktestDoc["report"]) =>
  r.profitFactor !== undefined ? r.profitFactor.toFixed(2) : r.profitFactorStatus === "no_losing_trades" ? "No losses" : "—";

function Figures({ doc }: { doc: BacktestDoc }) {
  const r = doc.report;
  const pnl = sub(r.endingEquity, r.startingEquity);
  return (
    <div className="figures">
      <Figure lead label="Total return" value={<span className={r.totalReturn > 0 ? "up" : r.totalReturn < 0 ? "down" : undefined}>{percent(r.totalReturn)}</span>} sub={<span className={toneOf(pnl)}>{money(pnl, true)}</span>} />
      <Figure label="Ending equity" value={money(r.endingEquity)} sub={`from ${money(r.startingEquity)}`} />
      <Figure label="Max drawdown" value={<span className={r.maxDrawdown < 0 ? "down" : undefined}>{percent(r.maxDrawdown)}</span>} />
      <Figure label="Trades" value={int(r.tradeCount)} sub={r.winRate !== undefined ? `${percent(r.winRate, 0, false)} won` : "none closed"} />
      <Figure label="Profit factor" value={pf(r)} />
      <Figure label="Sharpe" value={r.sharpe !== undefined ? r.sharpe.toFixed(2) : "—"} sub={`annualised, ${int(doc.config.periodsPerYear)}/yr`} />
      <Figure label="Fees" value={money(r.feesPaid)} sub={`${int(r.fillCount)} fills`} />
    </div>
  );
}

const fillColumns: Column<BacktestFill>[] = [
  { key: "time", header: "Filled", render: (f) => dateTime(f.time), sort: (f) => f.time },
  { key: "symbol", header: "Symbol", render: (f) => <b>{f.symbol}</b>, sort: (f) => f.symbol },
  { key: "side", header: "Side", render: (f) => <Badge tone={f.side === "buy" ? "up" : "down"}>{f.side === "buy" ? "Buy" : "Sell"}</Badge>, sort: (f) => f.side },
  { key: "qty", header: "Shares", align: "right", render: (f) => f.quantity, sort: (f) => Number(f.quantity) },
  { key: "price", header: "Price", align: "right", render: (f) => price(f.price), sort: (f) => toNumber(f.price) },
  { key: "slip", header: "Slippage", align: "right", render: (f) => price(f.slippage) },
  { key: "fees", header: "Fees", align: "right", render: (f) => money(f.fees) },
  { key: "signal", header: "Signal", align: "right", render: (f) => (f.signalId ? `#${f.signalId}` : "—") },
];

const decisionColumns: Column<BacktestDecision>[] = [
  { key: "signal", header: "Signal", align: "right", render: (d) => `#${d.signalId}`, sort: (d) => d.signalId },
  { key: "time", header: "Decided", render: (d) => dateTime(d.time), sort: (d) => d.time },
  { key: "side", header: "Request", render: (d) => `${d.side === "sell" ? "Sell" : "Buy"} ${d.requested} ${d.symbol}` },
  { key: "outcome", header: "Risk", render: (d) => <Badge tone={d.approved ? (d.adjusted ? "warn" : "up") : "down"}>{d.approved ? (d.adjusted ? `Resized to ${d.adjusted}` : "Approved") : "Rejected"}</Badge>, sort: (d) => (d.approved ? 1 : 0) },
  { key: "reason", header: "Reason", render: (d) => <span className="wrap">{d.approved && !d.adjusted ? "—" : d.reason}</span> },
];

const positionColumns: Column<BacktestPosition>[] = [
  { key: "symbol", header: "Symbol", render: (p) => <b>{p.symbol}</b> },
  { key: "qty", header: "Shares", align: "right", render: (p) => p.quantity },
  { key: "cost", header: "Average cost", align: "right", render: (p) => price(p.averageCost) },
  { key: "realized", header: "Realised", align: "right", render: (p) => <span className={toneOf(p.realizedPnl)}>{money(p.realizedPnl, true)}</span> },
  { key: "unrealized", header: "Unrealised", align: "right", render: (p) => <span className={toneOf(p.unrealizedPnl)}>{money(p.unrealizedPnl, true)}</span> },
];

function Result({ lab, bt }: { lab: LabState; bt: BacktestState }) {
  const result = bt.result!;
  const doc = result.doc;
  const relation = backtestRelation(lab, bt);
  const equity = useMemo(() => doc.equity.map((p) => ({ time: chartTime(p.time), value: toNumber(p.equity) })), [doc]);
  const drawdown = useMemo(() => doc.equity.map((p) => ({ time: chartTime(p.time), value: p.drawdown * 100 })), [doc]);
  const markers: MarkerSpec[] = useMemo(() => doc.fills.map((f) => ({ time: chartTime(f.time), side: f.side, text: `${f.side === "buy" ? "B" : "S"} ${f.quantity}` })), [doc]);
  const c = doc.counts;
  return (
    <>
      <div className="row" style={{ justifyContent: "space-between", flexWrap: "wrap", marginBottom: 10 }}>
        <div>
          <b>{result.title}</b> <span className="muted">on {doc.dataset.name} · {doc.dataset.symbols.join(", ")}</span>
        </div>
        <Badge tone={relation === "current" ? "accent" : "warn"}>{relation === "current" ? "Matches the settings on screen" : "Settings changed since this backtest"}</Badge>
      </div>
      {c.openOrdersAtEnd > 0 && (
        <div style={{ marginBottom: 10 }}><Notice tone="warn">{int(c.openOrdersAtEnd)} order{c.openOrdersAtEnd === 1 ? " was" : "s were"} still working when the data ended and never filled.</Notice></div>
      )}
      <Figures doc={doc} />
      <div className="grid grid-2">
        <Panel title="Equity" hint="After each bar, fills marked" flush>
          {equity.length > 1 ? (
            <div style={{ padding: "8px 4px 4px 0" }}>
              <TimeChart label="Backtest equity" height={260} format={moneyAxis} series={[{ id: "equity", kind: "line", tone: "accent", data: equity }]} markers={{ seriesId: "equity", items: markers }} />
            </div>
          ) : <Empty title="Not enough bars for a chart" />}
        </Panel>
        <Panel title="Drawdown" hint="Below the running peak" flush>
          {drawdown.length > 1 ? (
            <div style={{ padding: "8px 4px 4px 0" }}>
              <TimeChart label="Backtest drawdown" height={260} format={pctAxis} series={[{ id: "dd", kind: "area", tone: "down", data: drawdown }]} />
            </div>
          ) : <Empty title="Not enough bars for a chart" />}
        </Panel>
        <Panel title="Pipeline" hint="What each stage did">
          <dl className="kv">
            <dt>Signals from the strategy</dt><dd>{int(c.signals)}</dd>
            <dt>Approved by risk</dt><dd>{int(c.approved)}{c.resized ? ` (${int(c.resized)} resized)` : ""}</dd>
            <dt>Rejected by risk</dt><dd>{int(c.rejected)}</dd>
            <dt>Orders / fills</dt><dd>{int(c.orders)} / {int(c.fills)}</dd>
            <dt>Closed trades</dt><dd>{int(doc.report.tradeCount)} ({int(doc.report.winningTrades)} won, {int(doc.report.losingTrades)} lost)</dd>
            <dt>Gross profit / loss</dt><dd>{money(doc.report.grossProfit)} / {money(doc.report.grossLoss)}</dd>
            <dt>Final cash</dt><dd>{money(doc.final.cash)}</dd>
            <dt>Fill rule</dt><dd>Next bar's open (its close when the data has no open), {doc.config.slippageBps} bps slippage</dd>
          </dl>
        </Panel>
        <Panel title="Positions at the end" flush>
          <DataTable label="Final positions" rows={doc.final.positions} columns={positionColumns} rowKey={(p) => p.symbol} empty={<Empty title="Flat">No position was open when the data ended.</Empty>} />
        </Panel>
        <Panel title="Fills" className="span-all" hint={`${int(doc.fills.length)} executions`} flush>
          <DataTable label="Backtest fills" rows={doc.fills} columns={fillColumns} rowKey={(f) => f.fillId} initialSort={{ key: "time", dir: "asc" }} maxHeight={420} limit={2000} empty={<Empty title="No fills">The strategy's requests were all rejected, or it made none.</Empty>} />
        </Panel>
        <Panel title="Risk decisions" className="span-all" hint="One per signal" flush>
          <DataTable label="Risk decisions" rows={doc.decisions} columns={decisionColumns} rowKey={(d) => d.signalId} initialSort={{ key: "signal", dir: "asc" }} maxHeight={420} limit={2000} empty={<Empty title="No signals" />} />
        </Panel>
      </div>
    </>
  );
}

export function StrategiesBacktest() {
  const lab = useLab();
  const bt = useBacktest();
  const error = bt.run.phase === "failed" ? bt.run.error : undefined;
  return (
    <div className="lab">
      <aside className="lab-side">
        <ConfigPanel state={lab} onRun={() => void labStore.runReplay()} actions={<Settings lab={lab} bt={bt} />} />
      </aside>
      <div className="lab-main">
        {error && (
          <div style={{ marginBottom: 10 }}>
            <Notice tone="down">
              <b>The backtest failed.</b> {error.message}
              {error.problems.length > 0 && <ul style={{ margin: "6px 0 0" }}>{error.problems.slice(0, 10).map((p, i) => <li key={i}>{p.where ? `${p.where}: ` : ""}{p.message}</li>)}</ul>}
              {bt.result && <div className="faint">The last successful backtest is still shown below.</div>}
            </Notice>
          </div>
        )}
        {bt.result ? <Result lab={lab} bt={bt} /> : bt.run.phase !== "running" && (
          <Panel>
            <Empty title="No backtest yet">
              Pick a strategy and a dataset, adjust the account, costs and risk limits if you like, then press Run backtest. The real engine trades the strategy's signals and reports equity, drawdown, every fill and every risk decision.
            </Empty>
          </Panel>
        )}
      </div>
    </div>
  );
}
