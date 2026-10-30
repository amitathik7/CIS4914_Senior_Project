# Event bus and Kafka

How the components talk to each other. Decision record:
[ADR 0003](adr/0003-kafka-transport.md). Contract:
[`event_bus.hpp`](../include/trading_engine/events/event_bus.hpp).

```
                       ┌──────────── IEventBus ────────────┐
 MarketDataService ──▶ │ InProcessEventBus   (in memory)   │ ──▶ StrategyEngine
 StrategyEngine    ──▶ │        or                         │ ──▶ RiskManager ─(approved)─▶ ExecutionSimulator
 RiskManager       ──▶ │ TopicEventBus ─▶ IMessageTransport│ ──▶ PortfolioBusBridge ─▶ PortfolioManager
 ExecutionSimulator──▶ │                                   │ ──▶ (web gateway)
                       └───────────────────│───────────────┘
                                           ├─ RdKafkaTransport  ── Kafka cluster
                                           └─ InMemoryTransport ── InMemoryBroker (tests)
```

Components only know `IEventBus`. Which bus runs underneath is chosen by
whoever composes the program (`app::Pipeline` takes the bus as a parameter).

## What is connected today

| Component | Subscribes to | Publishes |
|---|---|---|
| `MarketDataService` | - (sources call `on_market_event`) | `MarketData` |
| `StrategyEngine` + both reference strategies | `MarketData` | `Signal` |
| `RiskManager` | `Signal`, `MarketData` (reference prices) | `RiskDecision`; approved signals go to execution by a direct call. Kill switch: `set_kill_switch()` |
| `ExecutionSimulator` | `MarketData` (fills working orders) | `Order` (every status change), `Fill` |
| `PortfolioManager` (via `PortfolioBusBridge`) | `MarketData` (marks), `Order`, `Fill` | - |
| Engine gateway (`python/engine_gateway/`, [ENGINE_GATEWAY.md](ENGINE_GATEWAY.md)) | every run topic and `<prefix>.run`, every prefix; `<control>.engine` | `<control>.command` (serves the web console) |
| `EngineService` (`trading_engine serve`) | `<control>.command` | `<prefix>.run`, `<control>.engine`; each run on its own bus |

So a run goes bars → normalised market data → strategies → signals →
risk decisions → orders → fills → portfolio, all over the bus. The
risk → execution hand-off is deliberately not a bus hop, so an unapproved
signal can never reach execution.

`app::Pipeline` wires market data, strategies and the portfolio;
`app::OrderFlow` (`include/trading_engine/app/order_flow.hpp`) adds risk
(the five starter policies plus the portfolio's buying-power / no-short
check) and execution (`NaiveFillModel`: a market order fills at the open of
the next market event for its symbol, with `slippage_bps` and fees).
`OrderFlow::start()` must run before `Pipeline::start()`, so execution sees
each bar before the portfolio marks it. `app::Backtest` composes the same
components itself and drives execution directly, bar by bar; `OrderFlow`
gives the same fills and final portfolio (tested in `order_flow_test.cpp`).

## Running it

```sh
cmake -S . -B build -DTRADING_ENGINE_WITH_KAFKA=ON      # OFF (default) = no Kafka needed
cmake --build build -j
./build/bin/trading_engine run --bars tests/fixtures/pipeline/demo_bars.csv --bus kafka-mock
```

`--bus` choices:

| Value | What runs | Needs |
|---|---|---|
| `inprocess` (default) | `InProcessEventBus` | nothing |
| `topics` | `TopicEventBus` over the in-memory broker (the Kafka code path, no Kafka) | nothing |
| `kafka-mock` | `TopicEventBus` over librdkafka's in-process mock cluster (real Kafka protocol) | Kafka build |
| `kafka` | `TopicEventBus` over a real cluster at `--brokers` (default `localhost:9092`) | Kafka build + broker |

`trading_engine serve --bars FILE` (Kafka build) is the engine as a
long-running process that the web console drives: it takes `start_backtest`,
`stop` and `set_kill_switch` commands on `trading_engine.control.command` and
replays the bars for each backtest on its own topic prefix
([ADR 0007](adr/0007-engine-command-channel.md),
[ENGINE_GATEWAY.md](ENGINE_GATEWAY.md)). Both programs build a run the same
way (`app::ConfiguredRun` from `control::RunSettings`).

Other flags: `--topic-prefix`, `--sma SHORT,LONG`, `--lookback N`,
`--quantity N`, `--cash N` (starting dollars), `--risk on|off` (the starter
policies; buying power and no-short are always checked). The CSV layout is the Strategy Lab's
(`symbol,exchange_time,type,price[,open,high,low,volume]`).

The run replays the bars in market time: the clock is set to each bar's
timestamp and the bus is drained before the next one, so a market order
placed on one bar fills at the next bar's open and every `--bus` gives the
same fills. It prints each fill and each risk rejection or resize, then a
summary:

```
risk          approved 18, resized 0, rejected 0
execution     orders 36 events, 0 rejected, fills 18, still open 0
portfolio     marks 180, fills applied 18, errors 0
              cash 99424.836926, equity 100022.416926
```

Try `--cash 300` (buying-power rejections) or `--quantity 2000` (position
limit rejections).

### A local Kafka broker

`docker compose -f docker-compose.kafka.yml up -d` starts a single-node KRaft
broker on `localhost:9092` (no ZooKeeper). Then:

