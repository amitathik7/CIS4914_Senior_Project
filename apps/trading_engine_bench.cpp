// trading_engine_bench: throughput and latency baselines for docs/PERFORMANCE.md.
//
//   trading_engine_bench [--bars N] [--symbols N] [--repeat N] [--events N]
//                        [--pings N] [--only replay|bus|codec]
//
//   replay  a full backtest (both reference strategies, risk, execution,
//           portfolio) over seeded random-walk minute bars, on each bus
//   bus     publish->handler latency one event at a time (ping-pong), and
//           burst throughput, on each bus
//   codec   encode + decode of one market-data envelope (the topic bus's
//           per-hop serialisation cost)
//
// Same seed, same bars: runs are comparable across machines and commits.
// Numbers from a Debug build are not meaningful; the header says which it is.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "trading_engine/app/configured_run.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/control/run_settings.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/events/event_codec.hpp"
#include "trading_engine/events/message_transport.hpp"
#include "trading_engine/events/topic_event_bus.hpp"
#include "trading_engine/version.hpp"

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace {

using namespace trading_engine;   // NOLINT(build/namespaces) -- small main only
using Steady = std::chrono::steady_clock;

struct Options {
    std::size_t bars{2'000};      // per symbol
    std::size_t symbols{5};
    std::size_t repeat{5};
    std::size_t events{200'000};
    std::size_t pings{20'000};
    std::string only;
};

[[noreturn]] void usage(const std::string& message) {
    std::cerr << "trading_engine_bench: " << message
              << "\nusage: trading_engine_bench [--bars N] [--symbols N] [--repeat N] [--events N]"
                 " [--pings N] [--only replay|bus|codec]\n";
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; i += 2) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) {
            usage(flag + " needs a value");
        }
        const std::string value = argv[i + 1];
        const auto number = [&]() -> std::size_t {
            try {
                std::size_t used = 0;
                const auto n = std::stoull(value, &used);
                if (used == value.size() && n > 0) {
                    return static_cast<std::size_t>(n);
                }
            } catch (...) {
            }
            usage(flag + " needs a positive whole number");
        };
        if (flag == "--bars") {
            o.bars = number();
        } else if (flag == "--symbols") {
            o.symbols = number();
        } else if (flag == "--repeat") {
            o.repeat = number();
        } else if (flag == "--events") {
            o.events = number();
        } else if (flag == "--pings") {
            o.pings = number();
        } else if (flag == "--only") {
            if (value != "replay" && value != "bus" && value != "codec") {
                usage("--only needs replay, bus or codec");
            }
            o.only = value;
        } else {
            usage("unknown option " + flag);
        }
    }
    return o;
}

common::Price cents(double price) {
    return common::Price::from_micros(std::llround(price * 100.0) * 10'000);
}

domain::MarketEvent bar(const std::string& symbol, common::Timestamp t, double open, double close, std::uint64_t seq) {
    domain::MarketEvent e;
    e.symbol        = symbol;
    e.exchange_time = t;
    e.ingest_time   = t;
    e.type          = domain::MarketEventType::Bar;
    e.open          = cents(open);
    e.price         = cents(close);
    e.high          = cents(std::max(open, close) * 1.001);
    e.low           = cents(std::min(open, close) * 0.999);
    e.volume        = 1'000 + static_cast<common::Quantity>(seq % 997);
    e.sequence      = seq;
    return e;
}

// Seeded random-walk minute bars from 2024-01-02 14:30 UTC, interleaved by time.
std::vector<domain::MarketEvent> synthetic_bars(std::size_t per_symbol, std::size_t symbols) {
    std::mt19937_64 rng{42};
    std::normal_distribution<double> step{0.0, 0.002};
    std::vector<double> close(symbols, 100.0);
    const common::Timestamp start{std::chrono::seconds{1'704'205'800}};
    std::vector<domain::MarketEvent> out;
    out.reserve(per_symbol * symbols);
    for (std::size_t i = 0; i < per_symbol; ++i) {
        for (std::size_t s = 0; s < symbols; ++s) {
            const double open = close[s];
            close[s]          = open * std::exp(step(rng));
            out.push_back(bar("SYM" + std::to_string(s), start + std::chrono::minutes{i}, open, close[s], i + 1));
        }
    }
    return out;
}

// One bus to measure, with what it needs kept alive.
struct BusUnderTest {
    std::shared_ptr<events::InMemoryBroker>    broker;
    std::unique_ptr<events::IMessageTransport> transport;
    std::unique_ptr<events::IEventBus>         bus;

    BusUnderTest(const std::string& kind, const common::IClock& clock) {
        if (kind == "inprocess") {
            bus = std::make_unique<events::InProcessEventBus>(clock);
            return;
        }
        broker    = std::make_shared<events::InMemoryBroker>();
        transport = std::make_unique<events::InMemoryTransport>(broker);
        events::TopicEventBusOptions options;
        options.poll_interval = std::chrono::milliseconds{5};   // what trading_engine run uses
        bus = std::make_unique<events::TopicEventBus>(*transport, clock, options);
    }
    ~BusUnderTest() {
        bus.reset();
        transport.reset();
    }
    BusUnderTest(const BusUnderTest&)            = delete;
    BusUnderTest& operator=(const BusUnderTest&) = delete;
};

const std::vector<std::string> kBuses{"inprocess", "topics"};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto i = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(i, sorted.size() - 1)];
}

double seconds_since(Steady::time_point t0) { return std::chrono::duration<double>(Steady::now() - t0).count(); }

// --- replay ------------------------------------------------------------------

control::RunSettings settings_for(const std::vector<domain::MarketEvent>& bars, std::size_t symbols) {
    std::vector<common::Symbol> names;
    for (std::size_t s = 0; s < symbols; ++s) {
        names.push_back("SYM" + std::to_string(s));
    }
    control::RunSettings s;
    s.name         = "bench";
    s.period_start = bars.front().exchange_time;
    s.period_end   = bars.back().exchange_time;
    control::StrategySettings sma;
    sma.kind        = control::StrategyKind::SmaCrossover;
    sma.strategy_id = "sma_crossover";
    sma.symbols     = names;
    control::StrategySettings mr;
    mr.kind        = control::StrategyKind::MeanReversion;
    mr.strategy_id = "mean_reversion";
    mr.symbols     = names;
    s.strategies   = {sma, mr};
    return s;
}

void bench_replay(const Options& o) {
    const auto bars     = synthetic_bars(o.bars, o.symbols);
    const auto settings = settings_for(bars, o.symbols);
    std::printf("replay: %zu bars (%zu symbols x %zu minutes), both strategies, risk on, median of %zu\n",
                bars.size(), o.symbols, o.bars, o.repeat);
    std::printf("  %-10s %12s %14s %10s %8s %8s\n", "bus", "seconds", "bars/s", "us/bar", "signals", "fills");
    for (const auto& kind : kBuses) {
        std::vector<double> times;
        std::uint64_t signals = 0, fills = 0;
        for (std::size_t r = 0; r < o.repeat; ++r) {
            common::ManualClock clock;
            BusUnderTest b{kind, clock};
            std::atomic<std::uint64_t> sig{0}, fil{0};
            b.bus->subscribe(events::EventType::Signal, [&](const events::Event&) { ++sig; });
            b.bus->subscribe(events::EventType::Fill, [&](const events::Event&) { ++fil; });
            app::ConfiguredRun run{*b.bus, clock, settings};
            const auto t0 = Steady::now();
            (void)run.replay(bars);
            times.push_back(seconds_since(t0));
            signals = sig;
            fills   = fil;
        }
        const double t = median(times);
        std::printf("  %-10s %12.3f %14.0f %10.2f %8llu %8llu\n", kind.c_str(), t,
                    static_cast<double>(bars.size()) / t, t * 1e6 / static_cast<double>(bars.size()),
                    static_cast<unsigned long long>(signals), static_cast<unsigned long long>(fills));
    }
    std::printf("\n");
}

// --- bus ---------------------------------------------------------------------

events::Event market_event(std::uint64_t i) {
    events::Event e;
    e.type    = events::EventType::MarketData;
    e.payload = bar("SYM0", common::Timestamp{std::chrono::seconds{1'704'205'800}}, 100.0, 100.5, i);
    return e;
}

void bench_bus(const Options& o) {
    std::printf("bus: publish -> handler, one market-data event at a time (%zu pings), then a burst of %zu\n",
                o.pings, o.events);
    std::printf("  %-10s %10s %10s %10s %10s %16s\n", "bus", "p50 us", "p90 us", "p99 us", "max us", "burst events/s");
    for (const auto& kind : kBuses) {
        const common::SystemClock clock;
        BusUnderTest b{kind, clock};
        std::atomic<std::uint64_t> delivered{0};
        b.bus->subscribe(events::EventType::MarketData, [&](const events::Event&) {
            delivered.fetch_add(1, std::memory_order_release);
        });
        b.bus->start();

        // Warm up, then one event in flight at a time.
        std::vector<double> latencies;
        latencies.reserve(o.pings);
        const std::size_t warmup = std::min<std::size_t>(o.pings / 10 + 1, 1'000);
        for (std::size_t i = 0; i < warmup + o.pings; ++i) {
            const std::uint64_t target = delivered.load() + 1;
            const auto t0 = Steady::now();
            (void)b.bus->publish(market_event(i));
            while (delivered.load(std::memory_order_acquire) < target) {
                std::this_thread::yield();
            }
            if (i >= warmup) {
                latencies.push_back(std::chrono::duration<double, std::micro>(Steady::now() - t0).count());
            }
        }
        std::sort(latencies.begin(), latencies.end());

        const std::uint64_t before = delivered.load();
        const auto t0 = Steady::now();
        for (std::size_t i = 0; i < o.events; ++i) {
            (void)b.bus->publish(market_event(i));
        }
        while (delivered.load(std::memory_order_acquire) < before + o.events) {
            std::this_thread::yield();
        }
        const double burst = static_cast<double>(o.events) / seconds_since(t0);

        std::printf("  %-10s %10.1f %10.1f %10.1f %10.1f %16.0f\n", kind.c_str(), percentile(latencies, 0.50),
                    percentile(latencies, 0.90), percentile(latencies, 0.99), latencies.back(), burst);
        b.bus->request_shutdown();
        b.bus->wait_until_drained();
    }
    std::printf("\n");
}

// --- codec -------------------------------------------------------------------

void bench_codec(const Options& o) {
    events::WireEnvelope envelope{"bench", market_event(1)};
    const std::string sample = events::encode(envelope);
    std::size_t sink = 0;

    auto t0 = Steady::now();
    for (std::size_t i = 0; i < o.events; ++i) {
        envelope.event.sequence = i;
        sink += events::encode(envelope).size();
    }
    const double encode_ns = seconds_since(t0) * 1e9 / static_cast<double>(o.events);

    t0 = Steady::now();
    for (std::size_t i = 0; i < o.events; ++i) {
        sink += events::decode(sample).event.sequence;
    }
    const double decode_ns = seconds_since(t0) * 1e9 / static_cast<double>(o.events);

    std::printf("codec: one market-data envelope (%zu bytes), %zu times\n", sample.size(), o.events);
    std::printf("  encode %8.0f ns  (%10.0f /s)\n", encode_ns, 1e9 / encode_ns);
    std::printf("  decode %8.0f ns  (%10.0f /s)\n", decode_ns, 1e9 / decode_ns);
    std::printf("  (checksum %zu)\n\n", sink % 10);
}

// Peak resident memory of the whole process so far, in MiB; negative if unknown.
double peak_memory_mib() {
#if defined(_WIN32)
    return -1.0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1.0;
    }
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);   // bytes
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;              // KiB
#endif
#endif
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
#if defined(NDEBUG)
    const char* build = "optimised";
#else
    const char* build = "DEBUG (numbers not meaningful; build with -DCMAKE_BUILD_TYPE=Release)";
#endif
    std::printf("trading_engine_bench %s, %s build, %u hardware threads\n\n", std::string(kVersion).c_str(), build,
                std::thread::hardware_concurrency());
    try {
        if (o.only.empty() || o.only == "replay") {
            bench_replay(o);
        }
        if (o.only.empty() || o.only == "bus") {
            bench_bus(o);
        }
        if (o.only.empty() || o.only == "codec") {
            bench_codec(o);
        }
        if (const double mib = peak_memory_mib(); mib >= 0.0) {
            std::printf("peak memory: %.1f MiB (whole process, all benchmarks so far)\n", mib);
        }
    } catch (const std::exception& e) {
        std::cerr << "trading_engine_bench: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
