// trading_engine -- runs the components that exist today, wired through one
// event bus (docs/EVENT_BUS.md), either once from the command line (`run`) or
// as a long-running engine that takes commands from the console (`serve`,
// docs/adr/0007-engine-command-channel.md):
//
//   CSV bars -> MarketDataService -> bus -> StrategyEngine -> bus (signals)
//        -> RiskManager -> bus (risk decisions) -> ExecutionSimulator
//        -> bus (orders, fills) -> PortfolioManager
//
// The bars are replayed in market time: the clock is set to each bar's time
// and the bus is drained before the next timestamp, so a market order placed
// on one bar fills at the next bar's open on every bus. Simulated only: no
// network market data, no real orders.
//
//   trading_engine                      build info and usage
//   trading_engine run --bars FILE [--bus inprocess|topics|kafka|kafka-mock]
//                      [--brokers HOST:PORT] [--topic-prefix NAME]
//                      [--sma SHORT,LONG] [--lookback N] [--quantity N]
//                      [--cash N] [--risk on|off]
//   trading_engine serve --bars FILE [--bars FILE...] [--bus kafka|kafka-mock]
//                      [--brokers HOST:PORT] [--control-prefix NAME]
//                      [--engine-id NAME] [--pace-ms N] [--heartbeat-ms N]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bar_csv.hpp"
#include "trading_engine/app/configured_run.hpp"
#include "trading_engine/app/engine_service.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/timestamp_text.hpp"
#include "trading_engine/control/control_codec.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/events/event_codec.hpp"
#include "trading_engine/events/message_transport.hpp"
#include "trading_engine/events/topic_event_bus.hpp"
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
    "  --quantity N            shares requested per signal (default 1)\n"
    "  --cash N                starting cash in dollars (default 100000)\n"
    "  --risk on|off           run the starter risk policies (default on); buying\n"
    "                          power and no-short-selling are always checked\n"
    "  With a topic bus, the run's settings are published on <prefix>.run.\n"
    "\n"
    "  trading_engine serve --bars FILE [options]\n"
    "\n"
    "  Waits for commands from the console (start a backtest, stop it, kill switch)\n"
    "  on <control-prefix>.command and replays the recorded bars for each backtest.\n"
    "  --bars FILE             recorded bars to offer (repeatable)\n"
    "  --bus kafka|kafka-mock  (default kafka)\n"
    "  --brokers HOST:PORT     Kafka bootstrap servers (default localhost:9092)\n"
    "  --control-prefix NAME   command/heartbeat topics (default trading_engine.control)\n"
    "  --engine-id NAME        this engine's id (default engine-<random>)\n"
    "  --pace-ms N             pause N ms after each bar time, so a run can be watched\n"
    "  --heartbeat-ms N        announce this engine every N ms (default 2000)\n";

