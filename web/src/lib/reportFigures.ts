// The Report page's headline figures, defined once. The Report page and the run-to-run comparison in Strategies both read them
// from here, so a figure means the same thing, is worded the same way, and is unavailable for the same reason in both places.
//
// Rules carried over from the Report page:
//   * Sharpe is shown only when the engine produced one (it needs three or more sessions); otherwise the figure says so.
//   * Profit factor is shown only when there is a losing trade to divide by; otherwise it says why it does not exist.
//   * Win rate exists only once a round trip has closed.
//   * An unavailable figure is a dash with its reason, never 0.

import type { PerformanceReport } from "../api/contract";
import { int, percent, ratioText } from "./format";
import { money } from "./decimal";

export interface ReportFigure {
  key: "total_return" | "max_drawdown" | "volatility" | "sharpe" | "profit_factor" | "trades" | "fees";
  label: string;
  /** What is shown (a dash when the figure does not exist). */
  value: string;
  /** Present when a gain or a loss colours the figure; the sign is also in the text. */
  tone?: "up" | "down";
  /** The line under the value: its unit, or why it is unavailable. */
  sub?: string;
  available: boolean;
  lead?: boolean;
}

export function reportFigures(r: PerformanceReport): ReportFigure[] {
  const tone = (n: number): "up" | "down" | undefined => (n > 0 ? "up" : n < 0 ? "down" : undefined);
  const returnTone = tone(r.total_return);
  return [
    { key: "total_return", label: "Total return", value: percent(r.total_return), ...(returnTone ? { tone: returnTone } : {}), sub: `ending ${money(r.ending_equity)}`, available: true, lead: true },
    { key: "max_drawdown", label: "Max drawdown", value: percent(r.max_drawdown), sub: "peak to trough", available: true },
    { key: "volatility", label: "Volatility", value: percent(r.volatility, 1, false), sub: "annualised", available: true },
    {
      key: "sharpe", label: "Sharpe", value: ratioText(r.sharpe), sub: r.sharpe !== undefined ? "from daily returns" : "needs 3+ sessions", available: r.sharpe !== undefined,
    },
    {
      key: "profit_factor", label: "Profit factor", value: r.profit_factor !== undefined ? ratioText(r.profit_factor) : "—", available: r.profit_factor !== undefined,
      sub: r.profit_factor_status === "no_losing_trades" ? "no losing trades" : r.profit_factor_status === "no_trades" ? "no closed trades" : "gross win / gross loss",
    },
    { key: "trades", label: "Trades", value: int(r.trade_count), sub: r.win_rate !== undefined ? `${percent(r.win_rate, 0, false)} winners` : "closed round trips", available: true },
    { key: "fees", label: "Fees", value: money(r.fees_paid), available: true },
  ];
}

/** "52%" or a dash with the reason it does not exist: a win rate needs a closed round trip. */
export function winRateText(r: PerformanceReport): { value: string; sub: string; available: boolean } {
  return r.win_rate !== undefined
    ? { value: percent(r.win_rate, 0, false), sub: `of ${int(r.trade_count)} closed round trips`, available: true }
    : { value: "—", sub: "no closed round trip yet", available: false };
}
