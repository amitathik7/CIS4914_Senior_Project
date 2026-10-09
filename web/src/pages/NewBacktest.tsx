import { useState, type FormEvent, type ReactNode } from "react";
import { Link, useNavigate } from "react-router";
import { EngineApiError } from "../api/client";
import type { BacktestRequest, StrategyConfig } from "../api/contract";
import { IconPlus, IconX } from "../components/icons";
import { Notice, Panel, Segmented } from "../components/ui";
import { useEngine } from "../state/engine";

type Kind = StrategyConfig["kind"];

interface StrategyDraft {
  key: number;
  kind: Kind;
  strategy_id: string;
  symbols: string[];
  quantity: string;
  short_window: string;
  long_window: string;
  lookback: string;
  entry: string;
  rearm: string;
}

let nextKey = 1;

function draft(kind: Kind, taken: string[], symbol: string): StrategyDraft {
  const base = kind === "sma_crossover" ? "sma_crossover" : "mean_reversion";
  let id = base;
  for (let n = 2; taken.includes(id); n++) id = `${base}_${n}`;
  return {
    key: nextKey++, kind, strategy_id: id, symbols: symbol ? [symbol] : [], quantity: kind === "sma_crossover" ? "100" : "50",
    short_window: "5", long_window: "20", lookback: "20", entry: "2", rearm: "0.5",
  };
}

function Field({ label, hint, error, children, prefix }: { label: string; hint?: string; error?: string; children: ReactNode; prefix?: string }) {
  return (
    <label className="field" data-invalid={error ? "true" : undefined}>
      <span>{label}</span>
      {prefix ? <div className="input-affix"><span>{prefix}</span>{children}</div> : children}
      {(error || hint) && <small>{error ?? hint}</small>}
    </label>
  );
}

const int = (v: string) => (/^\d+$/.test(v.trim()) ? Number(v) : NaN);
const num = (v: string) => (/^-?\d*\.?\d+$/.test(v.trim()) ? Number(v) : NaN);
const dec = (v: string) => v.trim().replace(/,/g, "");