```sh
./build/bin/trading_engine run --bars tests/fixtures/pipeline/demo_bars.csv --bus kafka --topic-prefix te.demo
# watch the raw messages from another terminal:
docker exec -it te-kafka /opt/kafka/bin/kafka-console-consumer.sh \
  --bootstrap-server localhost:9092 --topic te.demo.signal --from-beginning
TE_KAFKA_BOOTSTRAP=localhost:9092 ctest --test-dir build -L kafka   # also runs the RealBroker test
```

### Installing librdkafka

- Windows (MSVC): `vcpkg install librdkafka:x64-windows`, then configure with
  `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
  -DTRADING_ENGINE_WITH_KAFKA=ON`. CMake finds `RdKafka::rdkafka`.
- Ubuntu/Debian: `sudo apt install librdkafka-dev` (found via pkg-config).
- macOS: `brew install librdkafka`.

## Topics and messages

| Topic | Payload | Key |
|---|---|---|
| `<prefix>.market_data` | `domain::MarketEvent` | symbol |
| `<prefix>.signal` | `domain::TradeSignal` | symbol |
| `<prefix>.order` | `domain::Order` | symbol |
| `<prefix>.fill` | `domain::Fill` | symbol |
| `<prefix>.risk_decision` | `domain::RiskDecisionReport` | symbol |
| `<prefix>.run` | `control::RunStatusReport`: status, settings, kill switch ([ADR 0007](adr/0007-engine-command-channel.md)) | run id |
| `<control>.command` | `control::Command` (start_backtest, stop, set_kill_switch) | run id |
| `<control>.engine` | `control::EngineAnnouncement` (heartbeat) | engine id |

Default prefix `trading_engine`; use one prefix per run. `<control>` is
`trading_engine.control`. The last three are not bus `EventType`s: they are
about runs, not inside one (`control/control_codec.hpp`, same envelope). Topics the transport
creates get 3 partitions and replication factor 1 (`KafkaTransportConfig`).

### Wire format (schema 1)

```json
{"schema":1,"origin":"9f2c4b1a0e7d3c55","sequence":42,"type":"signal",
 "enqueued_at":"2026-01-05T14:31:00.000000000Z",
 "payload":{"id":3,"strategy_id":"sma_crossover","symbol":"AAPL","side":"buy",
            "created_at":"2026-01-05T14:31:00.000000000Z","requested_quantity":1,
            "metadata":{"trigger":"short_crossed_above_long","short_sma":"190.5"},
            "order_type":"market"}}
```

- `origin`: id of the publishing bus instance; `sequence` is monotonic per origin.
- Prices/money: exact decimal strings (`"150.02"`, may be negative for
  slippage). Quantities and ids: integers. Times: RFC 3339 UTC, nine
  fractional digits. Absent optionals are omitted. Enum spellings are the
  `to_string()` values in `src/domain/domain.cpp`.
- Field names are the C++ member names of the payload struct
  (`include/trading_engine/domain/`).
- A `risk_decision` payload follows the console's `RiskDecision`
  (`web/src/api/contract.ts`), plus `run_id` (here with the five passing
  policy checks shortened to one):
  ```json
  {"id":5,"run_id":1,"signal_id":5,"decided_at":"2026-01-05T15:04:00.000000000Z",
   "symbol":"AAPL","side":"sell","strategy_id":"mean_reversion","outcome":"rejected",
   "requested_quantity":1,"approved_quantity":0,"reason":"buying_power",
   "checks":[{"policy":"max_order_quantity","passed":true,"detail":"ok"},
             {"policy":"buying_power","passed":false,
              "detail":"not enough shares held to sell 1 (no short selling)"}]}
  ```
  `outcome` is `approved|resized|rejected`; `reason` is the deciding policy
  (absent when approved unchanged); `checks` lists the policies in the order
  they ran, stopping at the first rejection. `limit`/`observed` on a check
  and `order_id` are reserved and not set yet: join a decision to its order
  with `Order.origin_signal == signal_id`.
- An order is published again on every status change (same `id`,
  `updated_at` set): `working`, then `partially_filled`/`filled`, or
  `rejected` with `reject_reason`. Each `Fill` is published just before the
  order update it caused; `filled_quantity` is signed (+buy / -sell).
- Consumers must ignore members they do not know; `schema` changes on any
  incompatible change. Encoder/decoder: `events/event_codec.hpp`.

## Guarantees and limits

- Handlers run on one dispatcher thread per bus; `unsubscribe()` is a barrier.
- Order is guaranteed **per symbol**. Across symbols or event types it is not
  (with more than one partition). Deterministic, byte-identical backtests
  should use `inprocess`.
- Every bus instance sees every event on its topics (no consumer groups).
- `publish()` returns `false` when refused (not running, full, transport
  refused); a strategy signal refused this way is counted and lost, as
  `StrategyEngine` documents.

## Tests

- `ctest -L events`: both buses, the codec, the portfolio bridge, the
  pipeline producing identical signals on the recording bus, the in-process
  bus and the topic bus, and `OrderFlow` producing identical fills on both
  buses and matching `app::Backtest`.
- `ctest -L kafka` (Kafka build): the adapter and the whole pipeline against
  librdkafka's mock cluster; plus a real broker when `TE_KAFKA_BOOTSTRAP` is set.

## Not done yet

- `TradingEngine::run_backtest()/run_live()` still throw; the CLI composes the
  pipeline itself (`app::ConfiguredRun`). `HistoricalReplay` is a stub.
- Execution has no `order_to_fill` latency, partial fills or cancels yet;
  `NaiveFillModel` fills the whole order at the next bar's open.
- No metrics wiring (`SystemMetrics`) on the buses yet; each bus exposes
  `stats()`.
