# Performance

Baseline numbers for the engine as it stands, and how to reproduce them. This
is the "before" for M8 (`docs/IMPLEMENTATION_PLAN.md`): any optimisation should
be measured against this table with the same command.

## Run it

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --target trading_engine_bench
./build-release/bin/trading_engine_bench
```

The first line says whether the build is optimised; Debug numbers are 5-10x
worse and not worth recording. Options: `--bars N` (minutes per symbol, default
2000), `--symbols N` (default 5), `--repeat N` (default 5), `--events N`
(burst and codec count, default 200000), `--pings N` (latency samples, default
20000), `--only replay|bus|codec`.

## What it measures

| Benchmark | What runs | Reported |
|---|---|---|
| `replay` | `app::ConfiguredRun` (the composition `trading_engine run` uses): both reference strategies, risk policies on, execution simulator, portfolio, over seeded random-walk minute bars, on each bus | wall time (median of `--repeat`), bars/s, µs per bar, signals and fills |
| `bus` | One market-data event published, wait until the handler has run, repeat (after a warm-up of 10%, at most 1000); then a burst published as fast as possible | publish→handler latency p50/p90/p99/max; burst throughput |
| `codec` | `events::encode` and `events::decode` of one market-data envelope | ns per call |
| peak memory | `getrusage` max RSS for the whole process (not on Windows) | MiB |

The bars come from a fixed seed (42), so every machine and commit replays the
same data; signals and fills must match between buses and between runs, which
doubles as a determinism check.

`inprocess` is `InProcessEventBus`. `topics` is `TopicEventBus` over the
in-memory broker: the same path as Kafka (JSON encode, broker, poll, decode)
without the network, with the 5 ms poll interval `trading_engine run` uses.

## Baseline (2026-11-13)

Apple M5 (10 cores), macOS 27, Apple clang 21, Release (`-O3`), on `main`
with PRs #24-#27 merged. Five runs of the default command; the range is shown
where runs differed by more than a few percent.

**Replay, 10,000 bars (5 symbols × 2,000 minutes), 1,018 signals, 977 fills**

| Bus | Wall time | Bars/s | µs per bar |
|---|---|---|---|
| inprocess | 13-14 ms | 707,000-757,000 | 1.3-1.4 |
| topics | 86-92 ms | 108,000-116,000 | 8.6-9.2 |

**Replay, 200,000 bars (10 symbols × 20,000 minutes), 21,160 signals, 20,900 fills**

| Bus | Wall time | Bars/s | µs per bar |
|---|---|---|---|
| inprocess | 171 ms | 1,170,000 | 0.85 |
| topics | 1.67-1.75 s | 114,000-120,000 | 8.3-8.7 |

**Bus, publish→handler latency (20,000 samples) and burst (200,000 events)**

| Bus | p50 | p90 | p99 | max | Burst |
|---|---|---|---|---|---|
| inprocess | 3.2-4.0 µs | 5.2-5.7 µs | 7.3-19.7 µs | 48-416 µs | 5.4-7.0 M events/s |
| topics | 7.3-7.4 µs | 8.1-8.9 µs | 17.6-21.8 µs | 101-227 µs | 287,000-311,000 events/s |

**Codec, one 331-byte market-data envelope**

| | Per call | Per second |
|---|---|---|
| encode | 1.23-1.27 µs | ~790,000-815,000 |
| decode | 1.21-1.29 µs | ~780,000-825,000 |

**Peak memory:** 15 MiB for the default run; 252 MiB for the 200,000-bar run.

## What the numbers say

- **The topic bus costs about 7 µs per bar over the in-process bus.** A bar
  makes several hops (market data, then signals, orders and fills for some),
  and each hop is an encode, a broker append, a poll and a decode. The codec
  alone is about 2.4 µs per hop, so JSON is roughly a third of the topic bus's
  overhead. A first optimisation candidate for M8.
- **The topic bus keeps every message.** `InMemoryBroker` is an append-only log,
  like Kafka, so a long run on `--bus topics` grows with its length (most of the
  252 MiB above). Fine for tests and demos; not a leak in the engine.
- **Tail latency is scheduling, not work.** p50 stays at 3-7 µs; the max
  moves 5x between identical runs, which is the OS waking the dispatcher
  thread. p99 is the number to compare.
- **A year of minute bars for a handful of symbols replays in well under a
  second** in process. Real Kafka adds network round trips that this does not
  measure.

## Targets

No targets are set yet (`docs/TECH_REQUIREMENTS.md` is not written). Starting
points for the team to argue about, from this baseline: in-process p99 under
50 µs, replay above 500,000 bars/s in process and above 100,000 bars/s on the
topic bus, peak memory under 100 MiB for a 10,000-bar run.

## Not measured yet

- Real Kafka (`--bus kafka`): needs a broker; the network will dominate.
- The Alpaca live path, which is limited by Alpaca (one bar per symbol per
  minute), not by the engine.
- Google Benchmark micro-benchmarks for single components, which the M8 plan
  lists; this harness has no dependencies and measures end to end instead.