struct Options {
    std::string command{"run"};
    std::vector<std::string> bar_files;
    std::string bars;
    std::string bus{"inprocess"};
    std::string brokers{"localhost:9092"};
    std::string topic_prefix{"trading_engine"};
    std::size_t short_window{5};
    std::size_t long_window{20};
    std::size_t lookback{20};
    common::Quantity quantity{1};
    std::int64_t cash{100'000};
    bool risk_policies{true};
    // serve
    std::string control_prefix{control::kDefaultControlPrefix};
    std::string engine_id;
    std::size_t pace_ms{0};
    std::size_t heartbeat_ms{2000};
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
    o.command = argv[1];
    if (o.command == "serve") {
        o.bus = "kafka";
    }
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) {
            usage_error(flag + " needs a value");
        }
        const std::string value = argv[++i];
        if (flag == "--bars") {
            o.bars = value;
            o.bar_files.push_back(value);
        } else if (o.command == "serve" && flag == "--control-prefix") {
            o.control_prefix = value;
        } else if (o.command == "serve" && flag == "--engine-id") {
            o.engine_id = value;
        } else if (o.command == "serve" && flag == "--pace-ms") {
            o.pace_ms = to_size(value, "--pace-ms");
        } else if (o.command == "serve" && flag == "--heartbeat-ms") {
            o.heartbeat_ms = std::max<std::size_t>(100, to_size(value, "--heartbeat-ms"));
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
        } else if (flag == "--cash") {
            o.cash = static_cast<std::int64_t>(to_size(value, "--cash"));
        } else if (flag == "--risk") {
            if (value != "on" && value != "off") {
                usage_error("--risk needs on or off");
            }
            o.risk_policies = value == "on";
        } else {
            usage_error("unknown option " + flag);
        }
    }
    if (o.bars.empty()) {
        usage_error(o.command + " needs --bars FILE");
    }
    if (o.command == "serve" && o.bus != "kafka" && o.bus != "kafka-mock") {
        usage_error("serve needs --bus kafka or kafka-mock: the console reaches it through Kafka");
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

#if defined(TRADING_ENGINE_HAVE_KAFKA)
// A Kafka transport for the topics under `prefix`. On the mock cluster, which
// does not implement CreateTopics, the topics are made on it directly.
std::unique_ptr<events::IMessageTransport> make_kafka_transport(const Options& o, const std::string& prefix,
                                                                kafka::KafkaMockCluster* mock) {
    kafka::KafkaTransportConfig config;
    config.bootstrap_servers = o.brokers;
    // A run drains the bus once per bar timestamp, so every hop's latency
    // counts: don't let an idle fetch sit for librdkafka's 500 ms.
    config.fetch_wait_max = std::chrono::milliseconds{10};
    if (mock != nullptr) {
        config.bootstrap_servers = mock->bootstrap_servers();
        config.create_topics     = false;
        std::vector<std::string> topics;
        if (prefix == o.control_prefix && o.command == "serve") {
            topics = {control::command_topic(prefix), control::engine_topic(prefix)};
        } else {
            for (const auto type : {events::EventType::MarketData, events::EventType::Signal,
                                    events::EventType::Order, events::EventType::Fill,
                                    events::EventType::RiskDecision}) {
                topics.push_back(prefix + "." + std::string(events::to_string(type)));
            }
            topics.push_back(control::run_topic(prefix));
        }
        for (const auto& topic : topics) {
            try {
                mock->create_topic(topic, config.partitions);
            } catch (const kafka::KafkaError&) {
                // already there
            }
        }
    }
    return std::make_unique<kafka::RdKafkaTransport>(config);
}
#endif

void make_bus(const Options& o, const common::IClock& clock, BusStack& stack) {
    events::TopicEventBusOptions topic_options;
    topic_options.topic_prefix  = o.topic_prefix;
    topic_options.poll_interval = std::chrono::milliseconds{5};

    if (o.bus == "inprocess") {
        stack.bus = std::make_unique<events::InProcessEventBus>(clock);
        return;
    }
    if (o.bus == "topics") {
        stack.broker    = std::make_shared<events::InMemoryBroker>();
        stack.transport = std::make_unique<events::InMemoryTransport>(stack.broker);
    } else if (o.bus == "kafka" || o.bus == "kafka-mock") {
#if defined(TRADING_ENGINE_HAVE_KAFKA)
        if (o.bus == "kafka-mock") {
            stack.mock_cluster = std::make_unique<kafka::KafkaMockCluster>();
        }
        stack.transport = make_kafka_transport(o, o.topic_prefix, stack.mock_cluster.get());
#else
        usage_error("this build has no Kafka support; reconfigure with -DTRADING_ENGINE_WITH_KAFKA=ON");
#endif
    } else {
        usage_error("unknown --bus '" + o.bus + "'");
    }
    stack.bus = std::make_unique<events::TopicEventBus>(*stack.transport, clock, topic_options);
}

// The settings the command-line flags describe: both reference strategies on
// every symbol in the file, the default risk limits and simulation.
control::RunSettings settings_from(const Options& o, const std::vector<common::Symbol>& symbols,
                                   const std::vector<domain::MarketEvent>& bars) {
    control::RunSettings s;
    s.name          = o.topic_prefix;
    s.period_start  = bars.front().exchange_time;
    s.period_end    = bars.back().exchange_time;
    s.risk_policies = o.risk_policies;
    s.simulation.starting_cash = common::Money::from_units(o.cash);
    control::StrategySettings sma;
    sma.kind               = control::StrategyKind::SmaCrossover;
    sma.strategy_id        = "sma_crossover";
    sma.symbols            = symbols;
    sma.requested_quantity = o.quantity;
    sma.short_window       = o.short_window;
    sma.long_window        = o.long_window;
    control::StrategySettings mr;
    mr.kind               = control::StrategyKind::MeanReversion;
    mr.strategy_id        = "mean_reversion";
    mr.symbols            = symbols;
    mr.requested_quantity = o.quantity;
    mr.lookback           = o.lookback;
    s.strategies = {sma, mr};
    return s;
}

// <prefix>.run for a command-line run: its settings and state, with no
// engine id (nothing can send it commands). Only on a topic bus.
void publish_status(const Options& o, BusStack& stack, const control::RunSettings& settings,
                    control::RunState state, std::optional<common::Timestamp> clock, std::uint64_t sequence) {
    if (!stack.transport) {
        return;
    }
    control::RunStatusReport r;
    r.run_id     = o.topic_prefix;
    r.status     = state;
    r.updated_at = common::SystemClock{}.now();
    r.settings   = settings;
    r.progress   = state == control::RunState::Completed ? 1.0 : 0.0;
    r.clock      = clock;
    const control::ControlEnvelope envelope{"trading_engine-run", sequence, r.updated_at, r};
    (void)stack.transport->send({control::run_topic(o.topic_prefix), o.topic_prefix, control::encode(envelope)});
}

int run(const Options& o) {
    std::vector<domain::MarketEvent> bars = cli::read_bar_csv(o.bars);
    std::stable_sort(bars.begin(), bars.end(), [](const auto& a, const auto& b) {
        return a.exchange_time < b.exchange_time;
    });
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

    common::ManualClock clock{bars.empty() ? common::Timestamp{} : bars.front().exchange_time};
    BusStack stack;
    make_bus(o, clock, stack);
    const control::RunSettings settings = settings_from(o, symbols, bars);
    control::validate(settings);

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

    std::uint64_t orders_seen = 0, fills_seen = 0, rejected_orders = 0;
    std::uint64_t approved = 0, resized = 0, rejected = 0;
    stack.bus->subscribe(events::EventType::RiskDecision, [&](const events::Event& e) {
        const auto& r = std::get<domain::RiskDecisionReport>(e.payload);
        const std::lock_guard lock{out_mutex};
        switch (r.outcome) {
            case domain::RiskOutcome::Approved: ++approved; return;
            case domain::RiskOutcome::Resized:  ++resized; break;
            case domain::RiskOutcome::Rejected: ++rejected; break;
        }
        std::cout << "  risk " << domain::to_string(r.outcome) << " signal #" << r.signal_id.value
                  << " (" << r.reason.value_or("") << ")";
        if (!r.checks.empty()) {
            std::cout << ": " << r.checks.back().detail;
        }
        std::cout << '\n';
    });
    stack.bus->subscribe(events::EventType::Order, [&](const events::Event& e) {
        const auto& order = std::get<domain::Order>(e.payload);
        const std::lock_guard lock{out_mutex};
        ++orders_seen;
        if (order.status == domain::OrderStatus::Rejected) {
            ++rejected_orders;
            std::cout << "  order #" << order.id.value << " rejected: "
                      << order.reject_reason.value_or("") << '\n';
        }
    });
    stack.bus->subscribe(events::EventType::Fill, [&](const events::Event& e) {
        const auto& f = std::get<domain::Fill>(e.payload);
        const std::lock_guard lock{out_mutex};
        ++fills_seen;
        std::cout << "  fill #" << f.id.value << "  order #" << f.order_id.value << "  "
                  << (f.filled_quantity < 0 ? "sold " : "bought ")
                  << (f.filled_quantity < 0 ? -f.filled_quantity : f.filled_quantity) << ' '
                  << f.symbol << " @ " << common::format_decimal(f.fill_price) << "  fees "
                  << common::format_decimal(f.fees) << "  " << common::format_timestamp(f.filled_at)
                  << '\n';
    });

    app::ConfiguredRun configured{*stack.bus, clock, settings};
    auto& pipeline = configured.pipeline();
    auto& flow     = configured.flow();

    std::cout << "bus: " << o.bus << ", bars: " << bars.size() << " from " << o.bars
              << ", symbols: " << symbols.size() << "\n\n";
    publish_status(o, stack, settings, control::RunState::Running, std::nullopt, 1);
    (void)configured.replay(bars);
    publish_status(o, stack, settings, control::RunState::Completed, bars.back().exchange_time, 2);
    if (stack.transport) {
        (void)stack.transport->flush(std::chrono::seconds{10});
    }

    const auto md   = pipeline.market_data().stats();
    const auto se   = pipeline.strategies().stats();
    const auto pb   = pipeline.portfolio_bridge().stats();
    const auto snap = pipeline.portfolio().snapshot();
    std::cout << "\nmarket data   accepted " << md.accepted << ", rejected " << md.rejected << '\n'
              << "strategies    routed " << se.events_routed << ", signals published "
              << se.signals_published << ", rejected " << se.signals_rejected << ", errors "
              << se.strategy_errors << '\n'
              << "signals seen  " << signals_seen << " (on the bus, after the round trip)\n"
              << "risk          approved " << approved << ", resized " << resized
              << ", rejected " << rejected << (o.risk_policies ? "" : "  (policies off)") << '\n'
              << "execution     orders " << orders_seen << " events, " << rejected_orders
              << " rejected, fills " << fills_seen << ", still open "
              << flow.execution().open_order_count() << '\n'
              << "portfolio     marks " << pb.marks << ", fills applied " << pb.fills_applied
              << ", errors " << pb.errors << '\n'
              << "              cash " << common::format_decimal(snap.cash) << ", equity "
              << common::format_decimal(snap.total_equity) << '\n';
    for (const auto& p : snap.positions) {
        if (!p.is_flat()) {
            std::cout << "              holding " << p.quantity << ' ' << p.symbol << " (avg cost "
                      << common::format_decimal(p.average_cost) << ")\n";
        }
    }

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

std::atomic<bool> g_interrupted{false};

extern "C" void on_signal(int) { g_interrupted = true; }

int serve(const Options& o) {
#if defined(TRADING_ENGINE_HAVE_KAFKA)
    app::EngineServiceConfig config;
    config.control_prefix = o.control_prefix;
    config.engine_id      = o.engine_id;
    config.version        = kVersion;
    config.instant_delay  = std::chrono::milliseconds{o.pace_ms};
    config.heartbeat      = std::chrono::milliseconds{o.heartbeat_ms};
    for (const auto& file : o.bar_files) {
        auto bars = cli::read_bar_csv(file);
        config.bars.insert(config.bars.end(), bars.begin(), bars.end());
    }
    std::unique_ptr<kafka::KafkaMockCluster> mock;
    if (o.bus == "kafka-mock") {
        mock = std::make_unique<kafka::KafkaMockCluster>();
    }
    common::SystemClock wall;
    app::EngineService service{[&](std::string_view prefix) {
                                   return make_kafka_transport(o, std::string(prefix), mock.get());
                               },
                               wall, config};
    service.start();
    const auto a = service.announcement();
    std::cout << "engine " << service.engine_id() << " serving " << config.bars.size() << " bars ("
              << a.symbols.size() << " symbols, " << a.first_session << " to " << a.last_session << ")\n"
              << "commands: " << control::command_topic(o.control_prefix) << " on "
              << (mock ? mock->bootstrap_servers() : o.brokers) << ". Ctrl+C stops it.\n"
              << std::flush;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    app::EngineService::Stats last{};
    while (!g_interrupted) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        const auto now = service.stats();
        if (now.completed + now.stopped + now.failed != last.completed + last.stopped + last.failed ||
            now.commands != last.commands) {
            std::cout << "commands " << now.commands << ", runs completed " << now.completed << ", stopped "
                      << now.stopped << ", failed " << now.failed << ", refused " << now.refused << '\n'
                      << std::flush;
            last = now;
        }
    }
    std::cout << "stopping\n";
    service.stop();
    return 0;
#else
    (void)o;
    usage_error("serve needs Kafka; reconfigure with -DTRADING_ENGINE_WITH_KAFKA=ON");
#endif
}

int main(int argc, char** argv) {
    const std::string command = argc < 2 ? "" : argv[1];
    if (command != "run" && command != "serve") {
        std::cout << kProjectName << ' ' << kVersion << '\n' << kDescription << "\n\n" << kUsage;
        return argc < 2 ? 0 : 2;
    }
    try {
        const Options options = parse(argc, argv);
        return command == "serve" ? serve(options) : run(options);
    } catch (const control::SettingsError& e) {
        std::cerr << "trading_engine: " << e.field() << ": " << e.message() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "trading_engine: " << e.what() << '\n';
        return 1;
    }
}
