import type { ReactNode } from "react";
import type { OrderSide, OrderStatus, RiskOutcome, RunStatus, SignalSide } from "../api/contract";
import { sign } from "../lib/decimal";
import { IconAlert, IconArrowDown, IconArrowUp, IconCheck, IconInfo, IconResize, IconX } from "./icons";

export function Panel({ title, hint, tools, children, flush, className }: {
  title?: ReactNode;
  hint?: ReactNode;
  tools?: ReactNode;
  children: ReactNode;
  flush?: boolean;
  className?: string;
}) {
  return (
    <section className={`panel ${className ?? ""}`}>
      {(title || tools) && (
        <header className="panel-head">
          {title && <h2>{title}</h2>}
          {hint && <span className="hint">{hint}</span>}
          {tools && <div className="panel-tools">{tools}</div>}
        </header>
      )}
      <div className={flush ? "panel-flush" : "panel-body"}>{children}</div>
    </section>
  );
}

export function Figure({ label, value, sub, lead, tone }: {
  label: string;
  value: ReactNode;
  sub?: ReactNode;
  lead?: boolean;
  tone?: "up" | "down";
}) {
  return (
    <div className={`figure ${lead ? "figure-lead" : ""}`}>
      <div className="figure-label">{label}</div>
      <div className={`figure-value ${tone ?? ""}`}>{value}</div>
      {sub !== undefined && <div className="figure-sub">{sub}</div>}
    </div>
  );
}

// Colour for a signed decimal, so gains and losses also carry their sign in text.
export function toneOf(text: string): "up" | "down" | undefined {
  const s = sign(text);
  return s > 0 ? "up" : s < 0 ? "down" : undefined;
}

export function toneOfNumber(n: number): "up" | "down" | undefined {
  return n > 0 ? "up" : n < 0 ? "down" : undefined;
}

export function Badge({ tone, icon, children }: { tone?: "up" | "down" | "warn" | "accent"; icon?: ReactNode; children: ReactNode }) {
  return (
    <span className={`badge ${tone ? `badge-${tone}` : ""}`}>
      {icon}
      {children}
    </span>
  );
}

const RUN_STATUS: Record<RunStatus, { text: string; tone?: "up" | "down" | "warn" | "accent" }> = {
  queued: { text: "Queued" },
  running: { text: "Running", tone: "accent" },
  completed: { text: "Completed" },
  failed: { text: "Failed", tone: "down" },
  stopped: { text: "Stopped", tone: "warn" },
};

export function RunStatusBadge({ status, live }: { status: RunStatus; live?: boolean }) {
  if (live && status === "running") {
    return <Badge tone="up" icon={<span className="live-dot" />}>Live</Badge>;
  }
  const s = RUN_STATUS[status];
  return (
    <Badge tone={s.tone} icon={status === "failed" ? <IconAlert size={12} /> : undefined}>
      {s.text}
    </Badge>
  );
}

const ORDER_STATUS: Record<OrderStatus, { text: string; tone?: "up" | "down" | "warn" | "accent" }> = {
  new: { text: "New" },
  pending_risk: { text: "Pending risk" },
  rejected: { text: "Rejected", tone: "down" },
  approved: { text: "Approved", tone: "accent" },
  working: { text: "Working", tone: "accent" },
  partially_filled: { text: "Partially filled", tone: "warn" },
  filled: { text: "Filled", tone: "up" },
  cancelled: { text: "Cancelled" },
  expired: { text: "Expired" },
};

export function OrderStatusBadge({ status }: { status: OrderStatus }) {
  const s = ORDER_STATUS[status];
  const icon = status === "filled" ? <IconCheck size={12} /> : status === "rejected" ? <IconX size={12} /> : undefined;
  return <Badge tone={s.tone} icon={icon}>{s.text}</Badge>;
}

export const orderStatusText = (status: OrderStatus) => ORDER_STATUS[status].text;

const OUTCOME: Record<RiskOutcome, { text: string; tone: "up" | "down" | "warn"; icon: ReactNode }> = {
  approved: { text: "Approved", tone: "up", icon: <IconCheck size={12} /> },
  resized: { text: "Resized", tone: "warn", icon: <IconResize size={12} /> },
  rejected: { text: "Rejected", tone: "down", icon: <IconX size={12} /> },
};

export function OutcomeBadge({ outcome }: { outcome: RiskOutcome }) {
  const o = OUTCOME[outcome];
  return <Badge tone={o.tone} icon={o.icon}>{o.text}</Badge>;
}

export function Side({ side }: { side: OrderSide | SignalSide }) {
  return (
    <span className={`side side-${side}`}>
      {side === "buy" ? <IconArrowUp size={12} /> : side === "sell" ? <IconArrowDown size={12} /> : null}
      {side === "buy" ? "Buy" : side === "sell" ? "Sell" : "Flat"}
    </span>
  );
}

export function Segmented<T extends string>({ value, options, onChange, label }: {
  value: T;
  options: { value: T; label: string }[];
  onChange: (value: T) => void;
  label: string;
}) {
  return (
    <div className="segmented" role="group" aria-label={label}>
      {options.map((o) => (
        <button key={o.value} type="button" aria-pressed={o.value === value} onClick={() => onChange(o.value)}>
          {o.label}
        </button>
      ))}
    </div>
  );
}

export function Meter({ value, label }: { value: number | null; label: string }) {
  const v = value ?? 0;
  const level = v >= 1 ? "breach" : v >= 0.8 ? "warn" : "ok";
  return (
    <div className="meter" data-level={level} role="meter" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(v * 100)}>
      <span style={{ width: `${Math.min(100, Math.max(0, v * 100))}%` }} />
    </div>
  );
}

export function Empty({ title, children }: { title: string; children?: ReactNode }) {
  return (
    <div className="empty">
      <strong>{title}</strong>
      {children && <span>{children}</span>}
    </div>
  );
}

export function Notice({ tone, children }: { tone?: "down" | "warn"; children: ReactNode }) {
  return (
    <div className={`notice ${tone ? `notice-${tone}` : ""}`} role={tone === "down" ? "alert" : undefined}>
      {tone ? <IconAlert /> : <IconInfo />}
      <div>{children}</div>
    </div>
  );
}

export function Skeleton({ height = 16, width = "100%" }: { height?: number; width?: number | string }) {
  return <div className="skeleton" style={{ height, width }} aria-hidden="true" />;
}

export function Loading({ height = 240 }: { height?: number }) {
  return (
    <div className="panel-body" aria-busy="true" aria-label="Loading">
      <Skeleton height={height} />
    </div>
  );
}
