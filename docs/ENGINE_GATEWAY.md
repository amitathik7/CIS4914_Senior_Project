# Engine gateway: the console on the Kafka topics

The web console (`web/`) talks to the engine through the API in
[ADR 0006](adr/0006-web-frontend-api-contract.md). The engine gateway serves
that API from the engine's event bus topics ([EVENT_BUS.md](EVENT_BUS.md)), so
the console shows a real run instead of its in-browser demo engine.

```
trading_engine run --bus kafka ──▶ Kafka ──▶ engine gateway ──▶ /api (Vite proxy) ──▶ console
trading_engine serve          ──▶   <prefix>.market_data / .signal / .order / .fill / .risk_decision / .run
                              ◀──   trading_engine.control.command  ◀── (New backtest, Stop, kill switch)
                              ──▶   trading_engine.control.engine   (heartbeat: the data it can replay)
```

It is a small Python program (`python/engine_gateway/`) like the Strategy Lab
gateway. It never sits on the trading path, and a slow browser cannot slow the
pipeline down (ADR 0006 section 1): it reads the topics, and the console's
three write actions become one command message each on the command channel
([ADR 0007](adr/0007-engine-command-channel.md)).

## Running it

```sh
docker compose -f docker-compose.kafka.yml up -d                       # broker on localhost:9092
pip install -r python/engine_gateway/requirements.txt                  # confluent-kafka (librdkafka)
python python/engine_gateway/serve.py                                  # or: cd web && pnpm engine
cd web && VITE_ENGINE_API=/api pnpm dev                                # http://127.0.0.1:5173
./build/bin/trading_engine serve --bars tests/fixtures/pipeline/demo_bars.csv --pace-ms 50
```

Then use **New backtest** in the console. `trading_engine serve` (Kafka build)
replays the bars it was given for each backtest, one at a time; `--pace-ms`
pauses after each bar time so a run can be watched (and its kill switch
used). Its other flags: `--bus kafka-mock`, `--brokers`, `--control-prefix`,
`--engine-id`, `--heartbeat-ms`.

A one-off run from the command line shows up too, read-only:

```sh
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
| `--control-prefix NAME` | the engines' command and heartbeat topics (default `trading_engine.control`) |
| `--read-only` | send no commands: New backtest, Stop and the kill switch answer 501 |
| `--starting-cash N` | starting cash for a run with no `<prefix>.run` (an older engine; default 100000) |
| `--idle-seconds N` | a run with no `<prefix>.run`, quiet for this long, shows as completed (default 10) |
| `--port N` | default 8080, where `web/vite.config.ts` proxies `/api` |

## What the console shows

One topic prefix is one run, named after the prefix. On Windows, run the same
commands from PowerShell with `python` and `.\build-kafka\bin\Release\trading_engine.exe`.

| Console | From | Today |
|---|---|---|
| Run list, run header, status, progress | `<prefix>.run` (status, progress, market clock), else market data times | real |
| Run settings: risk limits, strategy parameters, starting cash, fees | `config` on `<prefix>.run` | real (defaults only for a run without one) |
| Kill switch (Risk page) | `kill_switch` on `<prefix>.run`; changed with a command | real for runs started from the console |
| Price chart | `market_data` bars (a close-only bar is a flat candle) | real |
| Signals (Orders page) | `signal` | real; their time is `created_at`, which `trading_engine run` stamps from the wall clock |
| Orders, order trace | `order`: every status change is one message, so the history is the sequence of them | real |
| Fills, positions, cash, equity, drawdown, trades | `fill` and marks, with the Portfolio Manager's accounting, from the run's starting cash | real |
| Risk log | `<prefix>.risk_decision` (payload as `RiskDecision` in `contract.ts`); `kill_switch` rejections while engaged | real |
| System page | message counts per topic; latency = gateway receive time minus `enqueued_at` (bus transit, not stage processing time; none for a backlog read at start-up) | real |
| Report | computed when the run completes or is stopped, from the derived equity and trades with ADR 0006 section 5's definitions (stands in for `PerformanceAnalyzer`) | real |

The portfolio is **derived** from fills and marks with the same rules as
`PortfolioManager` (fees charged to realised P&L, average cost blended on adds,
a flip opens the remainder at the fill price, integer millionths throughout).
Cash reserved for an open buy is estimated as remaining quantity x limit price
(or the last mark), without fees.

### Commands

| Console | Endpoint | What the gateway does |
|---|---|---|
| New backtest | `POST /v1/runs` | 503 unless an engine heartbeat arrived lately; picks a run id (`bt-<time>-<hex>`), sends `start_backtest` with the form as the run's settings, waits up to 10 s for the engine's answer. 201 with the queued (or running) run, or 400 with the engine's refusal and the form `field` it names (`strategies.0.long_window`), or 504 |
| Stop | `POST /v1/runs/{id}/stop` | sends `stop`; a queued run ends at once, a running one after the current bar time |
| Kill switch | `PUT /v1/runs/{id}/kill-switch` | sends `set_kill_switch`, returns the run detail once the engine confirms. While engaged the Risk Manager rejects every new signal (`kill_switch`) |

Stop and the kill switch answer 409 for a run started with `trading_engine
run` (nothing listens for its commands) and for a run whose engine stopped
heartbeating; such a queued or running run shows as failed. A start the engine
refused is not listed among the runs. With `--replay` or `--read-only` all
three answer 501. `GET /v1/engine`'s `market_data` lists what the live engines
can replay, so the New backtest form offers only recorded symbols and dates.

## Endpoints

All of ADR 0006 section 2, under `/v1` (or `/api/v1`). `GET /v1/engine` adds a
`gateway` member: its version, the source state (`connecting`, `connected`,
`replayed`, `failed`), whether it sends commands, the live engines, the count
of messages it could not use and the last such error. Bound to 127.0.0.1; the Host header must be a loopback name and a browser
`Origin` must be the console's own (add more with `--allow-origin`).

## Tests

```sh
cd python/engine_gateway && python -m unittest discover -s tests
TE_KAFKA_BOOTSTRAP=localhost:9092 python -m unittest discover -s tests   # adds the real-broker test
```

`tests/test_commands.py` drives the write endpoints against a fake engine that
answers like `trading_engine serve`; the engine side is tested in C++
(`tests/unit/control_test.cpp`, over the in-memory broker).

The checked-in recording `tests/fixtures/engine_gateway/demo_run.jsonl` is a
real `trading_engine run --bus kafka` of `tests/fixtures/pipeline/demo_bars.csv`
read through a broker with `--record`.