export function NewBacktest() {
  const { client, info, refreshRuns } = useEngine();
  const navigate = useNavigate();
  const coverage = info?.market_data;
  const symbols = coverage?.symbols ?? [];
  const last = coverage?.last_session ?? "";

  const [name, setName] = useState("");
  const [start, setStart] = useState("2026-09-21");
  const [end, setEnd] = useState("2026-09-25");
  const [strategies, setStrategies] = useState<StrategyDraft[]>(() => [draft("sma_crossover", [], "AAPL")]);
  const [cash, setCash] = useState("100000");
  const [slippage, setSlippage] = useState("1");
  const [perShare, setPerShare] = useState("0.0035");
  const [participation, setParticipation] = useState("1");
  const [toOrder, setToOrder] = useState("1");
  const [toFill, setToFill] = useState("5");
  const [maxQty, setMaxQty] = useState("1000");
  const [maxPosition, setMaxPosition] = useState("25000");
  const [maxGross, setMaxGross] = useState("100000");
  const [maxLoss, setMaxLoss] = useState("5000");
  const [maxOpen, setMaxOpen] = useState("50");
  const [error, setError] = useState<{ field?: string; message: string }>();
  const [submitting, setSubmitting] = useState(false);

  const update = (key: number, patch: Partial<StrategyDraft>) =>
    setStrategies((list) => list.map((s) => (s.key === key ? { ...s, ...patch } : s)));

  const fieldError = (field: string) => (error?.field === field ? error.message : undefined);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    setError(undefined);
    const request: BacktestRequest = {
      name: name.trim() || `${strategies.map((s) => s.strategy_id).join(" + ")}, ${start} to ${end}`,
      period_start: `${start}T13:30:00Z`,
      period_end: `${end}T20:00:00Z`,
      strategies: strategies.map((s) => ({
        kind: s.kind,
        strategy_id: s.strategy_id.trim(),
        symbols: s.symbols,
        requested_quantity: int(s.quantity),
        ...(s.kind === "sma_crossover"
          ? { short_window: int(s.short_window), long_window: int(s.long_window) }
          : { lookback: int(s.lookback), entry_threshold: num(s.entry), rearm_threshold: num(s.rearm) }),
      })),
      risk: {
        max_order_quantity: int(maxQty),
        max_position_notional: dec(maxPosition),
        max_gross_exposure: dec(maxGross),
        max_daily_loss: dec(maxLoss),
        max_open_orders: int(maxOpen),
        allow_short: false,
      },
      simulation: {
        starting_cash: dec(cash),
        slippage_bps: num(slippage),
        fees: { per_share: dec(perShare), per_trade: "0", bps: 0 },
        latency_ms: { signal_to_order: num(toOrder), order_to_fill: num(toFill) },
        participation_cap: num(participation) / 100,
      },
    };
    setSubmitting(true);
    try {
      const run = await client.createBacktest(request);
      refreshRuns();
      navigate(`/runs/${run.run_id}/report`);
    } catch (err) {
      setError(err instanceof EngineApiError ? { field: err.field, message: err.message } : { message: "The engine did not accept the backtest. Try again." });
      window.scrollTo({ top: 0, behavior: "smooth" });
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <form onSubmit={submit} noValidate>
      <div className="page-head">
        <div>
          <h1>New backtest</h1>
          <p>Replay recorded one-minute bars through strategies, risk and the execution simulator.</p>
        </div>
        <div className="page-actions">
          <Link to="/backtests" className="btn btn-ghost">Cancel</Link>
          <button type="submit" className="btn btn-primary" disabled={submitting}>{submitting ? "Starting…" : "Run backtest"}</button>
        </div>
      </div>

      {error && <div style={{ marginBottom: 14 }}><Notice tone="down">{error.message}</Notice></div>}

      <Panel>
        <section className="form-section">
          <header>
            <h2>Run</h2>
            <p>{coverage ? `Recorded data covers ${coverage.first_session} to ${coverage.last_session}.` : ""}</p>
          </header>
          <div className="form-grid">
            <Field label="Name" hint="Optional" error={fieldError("name")}>
              <input className="input" value={name} onChange={(e) => setName(e.target.value)} placeholder="Describe what you are testing" />
            </Field>
            <Field label="First session" error={fieldError("period")}>
              <input className="input" type="date" value={start} min={coverage?.first_session} max={last} onChange={(e) => setStart(e.target.value)} required />
            </Field>
            <Field label="Last session">
              <input className="input" type="date" value={end} min={start} max={last} onChange={(e) => setEnd(e.target.value)} required />
            </Field>
          </div>
        </section>

        <section className="form-section">
          <header>
            <h2>Strategies</h2>
            <p>Each strategy sees only the symbols listed for it. Ids must be unique.</p>
          </header>
          <div>
            {strategies.map((s, i) => {
              const at = `strategies.${i}`;
              return (
                <div className="strategy-card" key={s.key}>
                  <div className="strategy-card-head">
                    <Segmented
                      label="Strategy kind"
                      value={s.kind}
                      onChange={(kind) => update(s.key, { ...draft(kind, strategies.filter((x) => x.key !== s.key).map((x) => x.strategy_id), ""), key: s.key, symbols: s.symbols })}
                      options={[{ value: "sma_crossover", label: "Moving-average crossover" }, { value: "mean_reversion", label: "Mean reversion" }]}
                    />
                    {strategies.length > 1 && (
                      <button type="button" className="btn btn-ghost btn-sm" style={{ marginLeft: "auto" }} onClick={() => setStrategies((l) => l.filter((x) => x.key !== s.key))}>
                        <IconX size={13} /> Remove
                      </button>
                    )}
                  </div>
                  <div className="form-grid">
                    <Field label="Strategy id" error={fieldError(`${at}.strategy_id`)}>
                      <input className="input" value={s.strategy_id} onChange={(e) => update(s.key, { strategy_id: e.target.value })} />
                    </Field>
                    <Field label="Shares per signal" error={fieldError(`${at}.requested_quantity`)}>
                      <input className="input" inputMode="numeric" value={s.quantity} onChange={(e) => update(s.key, { quantity: e.target.value })} />
                    </Field>
                    {s.kind === "sma_crossover" ? (
                      <>
                        <Field label="Short window" hint="Bars" error={fieldError(`${at}.short_window`)}>
                          <input className="input" inputMode="numeric" value={s.short_window} onChange={(e) => update(s.key, { short_window: e.target.value })} />
                        </Field>
                        <Field label="Long window" hint="Bars; first signal after this many" error={fieldError(`${at}.long_window`)}>
                          <input className="input" inputMode="numeric" value={s.long_window} onChange={(e) => update(s.key, { long_window: e.target.value })} />
                        </Field>
                      </>
                    ) : (
                      <>
                        <Field label="Lookback" hint="Bars in the z-score window" error={fieldError(`${at}.lookback`)}>
                          <input className="input" inputMode="numeric" value={s.lookback} onChange={(e) => update(s.key, { lookback: e.target.value })} />
                        </Field>
                        <Field label="Entry threshold" hint={`|z| at or beyond; max reachable ${Math.sqrt(Math.max(1, int(s.lookback) - 1) || 0).toFixed(2)}`} error={fieldError(`${at}.entry_threshold`)}>
                          <input className="input" inputMode="decimal" value={s.entry} onChange={(e) => update(s.key, { entry: e.target.value })} />
                        </Field>
                        <Field label="Rearm threshold" hint="|z| that resets the latch" error={fieldError(`${at}.rearm_threshold`)}>
                          <input className="input" inputMode="decimal" value={s.rearm} onChange={(e) => update(s.key, { rearm: e.target.value })} />
                        </Field>
                      </>
                    )}
                  </div>
                  <div className="field" style={{ marginTop: 14 }} data-invalid={fieldError(`${at}.symbols`) ? "true" : undefined}>
                    <span>Symbols</span>
                    <div className="chips" role="group" aria-label="Symbols">
                      {symbols.map((sym) => {
                        const on = s.symbols.includes(sym);
                        return (
                          <button key={sym} type="button" className="chip-toggle" aria-pressed={on} onClick={() => update(s.key, { symbols: on ? s.symbols.filter((x) => x !== sym) : [...s.symbols, sym] })}>
                            {sym}
                          </button>
                        );
                      })}
                    </div>
                    {fieldError(`${at}.symbols`) && <small>{fieldError(`${at}.symbols`)}</small>}
                  </div>
                </div>
              );
            })}
            {strategies.length < 4 && (
              <button
                type="button"
                className="btn"
                style={{ marginTop: 12 }}
                onClick={() => setStrategies((l) => [...l, draft(l.some((x) => x.kind === "sma_crossover") ? "mean_reversion" : "sma_crossover", l.map((x) => x.strategy_id), "SPY")])}
              >
                <IconPlus /> Add strategy
              </button>
            )}
          </div>
        </section>

        <section className="form-section">
          <header>
            <h2>Risk limits</h2>
            <p>Checked against the portfolio before every order. Over-size orders are resized; orders that cannot fit are rejected.</p>
          </header>
          <div className="form-grid">
            <Field label="Max order size" hint="Shares" error={fieldError("risk.max_order_quantity")}>
              <input className="input" inputMode="numeric" value={maxQty} onChange={(e) => setMaxQty(e.target.value)} />
            </Field>
            <Field label="Max position" hint="Per symbol" prefix="$" error={fieldError("risk.max_position_notional")}>
              <input className="input" inputMode="decimal" value={maxPosition} onChange={(e) => setMaxPosition(e.target.value)} />
            </Field>
            <Field label="Max gross exposure" prefix="$" error={fieldError("risk.max_gross_exposure")}>
              <input className="input" inputMode="decimal" value={maxGross} onChange={(e) => setMaxGross(e.target.value)} />
            </Field>
            <Field label="Daily loss stop" prefix="$" error={fieldError("risk.max_daily_loss")}>
              <input className="input" inputMode="decimal" value={maxLoss} onChange={(e) => setMaxLoss(e.target.value)} />
            </Field>
            <Field label="Max open orders" error={fieldError("risk.max_open_orders")}>
              <input className="input" inputMode="numeric" value={maxOpen} onChange={(e) => setMaxOpen(e.target.value)} />
            </Field>
          </div>
        </section>

        <section className="form-section">
          <header>
            <h2>Execution</h2>
            <p>How the simulator fills orders. Fills never exceed the participation cap of a bar’s volume.</p>
          </header>
          <div className="form-grid">
            <Field label="Starting cash" prefix="$" error={fieldError("simulation.starting_cash")}>
              <input className="input" inputMode="decimal" value={cash} onChange={(e) => setCash(e.target.value)} />
            </Field>
            <Field label="Slippage" hint="Basis points" error={fieldError("simulation.slippage_bps")}>
              <input className="input" inputMode="decimal" value={slippage} onChange={(e) => setSlippage(e.target.value)} />
            </Field>
            <Field label="Fee per share" prefix="$" error={fieldError("simulation.fees.per_share")}>
              <input className="input" inputMode="decimal" value={perShare} onChange={(e) => setPerShare(e.target.value)} />
            </Field>
            <Field label="Participation cap" hint="Percent of bar volume" error={fieldError("simulation.participation_cap")}>
              <input className="input" inputMode="decimal" value={participation} onChange={(e) => setParticipation(e.target.value)} />
            </Field>
            <Field label="Signal to order" hint="Milliseconds" error={fieldError("simulation.latency_ms.signal_to_order")}>
              <input className="input" inputMode="decimal" value={toOrder} onChange={(e) => setToOrder(e.target.value)} />
            </Field>
            <Field label="Order to fill" hint="Milliseconds" error={fieldError("simulation.latency_ms.order_to_fill")}>
              <input className="input" inputMode="decimal" value={toFill} onChange={(e) => setToFill(e.target.value)} />
            </Field>
          </div>
        </section>
      </Panel>
    </form>
  );
}
