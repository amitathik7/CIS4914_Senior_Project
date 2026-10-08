import {
  AreaSeries, BaselineSeries, CandlestickSeries, ColorType, CrosshairMode, HistogramSeries, LineSeries, LineStyle,
  createChart, createSeriesMarkers, type IChartApi, type ISeriesApi, type ISeriesMarkersPluginApi, type SeriesMarker,
  type SeriesType, type Time, type UTCTimestamp,
} from "lightweight-charts";
import { useEffect, useRef } from "react";
import { useTheme } from "../../state/theme";

// Canvas time-series chart (TradingView lightweight-charts) themed from the CSS tokens.
// The library has no time zones, so times are shifted to exchange time (ET) before plotting.

const offsetCache = new Map<string, number>();
const tzParts = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", timeZoneName: "shortOffset" });

function etOffsetSeconds(ms: number): number {
  const key = new Date(ms).toISOString().slice(0, 13);
  let offset = offsetCache.get(key);
  if (offset === undefined) {
    const name = tzParts.formatToParts(new Date(ms)).find((p) => p.type === "timeZoneName")?.value ?? "GMT-5";
    const m = /GMT([+-]\d+)/.exec(name);
    offset = (m ? Number(m[1]) : -5) * 3600;
    offsetCache.set(key, offset);
  }
  return offset;
}

export function chartTime(iso: string): UTCTimestamp {
  const ms = Date.parse(iso);
  return (Math.floor(ms / 1000) + etOffsetSeconds(ms)) as UTCTimestamp;
}

export function fromChartTime(time: Time): string {
  const t = time as number;
  const ms = t * 1000;
  return new Date(ms - etOffsetSeconds(ms) * 1000).toISOString();
}

const css = (name: string) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

export type Tone = "line" | "accent" | "up" | "down" | "warn" | "muted";
const TONE_VAR: Record<Tone, string> = {
  line: "--chart-line",
  accent: "--accent",
  up: "--up",
  down: "--down",
  warn: "--warn",
  muted: "--text-3",
};

export interface LinePoint {
  time: UTCTimestamp;
  value: number;
}

export interface CandlePoint {
  time: UTCTimestamp;
  open: number;
  high: number;
  low: number;
  close: number;
}

export type SeriesSpec =
  | { id: string; kind: "line"; data: LinePoint[]; tone?: Tone; dashed?: boolean; width?: 1 | 2; integer?: boolean }
  | { id: string; kind: "area"; data: LinePoint[]; tone?: Tone }
  | { id: string; kind: "baseline"; data: LinePoint[]; base: number }
  | { id: string; kind: "histogram"; data: LinePoint[]; tone?: Tone }
  | { id: string; kind: "candles"; data: CandlePoint[] };

export interface MarkerSpec {
  time: UTCTimestamp;
  side: "buy" | "sell";
  text?: string;
}

interface Props {
  series: SeriesSpec[];
  markers?: { seriesId: string; items: MarkerSpec[] };
  height: number;
  format?: (value: number) => string;
  onHover?: (time: Time | null) => void;
  label: string;
  initialBars?: number; // show the last N points rather than everything
  seconds?: boolean;
}

