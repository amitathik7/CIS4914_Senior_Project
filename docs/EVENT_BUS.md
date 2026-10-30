# Event bus and Kafka

How the components talk to each other. Decision record:
[ADR 0003](adr/0003-kafka-transport.md). Contract:
[`event_bus.hpp`](../include/trading_engine/events/event_bus.hpp).

```
                       ┌──────────── IEventBus ────────────┐
 MarketDataService ──▶ │ InProcessEventBus   (in memory)   │ ──▶ StrategyEngine
 StrategyEngine    ──▶ │        or                         │ ──▶ PortfolioBusBridge ─▶ PortfolioManager
 (Risk, Execution) ──▶ │ TopicEventBus ─▶ IMessageTransport│ ──▶ (Risk, Execution, gateway)
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
| `PortfolioManager` (via `PortfolioBusBridge`) | `MarketData` (marks), `Order`, `Fill` | - |
| `RiskManager`, `ExecutionSimulator` | *stubs* | *stubs* |
| Engine gateway (`python/engine_gateway/`, [ENGINE_GATEWAY.md](ENGINE_GATEWAY.md)) | all four topics, every prefix | - (serves the web console) |

So a run today goes bars → normalised market data → strategies → signals on
the bus, with the portfolio marking prices. When risk and execution land,
they subscribe to `Signal` and publish `Order` / `Fill`, and the portfolio
starts moving with no further wiring.

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

Other flags: `--topic-prefix`, `--sma SHORT,LONG`, `--lookback N`,
`--quantity N`. The CSV layout is the Strategy Lab's
(`symbol,exchange_time,type,price[,open,high,low,volume]`).

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

Default prefix `trading_engine`; use one prefix per run. Topics the transport
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

- `ctest -L events`: both buses, the codec, the portfolio bridge, and the
  pipeline producing identical signals on the recording bus, the in-process
  bus and the topic bus.
- `ctest -L kafka` (Kafka build): the adapter and the whole pipeline against
  librdkafka's mock cluster; plus a real broker when `TE_KAFKA_BOOTSTRAP` is set.

## Not done yet

- `TradingEngine::run_backtest()/run_live()` still throw; the CLI composes the
  pipeline itself. `HistoricalReplay`, risk, execution and analytics are stubs.
- No metrics wiring (`SystemMetrics`) on the buses yet; each bus exposes
  `stats()`.
