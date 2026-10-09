import { useEffect, useRef, useState } from "react";
import { NavLink, Outlet, useLocation, useNavigate, useParams } from "react-router";
import type { RunSummary } from "../api/contract";
import { day, period, time } from "../lib/format";
import { useEngine, useRunResource } from "../state/engine";
import { setTheme, useTheme } from "../state/theme";
import {
  IconBacktests, IconChevronDown, IconMoon, IconOrders, IconOverview, IconReport, IconRisk, IconSun, IconSystem,
} from "./icons";
import { Badge, RunStatusBadge } from "./ui";

const RUN_PAGES = [
  { path: "overview", label: "Overview", icon: IconOverview },
  { path: "orders", label: "Orders", icon: IconOrders },
  { path: "risk", label: "Risk", icon: IconRisk },
  { path: "report", label: "Report", icon: IconReport },
  { path: "system", label: "System", icon: IconSystem },
];

function useCurrentRun(): RunSummary | undefined {
  const { runs } = useEngine();
  const { runId } = useParams();
  return runs?.find((r) => r.run_id === runId);
}

function Mark() {
  return (
    <svg width="22" height="22" viewBox="0 0 32 32" aria-hidden="true">
      <rect width="32" height="32" rx="7" fill="var(--surface-2)" stroke="var(--line-strong)" />
      <path d="M6 21h5l3-9 4 12 3-7h5" fill="none" stroke="var(--accent)" strokeWidth="2.4" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

function Nav() {
  const { runs, defaultRunId } = useEngine();
  const { runId } = useParams();
  const theme = useTheme();
  const target = runId ?? defaultRunId;
  const running = runs?.filter((r) => r.mode === "backtest" && (r.status === "running" || r.status === "queued")).length ?? 0;

  return (
    <nav className="nav" aria-label="Main">
      <div className="brand">
        <Mark />
        <div>
          Engine console
          <small>Simulated trading engine</small>
        </div>
      </div>
      <div className="nav-group">
        <div className="nav-heading">This run</div>
        {RUN_PAGES.map(({ path, label, icon: Icon }) => (
          <NavLink key={path} to={target ? `/runs/${target}/${path}` : "/"} className="nav-link">
            <Icon />
            {label}
          </NavLink>
        ))}
      </div>
      <div className="nav-group">
        <NavLink to="/backtests" end={false} className="nav-link">
          <IconBacktests />
          Backtests
          {running > 0 && <span className="count">{running} running</span>}
        </NavLink>
      </div>
      <div className="nav-foot">
        <button
          type="button"
          className="btn btn-ghost btn-sm"
          style={{ justifyContent: "flex-start", paddingLeft: 2 }}
          onClick={() => setTheme(theme === "dark" ? "light" : "dark")}
        >
          {theme === "dark" ? <IconSun /> : <IconMoon />}
          {theme === "dark" ? "Light theme" : "Dark theme"}
        </button>
      </div>
    </nav>
  );
}

function RunSwitcher() {
  const { runs } = useEngine();
  const current = useCurrentRun();
  const navigate = useNavigate();
  const location = useLocation();
  const [open, setOpen] = useState(false);
  const box = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent) => !box.current?.contains(e.target as Node) && setOpen(false);
    const esc = (e: KeyboardEvent) => e.key === "Escape" && setOpen(false);
    document.addEventListener("mousedown", close);
    document.addEventListener("keydown", esc);
    return () => {
      document.removeEventListener("mousedown", close);
      document.removeEventListener("keydown", esc);
    };
  }, [open]);

  const choose = (run: RunSummary) => {
    setOpen(false);
    const page = /^\/runs\/[^/]+\/(\w+)/.exec(location.pathname)?.[1] ?? "overview";
    navigate(`/runs/${run.run_id}/${page}`);
  };

  const live = runs?.filter((r) => r.mode === "live") ?? [];
  const backtests = runs?.filter((r) => r.mode === "backtest") ?? [];

  const option = (r: RunSummary) => (
    <button key={r.run_id} type="button" role="option" aria-selected={r.run_id === current?.run_id} className="run-option" onClick={() => choose(r)}>
      <strong>{r.name}</strong>
      <small>{r.mode === "live" ? `Live data since ${time(r.period_start)} ET` : period(r.period_start, r.period_end)}</small>
      <RunStatusBadge status={r.status} live={r.mode === "live"} />
    </button>
  );

  return (
    <div className="run-switch" ref={box}>
      <button type="button" className="run-switch-btn" aria-haspopup="listbox" aria-expanded={open} onClick={() => setOpen((o) => !o)}>
        {current ? (
          <>
            <span className="faint" style={{ fontSize: "var(--fs-sm)" }}>{current.mode === "live" ? "Live run" : "Backtest"}</span>
            <strong>{current.name}</strong>
          </>
        ) : (
          <strong>Choose a run</strong>
        )}
        <IconChevronDown />
      </button>
      {open && (
        <div className="popover" role="listbox" aria-label="Runs" style={{ top: 42, left: 0 }}>
          {live.length > 0 && <div className="popover-label">Live data</div>}
          {live.map(option)}
          {backtests.length > 0 && <div className="popover-label">Backtests</div>}
          {backtests.map(option)}
        </div>
      )}
    </div>
  );
}

function TopBar() {
  const { info } = useEngine();
  const listed = useCurrentRun();
  const fresh = useRunResource(listed?.run_id, (c, id) => c.getRun(id));
  const run = fresh.data?.run_id === listed?.run_id ? fresh.data : listed;
  return (
    <header className="topbar">
      <RunSwitcher />
      {run && <RunStatusBadge status={run.status} live={run.mode === "live"} />}
      <div className="topbar-spacer" />
      {info?.data_source === "demo" && (
        <span title="No engine API is configured, so this console runs a deterministic simulation in the browser. Set VITE_ENGINE_API to connect to the engine.">
          <Badge tone="warn">Demo data</Badge>
        </span>
      )}
      {run?.clock && (
        <div className="clock" aria-label="Run clock">
          <strong>{time(run.clock)} ET</strong>
          <span>{run.mode === "live" ? day(run.clock) : `Replay ended ${day(run.clock)}`}</span>
        </div>
      )}
    </header>
  );
}

export function Shell() {
  return (
    <div className="app">
      <Nav />
      <TopBar />
      <main className="main" id="main">
        <Outlet />
      </main>
    </div>
  );
}

export { useCurrentRun };
