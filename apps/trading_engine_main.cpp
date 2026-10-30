// trading_engine -- runs the components that exist today, wired through one
// event bus (docs/EVENT_BUS.md):
//
//   CSV bars -> MarketDataService -> bus -> StrategyEngine -> bus (signals)
//                                        -> PortfolioManager (marks)
//
// Risk, execution and analytics are still stubs, so a run ends at the signals
// the strategies publish; nothing is ordered or filled. Simulated only: no
// network market data, no real orders.
//
//   trading_engine                      build info and usage
//   trading_engine run --bars FILE [--bus inprocess|topics|kafka|kafka-mock]
//                      [--brokers HOST:PORT] [--topic-prefix NAME]
//                      [--sma SHORT,LONG] [--lookback N] [--quantity N]

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "bar_csv.hpp"
#include "trading_engine/app/pipeline.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/timestamp_text.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/events/event_codec.hpp"
#include "trading_engine/events/message_transport.hpp"
#include "trading_engine/events/topic_event_bus.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"
#include "trading_engine/version.hpp"

#if defined(TRADING_ENGINE_HAVE_KAFKA)
#include "trading_engine/kafka/rdkafka_transport.hpp"
#endif

namespace {

using namespace trading_engine;   // NOLINT(build/namespaces) -- small main only

constexpr const char* kUsage =
    "usage:\n"
    "  trading_engine run --bars FILE [options]\n"
    "\n"
    "options:\n"
    "  --bus inprocess|topics|kafka|kafka-mock   event bus (default inprocess)\n"
    "        inprocess   bounded in-memory queue, one dispatcher thread\n"
    "        topics      the Kafka topic bus over an in-memory broker (no Kafka needed)\n"
    "        kafka       the topic bus over a real Kafka cluster (--brokers)\n"
    "        kafka-mock  the topic bus over librdkafka's in-process mock cluster\n"
    "  --brokers HOST:PORT     Kafka bootstrap servers (default localhost:9092)\n"
    "  --topic-prefix NAME     topic prefix (default trading_engine)\n"
    "  --sma SHORT,LONG        moving-average windows (default 5,20)\n"
    "  --lookback N            mean-reversion lookback (default 20)\n"
    "  --quantity N            shares requested per signal (default 1)\n";

struct Options {
    std::string bars;
    std::string bus{"inprocess"};
    std::string brokers{"localhost:9092"};
    std::string topic_prefix{"trading_engine"};
    std::size_t short_window{5};
    std::size_t long_window{20};
    std::size_t lookback{20};
    common::Quantity quantity{1};
};

[[noreturn]] void usage_error(const std::string& message) {
    std::cerr << "trading_engine: " << message << "\n\n" << kUsage;
    std::exit(2);
}

std::size_t to_size(const std::string& text, const char* flag) {
    try {
        std::size_t used = 0;
        const unsigned long long value = std::stoull(text, &used);
        if (used == text.size()) {
            return static_cast<std::size_t>(value);
        }
    } catch (...) {
    }
    usage_error(std::string(flag) + " needs a whole number, got '" + text + "'");
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) {
            usage_error(flag + " needs a value");
        }
        const std::string value = argv[++i];
        if (flag == "--bars") {
            o.bars = value;
        } else if (flag == "--bus") {
            o.bus = value;
        } else if (flag == "--brokers") {
            o.brokers = value;
        } else if (flag == "--topic-prefix") {
            o.topic_prefix = value;
        } else if (flag == "--sma") {
            const auto comma = value.find(',');
            if (comma == std::string::npos) {
                usage_error("--sma needs SHORT,LONG");
            }
            o.short_window = to_size(value.substr(0, comma), "--sma");
            o.long_window  = to_size(value.substr(comma + 1), "--sma");
        } else if (flag == "--lookback") {
            o.lookback = to_size(value, "--lookback");
        } else if (flag == "--quantity") {
            o.quantity = static_cast<common::Quantity>(to_size(value, "--quantity"));
        } else {
            usage_error("unknown option " + flag);
        }
    }
    if (o.bars.empty()) {
        usage_error("run needs --bars FILE");
    }
    return o;
}

// Everything a bus choice needs kept alive for the run.
struct BusStack {
    std::shared_ptr<events::InMemoryBroker>     broker;
    std::unique_ptr<events::IMessageTransport>  transport;
#if defined(TRADING_ENGINE_HAVE_KAFKA)
    std::unique_ptr<kafka::KafkaMockCluster>    mock_cluster;
#endif
    std::unique_ptr<events::IEventBus>          bus;

    ~BusStack() {
        bus.reset();         // stops the dispatcher before its transport goes
        transport.reset();
    }
};

