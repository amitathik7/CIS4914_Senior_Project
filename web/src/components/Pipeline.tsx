import { useEffect, useRef, useState } from "react";
import type { Stage, SystemMetrics } from "../api/contract";
import { compact, int, latency } from "../lib/format";

// The engine's event path, left to right, with what each stage has handled so far.
const PATH: { stage: Stage | "bus"; name: string }[] = [
  { stage: "ingest", name: "Market data" },
  { stage: "bus", name: "Event bus" },
  { stage: "strategy", name: "Strategy" },
  { stage: "risk", name: "Risk" },
  { stage: "execution", name: "Execution" },
  { stage: "portfolio", name: "Portfolio" },
];

function Arrow() {
  return (
    <svg className="stage-arrow" viewBox="0 0 12 60" preserveAspectRatio="none" aria-hidden="true">
      <path d="M0 0 L11 30 L0 60" fill="none" stroke="currentColor" strokeWidth="1" vectorEffect="non-scaling-stroke" />
    </svg>
  );
}

// Minimum time between pulses: often enough to show the run is live, rare enough not to distract.
const PULSE_EVERY_MS = 10_000;

export function Pipeline({ metrics, live }: { metrics: SystemMetrics; live: boolean }) {
  const previous = useRef(new Map<string, number>());
  const lastPulse = useRef(0);
  const timer = useRef<number | undefined>(undefined);
  const [pulsing, setPulsing] = useState<Set<string>>(new Set());

  useEffect(() => {
    if (!live) return;
    const moved = new Set<string>();
    for (const s of metrics.stages) {
      const before = previous.current.get(s.stage);
      if (before !== undefined && s.events > before) moved.add(s.stage);
      previous.current.set(s.stage, s.events);
    }
    if (moved.has("ingest")) moved.add("bus");
    const now = Date.now();
    if (moved.size === 0 || now - lastPulse.current < PULSE_EVERY_MS) return;
    lastPulse.current = now;
    setPulsing(moved);
    timer.current = window.setTimeout(() => setPulsing(new Set()), 2000);
  }, [metrics, live]);

  useEffect(() => () => window.clearTimeout(timer.current), []);

  const latest = metrics.history[metrics.history.length - 1];

  return (
    <div className="pipeline" role="list" aria-label="Event pipeline">
      {PATH.map(({ stage, name }, i) => {
        const m = metrics.stages.find((s) => s.stage === stage);
        const active = live && pulsing.has(stage);
        return (
          <div key={stage} className="stage" role="listitem" data-active={live} data-pulse={active}>
            {i > 0 && <Arrow />}
            <div className="stage-name">
              <span className="stage-dot" />
              {name}
            </div>
            {stage === "bus" ? (
              <>
                <div className="stage-count">{latest ? int(latest.queue_depth) : "0"} queued</div>
                <div className="stage-meta">capacity {compact(metrics.queue_capacity)}</div>
              </>
            ) : (
              <>
                <div className="stage-count">{m ? compact(m.events) : "0"}</div>
                <div className="stage-meta">
                  {m && m.events > 0 ? `p50 ${latency(m.latency_us.p50)}` : "idle"}
                  {m && m.errors > 0 && <span className="down"> {m.errors} errors</span>}
                </div>
              </>
            )}
            <span className="stage-pulse" />
          </div>
        );
      })}
    </div>
  );
}
