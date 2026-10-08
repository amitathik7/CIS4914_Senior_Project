import { useMemo } from "react";
import { useParams } from "react-router";
import type { UTCTimestamp } from "lightweight-charts";
import type { StageMetrics } from "../api/contract";
import { BarChart } from "../components/charts/BarChart";
import { TimeChart, chartTime } from "../components/charts/TimeChart";
import { DataTable, type Column } from "../components/DataTable";
import { Pipeline } from "../components/Pipeline";
import { useCurrentRun } from "../components/Shell";
import { Empty, Figure, Loading, Notice, Panel } from "../components/ui";
import { compact, int, label, latency } from "../lib/format";
import { useEngine, useRunResource } from "../state/engine";

const columns: Column<StageMetrics>[] = [
  { key: "stage", header: "Stage", render: (s) => <b>{label(s.stage)}</b> },
  { key: "events", header: "Events", align: "right", render: (s) => int(s.events), sort: (s) => s.events },
  { key: "errors", header: "Errors", align: "right", render: (s) => (s.errors ? <span className="down">{int(s.errors)}</span> : <span className="faint">0</span>), sort: (s) => s.errors },
  { key: "min", header: "Min", align: "right", render: (s) => latency(s.latency_us.min) },
  { key: "p50", header: "p50", align: "right", render: (s) => latency(s.latency_us.p50), sort: (s) => s.latency_us.p50 },
  { key: "p99", header: "p99", align: "right", render: (s) => latency(s.latency_us.p99), sort: (s) => s.latency_us.p99 },
  { key: "max", header: "Max", align: "right", render: (s) => latency(s.latency_us.max), sort: (s) => s.latency_us.max },
];

// Wall-clock samples; the chart needs strictly increasing times, so equal seconds are nudged apart.
function samples<T>(history: { time: string }[], pick: (i: number) => T) {
  let last = 0;
  return history.map((h, i) => {
    let t: number = chartTime(h.time);
    if (t <= last) t = last + 1;
    last = t;
    return { time: t as UTCTimestamp, value: pick(i) as number };
  });
}

export function System() {
  const { runId } = useParams();
  const run = useCurrentRun();
  const { info } = useEngine();
  const metrics = useRunResource(runId, (c, id) => c.metrics(id));
  const live = run?.mode === "live" && run.status === "running";
  const m = metrics.data;

  const throughput = useMemo(() => (m ? samples(m.history, (i) => m.history[i]!.events_per_sec) : []), [m]);
  const depth = useMemo(() => (m ? samples(m.history, (i) => m.history[i]!.queue_depth) : []), [m]);
  const histogram = useMemo(
    () => (m ? m.latency_histogram.map((b, i, all) => ({
      label: `≤${b.upper_us}`,
      value: b.count,
      tip: `${int(b.count)} events ${i === 0 ? "at or under" : `over ${all[i - 1]!.upper_us} µs and up to`} ${b.upper_us} µs`,
    })) : []),
    [m],
  );

  const latest = m?.history[m.history.length - 1];
  const e2e = m?.stages.filter((s) => s.stage !== "persistence").reduce((a, s) => a + s.latency_us.p50, 0) ?? 0;

  return (
    <>
      <div className="page-head">
        <div>
          <h1>System</h1>
          <p>How the engine itself is performing: events handled, time spent in each stage, and queue pressure.</p>
        </div>
      </div>

      {info?.data_source === "demo" && (
        <div style={{ marginBottom: 14 }}>
          <Notice tone="warn">Demo data: event counts follow the simulated pipeline, but latencies, queue depth and throughput are synthetic until the engine reports real SystemMetrics.</Notice>
        </div>
      )}

      {m ? (
        <div className="figures">
          <Figure lead label="Events handled" value={int(m.total_events)} sub="all stages" />
          <Figure label={live ? "Throughput" : "Replay throughput"} value={latest ? `${compact(latest.events_per_sec)}/s` : "—"} sub={live ? "last second" : "last sample"} />
          <Figure label="Pipeline p50" value={latency(e2e)} sub="ingest to portfolio" />
          <Figure label="End-to-end p99" value={latest ? `≤ ${latency(latest.p99_us)}` : "—"} sub="histogram bucket" />
          <Figure label="Queue depth" value={latest ? int(latest.queue_depth) : "—"} sub={`of ${compact(m.queue_capacity)}`} />
          <Figure label="Errors" value={<span className={m.total_errors ? "down" : undefined}>{int(m.total_errors)}</span>} sub={`${int(m.dropped_events)} dropped`} />
        </div>
      ) : (
        <div className="figures"><Loading height={58} /></div>
      )}

      {m && <Pipeline metrics={m} live={live} />}

      <div className="grid grid-2">
        <Panel title="Stages" className="span-all" flush>
          {m ? <DataTable label="Per-stage metrics" rows={m.stages} columns={columns} rowKey={(s) => s.stage} /> : <Loading height={220} />}
        </Panel>

        <Panel title="Throughput" hint="Events per second" flush>
          {throughput.length > 1 ? (
            <div style={{ padding: "8px 4px 4px 0" }}>
              <TimeChart seconds label="Throughput over time" height={220} format={(v) => compact(v)} series={[{ id: "tp", kind: "area", tone: "accent", data: throughput }]} />
            </div>
          ) : m ? <Empty title="Not enough samples yet" /> : <Loading height={220} />}
        </Panel>

        <Panel title="Queue depth" hint="Events waiting on the bus" flush>
          {depth.length > 1 ? (
            <div style={{ padding: "8px 4px 4px 0" }}>
              <TimeChart seconds label="Queue depth over time" height={220} series={[{ id: "qd", kind: "line", tone: "line", width: 1, integer: true, data: depth }]} />
            </div>
          ) : m ? <Empty title="Not enough samples yet" /> : <Loading height={220} />}
        </Panel>

        <Panel title="End-to-end latency" hint="Per market event, ingest to portfolio" className="span-all">
          {m ? (
            histogram.some((b) => b.value > 0)
              ? <BarChart label="End-to-end latency distribution" data={histogram} height={220} format={(v) => compact(v)} xLabel="Microseconds" />
              : <Empty title="No events yet" />
          ) : <Loading height={220} />}
        </Panel>
      </div>
    </>
  );
}
