// Plain-language copy for the Strategies section: friendly names, units and short explanations. This is presentation only.
// Names, types, defaults, constraints and reason texts come from the replay tool's catalog; where a strategy or parameter is
// not listed here the UI falls back to the catalog's own wording, so a new parameter still appears (just less polished).

export interface ParamCopy {
  label: string;
  unit?: string;
  hint: string;
  advanced?: boolean;
}

export const PARAM_COPY: Record<string, ParamCopy> = {
  short_window: { label: "Short window", unit: "bars", hint: "Bars in the faster average." },
  long_window: { label: "Long window", unit: "bars", hint: "Bars in the slower average. Nothing is decided until it is full." },
  lookback: { label: "Lookback", unit: "bars", hint: "Bars in the rolling mean and spread, the current bar included." },
  entry_threshold: { label: "Entry threshold", unit: "z-score", hint: "A request is made when the z-score reaches this far from zero." },
  rearm_threshold: { label: "Re-arm threshold", unit: "z-score", hint: "After a request, the strategy asks again only once |z| falls back to this or lower." },
  requested_quantity: { label: "Shares per request", hint: "Whole shares carried by every Buy or Sell request." },
  symbols: { label: "Symbols", hint: "Allowlist, comma-separated. Matched exactly, so case matters." },
  strategy_id: { label: "Strategy id", hint: "Names this strategy's requests. Must be unique within a run.", advanced: true },
};

export const paramCopy = (name: string): ParamCopy => PARAM_COPY[name] ?? { label: name, hint: "" };

export interface StrategyCopy {
  short: string;
  rules: string[];
}

export const STRATEGY_COPY: Record<string, StrategyCopy> = {
  sma_crossover: {
    short: "Compares a fast and a slow moving average of the close.",
    rules: [
      "For each allowed symbol it keeps two simple moving averages of the close: a short one and a long one. Nothing is decided until the long window is full (warm-up).",
      "When the short average moves above the long one it makes a Buy request; when it moves below, a Sell request.",
      "The first bar after warm-up where the averages differ only sets a baseline and never makes a request.",
      "Equal averages change nothing.",
    ],
  },
  mean_reversion: {
    short: "Measures how far each close is from its recent mean, in standard deviations.",
    rules: [
      "For each allowed symbol it keeps the last Lookback closes and computes the z-score of the newest close against their mean. Nothing is decided until the window is full (warm-up).",
      "A z-score at or below the negative entry threshold makes a Buy request; at or above the positive threshold, a Sell request.",
      "It asks once per excursion: it re-arms when |z| falls to the re-arm threshold or lower, or when the window goes flat.",
      "A flat window (no spread) has no z-score; it is shown as unavailable, never as zero.",
    ],
  },
};

/** Said wherever Buy/Sell requests appear, so no one reads them as orders, trades or positions. */
export const REQUEST_CAVEATS = [
  "A Buy or Sell is a request from the strategy to later stages, not an order or a trade. The strategy tracks no position and sizes nothing, so a Sell request does not necessarily close anything.",
  "Nothing here promises an order is placed or filled: whether a request becomes an order is decided by risk and execution, which a signal replay does not run.",
  "A bar that produces no request is silent. That is not a Hold signal.",
];

export const SIGNAL_ONLY_NOTICE =
  "Signal replay only: it shows what the real strategy decided on each recorded row. It does not run risk or execution, so there are no orders, fills, positions or performance figures, and no Orders or Risk records to link to.";

/** The tool's reasons a value can be unavailable (docs/STRATEGY_LAB.md section 9), in words. */
export const UNAVAILABLE_TEXT: Record<string, string> = {
  warming_up: "Not enough bars yet: the window is not full.",
  event_ignored: "This row did not change the strategy's state, so nothing was computed.",
  constant_window: "The window is flat (no spread), so a z-score is not meaningful.",
  measurement_failed: "The statistic could not be measured (it came out non-finite).",
  non_finite: "The value was not a finite number, so it is not shown.",
};

export const unavailableText = (reason: string): string => UNAVAILABLE_TEXT[reason] ?? reason;

export const VERDICT_LABEL: Record<string, string> = { evaluated: "Evaluated", warming_up: "Warming up", ignored: "Ignored" };
export const verdictLabel = (verdict: string | undefined): string => (verdict ? VERDICT_LABEL[verdict] ?? verdict : "Unavailable");
