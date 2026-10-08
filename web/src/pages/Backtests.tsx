import { useNavigate } from "react-router";
import { Link } from "react-router";
import type { RunSummary } from "../api/contract";
import { DataTable, type Column } from "../components/DataTable";
import { IconPlus } from "../components/icons";
import { Empty, Loading, Panel, RunStatusBadge, toneOfNumber } from "../components/ui";
import { int, percent, period, relative } from "../lib/format";
import { useEngine } from "../state/engine";

const columns: Column<RunSummary>[] = [
  {
    key: "name", header: "Backtest", render: (r) => (
      <div style={{ display: "grid", lineHeight: 1.3 }}>
        <b style={{ fontWeight: 500 }}>{r.name}</b>
        <span className="faint" style={{ fontSize: "var(--fs-xs)" }}>{r.strategies.join(", ")}</span>
      </div>
    ), sort: (r) => r.name,
  },
  {
    key: "status", header: "Status", render: (r) => (
      r.status === "running" && r.progress !== undefined ? (
        <div className="row"><RunStatusBadge status={r.status} /><div className="progress-line"><span style={{ width: `${Math.round(r.progress * 100)}%` }} /></div></div>
      ) : <RunStatusBadge status={r.status} />
    ), sort: (r) => r.status,
  },
  { key: "period", header: "Period", render: (r) => period(r.period_start, r.period_end), sort: (r) => r.period_start },
  { key: "symbols", header: "Symbols", render: (r) => <span className="muted">{r.symbols.join(", ")}</span> },
  { key: "return", header: "Return", align: "right", render: (r) => (r.report ? <span className={toneOfNumber(r.report.total_return)}>{percent(r.report.total_return)}</span> : <span className="faint">—</span>), sort: (r) => r.report?.total_return ?? -Infinity },
  { key: "dd", header: "Max drawdown", align: "right", render: (r) => (r.report ? percent(r.report.max_drawdown) : <span className="faint">—</span>), sort: (r) => r.report?.max_drawdown ?? -Infinity },
  { key: "trades", header: "Trades", align: "right", render: (r) => (r.report ? int(r.report.trade_count) : <span className="faint">—</span>), sort: (r) => r.report?.trade_count ?? -1 },
  { key: "created", header: "Started", align: "right", render: (r) => <span className="muted">{relative(r.created_at)}</span>, sort: (r) => r.created_at },
];

export function Backtests() {
  const { runs } = useEngine();
  const navigate = useNavigate();
  const backtests = runs?.filter((r) => r.mode === "backtest");

  return (
    <>
      <div className="page-head">
        <div>
          <h1>Backtests</h1>
          <p>Replays of recorded market data through the same strategy, risk, execution and portfolio stages as live runs.</p>
        </div>
        <div className="page-actions">
          <Link className="btn btn-primary" to="/backtests/new">
            <IconPlus />
            New backtest
          </Link>
        </div>
      </div>
      <Panel flush>
        {backtests ? (
          <DataTable
            label="Backtests"
            rows={backtests}
            columns={columns}
            rowKey={(r) => r.run_id}
            initialSort={{ key: "created", dir: "desc" }}
            onRowClick={(r) => navigate(`/runs/${r.run_id}/report`)}
            empty={
              <Empty title="No backtests yet">
                <Link to="/backtests/new">Run your first backtest</Link> to replay recorded bars through your strategies.
              </Empty>
            }
          />
        ) : <Loading height={240} />}
      </Panel>
    </>
  );
}