void make_bus(const Options& o, const common::IClock& clock, BusStack& stack) {
    events::TopicEventBusOptions topic_options;
    topic_options.topic_prefix = o.topic_prefix;

    if (o.bus == "inprocess") {
        stack.bus = std::make_unique<events::InProcessEventBus>(clock);
        return;
    }
    if (o.bus == "topics") {
        stack.broker    = std::make_shared<events::InMemoryBroker>();
        stack.transport = std::make_unique<events::InMemoryTransport>(stack.broker);
    } else if (o.bus == "kafka" || o.bus == "kafka-mock") {
#if defined(TRADING_ENGINE_HAVE_KAFKA)
        kafka::KafkaTransportConfig config;
        config.bootstrap_servers = o.brokers;
        if (o.bus == "kafka-mock") {
            stack.mock_cluster       = std::make_unique<kafka::KafkaMockCluster>();
            config.bootstrap_servers = stack.mock_cluster->bootstrap_servers();
            // The mock does not implement CreateTopics: make the topics on it directly.
            config.create_topics = false;
            for (const auto type : {events::EventType::MarketData, events::EventType::Signal,
                                    events::EventType::Order, events::EventType::Fill}) {
                stack.mock_cluster->create_topic(
                    o.topic_prefix + "." + std::string(events::to_string(type)), config.partitions);
            }
        }
        stack.transport = std::make_unique<kafka::RdKafkaTransport>(config);
#else
        usage_error("this build has no Kafka support; reconfigure with -DTRADING_ENGINE_WITH_KAFKA=ON");
#endif
    } else {
        usage_error("unknown --bus '" + o.bus + "'");
    }
    stack.bus = std::make_unique<events::TopicEventBus>(*stack.transport, clock, topic_options);
}

int run(const Options& o) {
    const std::vector<domain::MarketEvent> bars = cli::read_bar_csv(o.bars);
    std::set<common::Symbol> symbol_set;
    for (const auto& bar : bars) {
        if (!bar.symbol.empty()) {
            symbol_set.insert(bar.symbol);
        }
    }
    const std::vector<common::Symbol> symbols(symbol_set.begin(), symbol_set.end());
    if (symbols.empty()) {
        std::cerr << "trading_engine: " << o.bars << " has no rows with a symbol\n";
        return 1;
    }

    const common::SystemClock clock;
    BusStack stack;
    make_bus(o, clock, stack);

    // Print signals as they come off the bus. The handler runs on the bus
    // dispatcher; the lock keeps lines whole if anything else prints.
    std::mutex out_mutex;
    std::uint64_t signals_seen = 0;
    stack.bus->subscribe(events::EventType::Signal, [&](const events::Event& e) {
        const auto& s = std::get<domain::TradeSignal>(e.payload);
        const std::lock_guard lock{out_mutex};
        ++signals_seen;
        std::cout << "signal #" << s.id.value << "  " << s.strategy_id << "  "
                  << domain::to_string(s.side) << ' ' << s.requested_quantity.value_or(0) << ' '
                  << s.symbol;
        if (const auto bar = s.metadata.find("trigger"); bar != s.metadata.end()) {
            std::cout << "  (" << bar->second << ")";
        }
        std::cout << '\n';
    });

    app::Pipeline pipeline{*stack.bus, clock};

    strategy::MovingAverageCrossoverConfig sma;
    sma.short_window       = o.short_window;
    sma.long_window        = o.long_window;
    sma.requested_quantity = o.quantity;
    sma.symbols            = symbols;
    pipeline.add_strategy(std::make_shared<strategy::MovingAverageCrossoverStrategy>(sma));

    strategy::MeanReversionConfig mr;
    mr.lookback           = o.lookback;
    mr.requested_quantity = o.quantity;
    mr.symbols            = symbols;
    pipeline.add_strategy(std::make_shared<strategy::MeanReversionStrategy>(mr));

    std::cout << "bus: " << o.bus << ", bars: " << bars.size() << " from " << o.bars
              << ", symbols: " << symbols.size() << "\n\n";
    pipeline.start();
    for (const auto& bar : bars) {
        pipeline.input().on_market_event(bar);
    }
    pipeline.finish();

    const auto md   = pipeline.market_data().stats();
    const auto se   = pipeline.strategies().stats();
    const auto pb   = pipeline.portfolio_bridge().stats();
    const auto snap = pipeline.portfolio().snapshot();
    std::cout << "\nmarket data   accepted " << md.accepted << ", rejected " << md.rejected << '\n'
              << "strategies    routed " << se.events_routed << ", signals published "
              << se.signals_published << ", rejected " << se.signals_rejected << ", errors "
              << se.strategy_errors << '\n'
              << "signals seen  " << signals_seen << " (on the bus, after the round trip)\n"
              << "portfolio     marks " << pb.marks << ", cash "
              << common::format_decimal(snap.cash) << ", equity "
              << common::format_decimal(snap.total_equity) << '\n'
              << "\nRisk and execution are not implemented yet, so no orders or fills.\n";

    if (auto* topic_bus = dynamic_cast<events::TopicEventBus*>(stack.bus.get())) {
        const auto s = topic_bus->stats();
        std::cout << "topic bus     published " << s.published << ", received " << s.received
                  << ", decode errors " << s.decode_errors << ", drain timeouts "
                  << s.drain_timeouts << "  (topics " << topic_bus->topic(events::EventType::MarketData)
                  << ", ...)\n";
    } else if (auto* local = dynamic_cast<events::InProcessEventBus*>(stack.bus.get())) {
        const auto s = local->stats();
        std::cout << "in-process    published " << s.published << ", delivered " << s.delivered
                  << ", high water " << s.high_water_mark << '\n';
    }
    return md.accepted == 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "run") {
        std::cout << kProjectName << ' ' << kVersion << '\n' << kDescription << "\n\n" << kUsage;
        return argc < 2 ? 0 : 2;
    }
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "trading_engine: " << e.what() << '\n';
        return 1;
    }
}
