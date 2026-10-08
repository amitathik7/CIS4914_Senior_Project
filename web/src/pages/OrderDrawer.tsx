import { useEffect, useRef } from "react";
import { IconCheck, IconClose, IconResize, IconX } from "../components/icons";
import { Empty, Loading, OrderStatusBadge, OutcomeBadge, Side, orderStatusText } from "../components/ui";
import { formatDecimal, money, price } from "../lib/decimal";
import { int, label, micros } from "../lib/format";
import { useRunResource } from "../state/engine";

interface Props {
  runId: string;
  orderId: number;
  onClose: () => void;
}

export function OrderDrawer({ runId, orderId, onClose }: Props) {
  const trace = useRunResource(runId, async (c, id) => {
    const [orders, signals, decisions, fills] = await Promise.all([c.orders(id), c.signals(id), c.riskDecisions(id), c.fills(id)]);
    const order = orders.find((o) => o.id === orderId);
    return {
      order,
      signal: order && signals.find((s) => s.id === order.origin_signal),
      decision: order && decisions.find((d) => d.order_id === order.id),
      fills: fills.filter((f) => f.order_id === orderId),
    };
  }, [orderId]);
  const closeBtn = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    closeBtn.current?.focus();
    const esc = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    document.addEventListener("keydown", esc);
    return () => document.removeEventListener("keydown", esc);
  }, [onClose]);

  const t = trace.data;
  const o = t?.order;

  return (
    <>
      <div className="drawer-scrim" onClick={onClose} />
      <aside className="drawer" role="dialog" aria-modal="true" aria-labelledby="drawer-title">
        <header className="drawer-head">
          <div>
            <h2 id="drawer-title">Order <span className="id">#{orderId}</span></h2>
            {o && (
              <p className="muted" style={{ marginTop: 4 }}>
                <Side side={o.side} /> {int(o.quantity)} {o.symbol}, {o.type} order from {o.strategy_id}
              </p>
            )}
          </div>
          <div className="row" style={{ marginLeft: "auto" }}>
            {o && <OrderStatusBadge status={o.status} />}
            <button ref={closeBtn} type="button" className="btn btn-ghost icon-btn" onClick={onClose} aria-label="Close">
              <IconClose />
            </button>
          </div>
        </header>
        <div className="drawer-body">
          {!t ? <Loading height={300} /> : !o ? <Empty title="Order not found" /> : (
            <ol className="trace">
              {t.signal && (
                <li className="trace-step" data-tone="accent">
                  <span className="trace-marker" />
                  <div className="trace-title">
                    Signal <span className="id">#{t.signal.id}</span>
                    <time dateTime={t.signal.created_at}>{micros(t.signal.created_at)}</time>
                  </div>
                  <div className="trace-body">
                    <dl className="kv">
                      <dt>Strategy</dt><dd>{t.signal.strategy_id}</dd>
                      <dt>Request</dt><dd><Side side={t.signal.side} /> {int(t.signal.requested_quantity ?? 0)} {t.signal.symbol}</dd>
                      {Object.entries(t.signal.metadata).map(([k, v]) => (
                        <FragmentKV key={k} k={label(k)} v={k === "trigger" ? label(v) : v} />
                      ))}
                    </dl>
                  </div>
                </li>
              )}

              {t.decision && (
                <li className="trace-step" data-tone={t.decision.outcome === "approved" ? "up" : t.decision.outcome === "resized" ? "warn" : "down"}>
                  <span className="trace-marker" />
                  <div className="trace-title">
                    Risk check <OutcomeBadge outcome={t.decision.outcome} />
                    <time dateTime={t.decision.decided_at}>{micros(t.decision.decided_at)}</time>
                  </div>
                  <div className="trace-body">
                    {t.decision.outcome !== "approved" && (
                      <p style={{ marginBottom: 8 }}>
                        {t.decision.outcome === "rejected"
                          ? `Rejected by ${label(t.decision.reason ?? "")}.`
                          : `Requested ${int(t.decision.requested_quantity)}, approved ${int(t.decision.approved_quantity)}.`}
                      </p>
                    )}
                    <ul className="checks">
                      {t.decision.checks.map((c) => {
                        const resized = !c.passed && /^Resized/.test(c.detail);
                        return (
                          <li key={c.policy}>
                            {c.passed ? <IconCheck className="up" /> : resized ? <IconResize style={{ color: "var(--warn)" }} /> : <IconX className="down" />}
                            <span>
                              <span className="policy">{label(c.policy)}</span>
                              <br />
                              {c.detail}
                            </span>
                            <span className="faint num">{limitText(c.policy, c.limit)}</span>
                          </li>
                        );
                      })}
                    </ul>
                  </div>
                </li>
              )}

              <li className="trace-step" data-tone={o.status === "rejected" ? "down" : "accent"}>
                <span className="trace-marker" />
                <div className="trace-title">Order lifecycle</div>
                <div className="trace-body">
                  <dl className="kv">
                    {o.history.map((h, i) => (
                      <FragmentKV key={i} k={orderStatusText(h.status)} v={<>{micros(h.at)}{h.note ? <span className="faint"> {h.note}</span> : null}</>} />
                    ))}
                  </dl>
                </div>
              </li>

              {t.fills.length > 0 && (
                <li className="trace-step" data-tone="up">
                  <span className="trace-marker" />
                  <div className="trace-title">
                    {t.fills.length === 1 ? "Fill" : `${t.fills.length} fills`}
                    <span className="faint num" style={{ fontWeight: 400 }}>
                      {int(o.filled_quantity)} of {int(o.quantity)}{o.average_fill_price ? ` at avg ${price(o.average_fill_price)}` : ""}
                    </span>
                  </div>
                  <div className="trace-body">
                    <table className="table" style={{ marginTop: 4 }}>
                      <thead>
                        <tr><th>Time</th><th className="r">Qty</th><th className="r">Price</th><th className="r">Slippage</th><th className="r">Fees</th></tr>
                      </thead>
                      <tbody>
                        {t.fills.map((f) => (
                          <tr key={f.id}>
                            <td>{micros(f.filled_at)}</td>
                            <td className="r">{int(f.filled_quantity)}</td>
                            <td className="r">{price(f.fill_price)}</td>
                            <td className="r">{price(f.slippage)}</td>
                            <td className="r">{money(f.fees)}</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                </li>
              )}
            </ol>
          )}
        </div>
      </aside>
    </>
  );
}

const MONEY_POLICIES = new Set(["max_daily_loss", "max_position_notional", "max_gross_exposure", "buying_power"]);

function limitText(policy: string, limit: string | undefined): string {
  if (limit === undefined) return "";
  if (policy === "buying_power") return `${money(limit)} available`;
  if (MONEY_POLICIES.has(policy)) return `limit ${formatDecimal(limit, { dp: 0, currency: true })}`;
  if (policy === "short_selling") return `${int(Number(limit))} held`;
  return `limit ${int(Number(limit))}`;
}

function FragmentKV({ k, v }: { k: string; v: React.ReactNode }) {
  return (
    <>
      <dt>{k}</dt>
      <dd>{v}</dd>
    </>
  );
}
