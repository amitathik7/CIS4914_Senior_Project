# Engine gateway: the console on the Kafka topics

The web console (`web/`) talks to the engine through the API in
[ADR 0006](adr/0006-web-frontend-api-contract.md). The engine gateway serves
that API from the engine's event bus topics ([EVENT_BUS.md](EVENT_BUS.md)), so
the console shows a real run instead of its in-browser demo engine.

```
trading_engine run --bus kafka ──▶ Kafka ──▶ engine gateway ──▶ /api (Vite proxy) ──▶ console
     <prefix>.market_data / .signal / .order / .fill      127.0.0.1:8080
```

It is a small Python program (`python/engine_gateway/`) like the Strategy Lab
gateway. It is **read-only**: it never sits on the trading path, and a slow
browser cannot slow the pipeline down (ADR 0006 section 1).

## Running it

```sh
docker compose -f docker-compose.kafka.yml up -d                       # broker on localhost:9092
pip install -r python/engine_gateway/requirements.txt                  # confluent-kafka (librdkafka)
python python/engine_gateway/serve.py                                  # or: cd web && pnpm engine
cd web && VITE_ENGINE_API=/api pnpm dev                                # http://127.0.0.1:5173
./build/bin/trading_engine run --bars tests/fixtures/pipeline/demo_bars.csv --bus kafka --topic-prefix te.demo
```

The order does not matter. The gateway reads every topic from the beginning,
so a run that finished before it started still shows in full, and a run that
starts later appears within a few seconds.

Without a broker, replay a recording (standard library only):

```sh
python python/engine_gateway/serve.py --replay tests/fixtures/engine_gateway/demo_run.jsonl --pace 0.05
```

| Flag | Meaning |
|---|---|
| `--brokers HOST:PORT` | Kafka bootstrap servers (default `localhost:9092`) |
| `--topic-prefix NAME` | only this run (repeatable). Default: every `<prefix>.<type>` topic on the cluster |
| `--record FILE` | also append every message to FILE, one JSON line each |
| `--replay FILE` | read a recording instead of Kafka; `--pace SECONDS` spaces the messages out |
| `--starting-cash N` | the run's starting cash (default 100000, the engine's `PipelineConfig` default) |
| `--idle-seconds N` | a run quiet for this long shows as completed (default 10) |
| `--port N` | default 8080, where `web/vite.config.ts` proxies `/api` |

## What the console shows

One topic prefix is one run, named after the prefix. On Windows, run the same
commands from PowerShell with `python` and `.\build-kafka\bin\Release\trading_engine.exe`.

| Console | From | Today |
|---|---|---|
| Run list, run header, clock | market data times, strategy ids on signals | real |
| Price chart | `market_data` bars (a close-only bar is a flat candle) | real |
| Signals (Orders page) | `signal` | real; their time is `created_at`, which `trading_engine run` stamps from the wall clock |
| Orders, order trace | `order`: every status change is one message, so the history is the sequence of them | empty until the execution simulator publishes |
| Fills, positions, cash, equity, drawdown, trades | `fill` and marks, with the Portfolio Manager's accounting | empty / flat until fills exist |
| Risk log | `<prefix>.risk_decision` if the Risk Manager publishes one (payload as `RiskDecision` in `contract.ts`) | empty |
| System page | message counts per topic; latency = gateway receive time minus `enqueued_at` (bus transit, not stage processing time; none for a backlog read at start-up) | real |
| Report | computed when the run goes quiet, from the derived equity and trades with ADR 0006 section 5's definitions (stands in for `PerformanceAnalyzer`) | flat until fills exist |

The portfolio is **derived** from fills and marks with the same rules as
`PortfolioManager` (fees charged to realised P&L, average cost blended on adds,
a flip opens the remainder at the fill price, integer millionths throughout).
Cash reserved for an open buy is estimated as remaining quantity x limit price
(or the last mark), without fees.

The topics do not carry the run's configuration, so a run's detail reports the
engine's default risk limits and simulation settings (`config::default_config()`)
and the starting cash from `--starting-cash`. Starting a backtest, stopping a
run and the kill switch answer 501 with a message, because the engine has no
command topic yet.

## Endpoints

All of ADR 0006 section 2, under `/v1` (or `/api/v1`). `GET /v1/engine` adds a
`gateway` member: its version, the source state (`connecting`, `connected`,
`replayed`, `failed`), the count of messages it could not use and the last such
error. Bound to 127.0.0.1; the Host header must be a loopback name and a browser
`Origin` must be the console's own (add more with `--allow-origin`).

## Tests

```sh
cd python/engine_gateway && python -m unittest discover -s tests
TE_KAFKA_BOOTSTRAP=localhost:9092 python -m unittest discover -s tests   # adds the real-broker test
```

The checked-in recording `tests/fixtures/engine_gateway/demo_run.jsonl` is a
real `trading_engine run --bus kafka` of `tests/fixtures/pipeline/demo_bars.csv`
read through a broker with `--record`.