export function TimeChart({ series, markers, height, format, onHover, label, initialBars, seconds }: Props) {
  const host = useRef<HTMLDivElement>(null);
  const chart = useRef<IChartApi | null>(null);
  const apis = useRef(new Map<string, ISeriesApi<SeriesType>>());
  const markerApi = useRef<ISeriesMarkersPluginApi<Time> | null>(null);
  const fitted = useRef(false);
  const hover = useRef(onHover);
  hover.current = onHover;
  const theme = useTheme();
  const shape = series.map((s) => `${s.id}:${s.kind}`).join(",");

  // Create the chart and its series when the series set changes.
  useEffect(() => {
    if (!host.current) return;
    const c = createChart(host.current, {
      height,
      autoSize: true,
      layout: { background: { type: ColorType.Solid, color: "transparent" }, attributionLogo: false, fontFamily: "IBM Plex Sans, system-ui, sans-serif", fontSize: 11 },
      crosshair: { mode: CrosshairMode.Magnet },
      rightPriceScale: { borderVisible: false, scaleMargins: { top: 0.12, bottom: 0.08 } },
      timeScale: { borderVisible: false, timeVisible: true, secondsVisible: !!seconds, rightOffset: 2, minBarSpacing: 0.02, lockVisibleTimeRangeOnResize: true },
      handleScale: { axisPressedMouseMove: { time: true, price: false } },
    });
    chart.current = c;
    for (const s of series) {
      const common = { priceLineVisible: false, lastValueVisible: s.kind !== "histogram" };
      const api =
        s.kind === "candles" ? c.addSeries(CandlestickSeries, common)
        : s.kind === "area" ? c.addSeries(AreaSeries, { ...common, lineWidth: 2 })
        : s.kind === "baseline" ? c.addSeries(BaselineSeries, { ...common, lineWidth: 2, baseValue: { type: "price", price: s.base } })
        : s.kind === "histogram" ? c.addSeries(HistogramSeries, { ...common, priceScaleId: "" })
        : c.addSeries(LineSeries, {
            ...common,
            lineWidth: s.width ?? 2,
            lineStyle: s.dashed ? LineStyle.Dashed : LineStyle.Solid,
            crosshairMarkerVisible: !s.dashed,
            ...(s.integer ? { priceFormat: { type: "price" as const, precision: 0, minMove: 1 } } : {}),
          });
      apis.current.set(s.id, api as ISeriesApi<SeriesType>);
    }
    c.subscribeCrosshairMove((p) => hover.current?.(p.time ?? null));
    fitted.current = false;
    return () => {
      markerApi.current = null;
      apis.current.clear();
      c.remove();
      chart.current = null;
    };
  }, [shape, height]);

  // Theme: colours come from CSS tokens, so re-read them when the theme flips.
  useEffect(() => {
    const c = chart.current;
    if (!c) return;
    const text = css("--text-3");
    const grid = css("--chart-grid");
    c.applyOptions({
      layout: { textColor: text },
      grid: { vertLines: { color: "transparent" }, horzLines: { color: grid } },
      crosshair: {
        vertLine: { color: css("--line-strong"), labelBackgroundColor: css("--surface-2"), width: 1, style: LineStyle.Solid },
        horzLine: { color: css("--line-strong"), labelBackgroundColor: css("--surface-2") },
      },
      localization: format ? { priceFormatter: format } : {},
    });
    for (const s of series) {
      const api = apis.current.get(s.id);
      if (!api) continue;
      if (s.kind === "candles") {
        const up = css("--up");
        const down = css("--down");
        // Down candles are hollow so direction never rests on red/green alone.
        api.applyOptions({ upColor: up, downColor: css("--surface"), borderUpColor: up, borderDownColor: down, wickUpColor: up, wickDownColor: down });
      } else if (s.kind === "baseline") {
        const up = css("--up");
        const down = css("--down");
        api.applyOptions({
          topLineColor: up, bottomLineColor: down,
          topFillColor1: css("--up-soft"), topFillColor2: "transparent",
          bottomFillColor1: "transparent", bottomFillColor2: css("--down-soft"),
        });
      } else if (s.kind === "area") {
        const color = css(TONE_VAR[s.tone ?? "line"]);
        api.applyOptions({ lineColor: color, topColor: css("--chart-fill"), bottomColor: "transparent" });
      } else {
        api.applyOptions({ color: css(TONE_VAR[s.tone ?? "line"]) });
      }
    }
  }, [theme, shape, format]);

  // Data.
  useEffect(() => {
    const c = chart.current;
    if (!c) return;
    for (const s of series) apis.current.get(s.id)?.setData(s.data as never);
    if (markers) {
      const target = apis.current.get(markers.seriesId);
      if (target) {
        const items: SeriesMarker<Time>[] = markers.items.map((m) => ({
          time: m.time,
          position: m.side === "buy" ? "belowBar" : "aboveBar",
          shape: m.side === "buy" ? "arrowUp" : "arrowDown",
          color: css(m.side === "buy" ? "--up" : "--down"),
          text: m.text,
          size: 1,
        }));
        if (!markerApi.current) markerApi.current = createSeriesMarkers(target, items);
        else markerApi.current.setMarkers(items);
      }
    }
    const points = series[0]?.data.length ?? 0;
    if (!fitted.current && points > 0) {
      fitted.current = true;
      if (initialBars && points > initialBars) c.timeScale().setVisibleLogicalRange({ from: points - initialBars, to: points + 2 });
      else c.timeScale().fitContent();
    }
  });

  return <div ref={host} className="chart" style={{ height }} role="img" aria-label={label} />;
}
