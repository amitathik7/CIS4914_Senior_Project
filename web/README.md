# Engine console

The web front end for the simulated trading engine: live and backtest runs,
portfolio, an order blotter with a signal-to-fill trace, risk decisions and
limits, performance reports, and engine telemetry.

React 19 + TypeScript + Vite. Charts are TradingView
[lightweight-charts](https://github.com/tradingview/lightweight-charts) for
time series and plain SVG for distributions. No CSS framework: tokens and
components live in `src/styles/global.css`.

## Run it

```bash
cd web
pnpm install
pnpm dev          # http://127.0.0.1:5173
```

With no configuration the console runs the **demo engine** in the browser (see
below) and shows a "Demo data" badge. To use a real engine API:

```bash
VITE_ENGINE_API=/api pnpm dev     # proxied to http://127.0.0.1:8080 (vite.config.ts)
```

Other scripts: `pnpm build` (type-check + production bundle in `dist/`),
`pnpm typecheck`, `pnpm test`.

## Layout

| Path | What |
|---|---|
| `src/api/contract.ts` | The wire contract. Explained in [ADR 0006](../docs/adr/0006-web-frontend-api-contract.md) |
| `src/api/client.ts` | `EngineClient` interface and the REST + SSE implementation |
| `src/mock/` | The demo engine: seeded market data, strategies, risk, fills, portfolio, telemetry |
| `src/state/` | Client context, per-run data hooks that refresh on stream events, theme |
| `src/components/` | Shell, pipeline strip, tables, charts, badges |
| `src/pages/` | Overview, Strategies (Configure, Explore, Compare, Advanced), Orders (and the order trace), Risk, Report, System, Backtests, New backtest |
| `src/strategies/`, `src/components/strategies/` | The Strategies section: replay of the real C++ strategies through the lab gateway, see below |
| `src/lib/losslessJson.ts`, `src/lib/exactTime.ts` | A JSON reader that keeps numbers as text, and exact (nanosecond) exchange-time display |
| `src/lib/decimal.ts` | Exact decimal text, the same representation as `common::Decimal` |

## Strategies (real engine)

The Strategies page configures one of the two reference strategies, replays recorded bars through the **real C++ strategy engine**
(not the demo engine), and **compares** two configurations (A and B) over one dataset. An Advanced area runs the Lab's scenario checks of the executable. A browser cannot start a native program, so it talks to a small local gateway:

```bash
python python/strategy_lab/lab_gateway.py     # or: pnpm lab   (needs the replay tool built, docs/STRATEGY_LAB.md section 4)
pnpm dev                                      # /lab is proxied to http://127.0.0.1:8765 (LAB_GATEWAY overrides)
```

Without the gateway the page says so at the Run button; everything else works. Compare is signal-only: financial figures need the Backtests pipeline (see the doc). What is real, what is a fixture and what is demo, the
numeric rules and the gateway contract are in [docs/STRATEGIES_CONSOLE.md](../docs/STRATEGIES_CONSOLE.md). Vite restarts a running dev
server when `vite.config.ts` changes; if `/lab` is still not proxied, restart it.

## The demo engine

`src/mock/` serves the whole contract without a backend, so the console can be
built and demonstrated before the engine's stages are wired together. It
follows the project's documented rules (the two reference strategies, the M6
risk policies, cash reservation per ADR 0004), and its accounting is tested:
equity minus starting cash equals realized plus unrealized P&L to the micro.

Its market data is synthetic and its latencies are invented. It is a stand-in,
not evidence that the engine works; the UI labels it as demo data everywhere it
appears. Backtests you start are kept in `localStorage` and replayed
deterministically on reload.

## Conventions

- Money and prices stay as exact decimal strings until they are formatted;
  `toNumber()` is only for chart coordinates.
- Times are UTC on the wire and shown in exchange time (ET).
- Gains and losses always carry a sign as well as a colour; down candles are
  hollow; risk outcomes have an icon and a label.
- Dark theme by default; the light theme is a full second palette, not an
  inversion. Both meet WCAG AA for text.
