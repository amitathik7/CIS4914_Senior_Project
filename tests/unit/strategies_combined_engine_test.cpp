// Both reference strategies registered together, with unique ids, in ONE real
// StrategyEngine over a RecordingEventBus and a ManualClock, driven by a
// deterministic two-symbol bar stream. What each strategy does on its own is in
// moving_average_crossover_*_test.cpp and mean_reversion_*_test.cpp; what a lone
// strategy does inside an engine is in the *_engine_test.cpp files. This file adds
// only what needs the two together: the interleaved order of their signals, their
// independence from each other and from each symbol, replay with fresh instances,
// a restart, and the engine's diagnostics when the bus refuses a signal or another
// strategy throws.
//
// The expected signals were derived by hand and cross-checked with an independent
// exact-fraction script; nothing is read back from the implementation.
//
// The stream (one bar per minute, AAPL's bar and then MSFT's):
//   AAPL  3 2 1 2 3 4 3 2 1
//   MSFT 10 10 10 6 9 2 30 30 30
// Strategies, registered in this order: "sma_2_3" (SMA 2/3, symbols AAPL, MSFT) and
// "mr_4" (lookback 4, entry 1.2, rearm 0.5, symbols AAPL, MSFT).
//   AAPL  sma_2_3: Buy at bar 5, Sell at 8.   mr_4: Sell at bar 5 (z +1.4142), Buy at 8.
//   MSFT  sma_2_3: Buy at bar 7.              mr_4: Buy at 4 (z -1.7321), Buy at 6
//         (z -1.5261), Sell at 7 (z +1.6859).
// Within a bar the engine delivers the event to sma_2_3 first (registration order),
// and AAPL's event before MSFT's, so the published order is:
//   minute 4  mr_4   MSFT Buy          id 1
//   minute 5  sma_2_3 AAPL Buy         id 2    mr_4 AAPL Sell    id 3
//   minute 6  mr_4   MSFT Buy          id 4
//   minute 7  sma_2_3 MSFT Buy         id 5    mr_4 MSFT Sell    id 6
//   minute 8  sma_2_3 AAPL Sell        id 7    mr_4 AAPL Buy     id 8

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/fake_strategy.hpp"
#include "support/price_literals.hpp"
#include "support/recording_event_bus.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

namespace {

using namespace std::chrono_literals;
namespace common   = trading_engine::common;
namespace domain   = trading_engine::domain;
namespace events   = trading_engine::events;
namespace strategy = trading_engine::strategy;
namespace support  = trading_engine::test_support;
using domain::SignalSide;

const common::Timestamp kStart = common::Timestamp{} + std::chrono::hours{5};

const std::vector<common::Price> kAapl = support::units({3, 2, 1, 2, 3, 4, 3, 2, 1});
const std::vector<common::Price> kMsft = support::units({10, 10, 10, 6, 9, 2, 30, 30, 30});

// What a published signal must show, independently of the implementation.
struct Expected {
    std::string strategy_id;
    std::string symbol;
    SignalSide  side;
    int         minute;   // the bar that triggered it; created_at is the clock at that bar
    std::uint64_t id;
};

const std::vector<Expected> kExpectedAll{
    {"mr_4", "MSFT", SignalSide::Buy, 4, 1},      {"sma_2_3", "AAPL", SignalSide::Buy, 5, 2},
    {"mr_4", "AAPL", SignalSide::Sell, 5, 3},     {"mr_4", "MSFT", SignalSide::Buy, 6, 4},
    {"sma_2_3", "MSFT", SignalSide::Buy, 7, 5},   {"mr_4", "MSFT", SignalSide::Sell, 7, 6},
    {"sma_2_3", "AAPL", SignalSide::Sell, 8, 7},  {"mr_4", "AAPL", SignalSide::Buy, 8, 8},
};

// A full world: bus, clock, real engine and the two real strategies. Each World is
// fresh, so two Worlds share nothing.
class World {
public:
    explicit World(bool with_sma = true, bool with_mr = true) {
        if (with_sma) {
            strategy::MovingAverageCrossoverConfig cfg;
            cfg.strategy_id  = "sma_2_3";
            cfg.short_window = 2;
            cfg.long_window  = 3;
            cfg.symbols      = {"AAPL", "MSFT"};
            engine.register_strategy(std::make_shared<strategy::MovingAverageCrossoverStrategy>(cfg));
        }
        if (with_mr) {
            strategy::MeanReversionConfig cfg;
            cfg.strategy_id     = "mr_4";
            cfg.lookback        = 4;
            cfg.entry_threshold = 1.2;
            cfg.rearm_threshold = 0.5;
            cfg.symbols         = {"AAPL", "MSFT"};
            engine.register_strategy(std::make_shared<strategy::MeanReversionStrategy>(cfg));
        }
    }

    static domain::MarketEvent bar(const std::string& symbol, common::Price close, int minute) {
        domain::MarketEvent event;
        event.symbol        = symbol;
        event.type          = domain::MarketEventType::Bar;
        event.exchange_time = kStart + std::chrono::minutes{minute};
        event.price         = close;
        return event;
    }

    // One bar through the bus, the way MarketDataService will deliver it. The clock
    // moves to the bar's time first, as a replay does.
    void deliver(const std::string& symbol, common::Price close, int minute) {
        clock.set(kStart + std::chrono::minutes{minute});
        events::Event envelope;
        envelope.type    = events::EventType::MarketData;
        envelope.payload = bar(symbol, close, minute);
        bus.publish(std::move(envelope));
    }

    // The same bar handed to the engine directly. Needed while the bus is refusing
    // everything: the double would refuse the market event itself too.
    void deliver_directly(const std::string& symbol, common::Price close, int minute) {
        clock.set(kStart + std::chrono::minutes{minute});
        engine.on_market_event(bar(symbol, close, minute));
    }

    // The whole stream: minute 1..9, AAPL then MSFT. `only` limits it to one symbol.
    void replay(const std::string& only = {}) {
        for (int minute = 1; minute <= 9; ++minute) {
            const auto i = static_cast<std::size_t>(minute) - 1;
            if (only.empty() || only == "AAPL") {
                deliver("AAPL", kAapl[i], minute);
            }
            if (only.empty() || only == "MSFT") {
                deliver("MSFT", kMsft[i], minute);
            }
        }
    }

    support::RecordingEventBus bus;
    common::ManualClock        clock{kStart};
    strategy::StrategyEngine   engine{bus, clock};
};

// Everything observable about a published signal, so two runs compare in one line.
using Fingerprint = std::tuple<std::uint64_t, std::string, std::string, SignalSide, common::Timestamp,
                               std::optional<common::Quantity>, std::optional<domain::OrderType>,
                               std::map<std::string, std::string>>;

std::vector<Fingerprint> fingerprints(const std::vector<domain::TradeSignal>& signals) {
    std::vector<Fingerprint> out;
    for (const domain::TradeSignal& s : signals) {
        out.emplace_back(s.id.value, s.strategy_id, s.symbol, s.side, s.created_at, s.requested_quantity,
                         s.order_type, s.metadata);
    }
    return out;
}

void expect_signals(const std::vector<domain::TradeSignal>& actual, const std::vector<Expected>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE("signal " + std::to_string(i));
        EXPECT_EQ(actual[i].strategy_id, expected[i].strategy_id);
        EXPECT_EQ(actual[i].symbol, expected[i].symbol);
        EXPECT_EQ(actual[i].side, expected[i].side);
        EXPECT_EQ(actual[i].created_at, kStart + std::chrono::minutes{expected[i].minute});
        EXPECT_EQ(actual[i].id.value, expected[i].id);
        EXPECT_EQ(actual[i].requested_quantity, std::optional<common::Quantity>{1});
        EXPECT_EQ(actual[i].order_type, std::optional<domain::OrderType>{domain::OrderType::Market});
        EXPECT_FALSE(actual[i].target_exposure.has_value());
        EXPECT_FALSE(actual[i].limit_price.has_value());
        EXPECT_FALSE(actual[i].confidence.has_value());
    }
}

// The signals of `all` that `keep` selects, with `ids` ignored (they depend on what else ran).
template <class Keep>
std::vector<Fingerprint> without_ids(const std::vector<domain::TradeSignal>& all, Keep keep) {
    std::vector<domain::TradeSignal> picked;
    for (domain::TradeSignal s : all) {
        if (keep(s)) {
            s.id = common::SignalId{};
            picked.push_back(std::move(s));
        }
    }
    return fingerprints(picked);
}

}  // namespace

// --- The combined stream ------------------------------------------------------------

TEST(CombinedStrategies, PublishTheExpectedSignalsInEngineOrderWithUniqueIdsAndClockTimes) {
    World run;
    run.engine.start();
    run.replay();

    expect_signals(run.bus.signals(), kExpectedAll);

    // Contents of a few, from the hand calculation (mean and z of the signalling window).
    const std::vector<domain::TradeSignal> signals = run.bus.signals();
    EXPECT_EQ(signals[0].metadata.at("mean"), "9");                // mr_4 MSFT [10 10 10 6]
    EXPECT_NEAR(std::stod(signals[0].metadata.at("z_score")), -1.7320508075688772, 1e-9);
    EXPECT_EQ(signals[1].metadata.at("short_sma"), "2.5");         // sma_2_3 AAPL bar 5
    EXPECT_EQ(signals[1].metadata.at("long_sma"), "2");
    EXPECT_EQ(signals[4].metadata.at("short_sma"), "16");          // sma_2_3 MSFT bar 7: (2+30)/2
    EXPECT_NEAR(std::stod(signals[4].metadata.at("long_sma")), 41.0 / 3.0, 1e-12);   // (9+2+30)/3
    EXPECT_NEAR(std::stod(signals[5].metadata.at("z_score")), 1.6858627860337072, 1e-9);

    const strategy::StrategyEngine::Stats stats = run.engine.stats();
    EXPECT_EQ(stats.events_routed, 18u);
    EXPECT_EQ(stats.signals_published, 8u);
    EXPECT_EQ(stats.signals_rejected, 0u);
    EXPECT_EQ(stats.publish_errors, 0u);
    EXPECT_EQ(stats.signals_dropped, 0u);
    EXPECT_EQ(stats.strategy_errors, 0u);
    const auto per_strategy = run.engine.strategy_stats();
    ASSERT_EQ(per_strategy.size(), 2u);
    EXPECT_EQ(per_strategy[0].id, "sma_2_3");
    EXPECT_EQ(per_strategy[0].signals_published, 3u);   // AAPL x2, MSFT x1
    EXPECT_EQ(per_strategy[1].id, "mr_4");
    EXPECT_EQ(per_strategy[1].signals_published, 5u);
    EXPECT_EQ(per_strategy[0].events_delivered, 18u) << "every strategy sees every event";
    EXPECT_EQ(per_strategy[1].events_delivered, 18u);
}

TEST(CombinedStrategies, EachSymbolAndEachStrategyIsUnaffectedByTheOthers) {
    World together;
    together.engine.start();
    together.replay();
    const std::vector<domain::TradeSignal> all = together.bus.signals();

    // A symbol alone: the other symbol's bars change nothing about it.
    for (const std::string symbol : {"AAPL", "MSFT"}) {
        SCOPED_TRACE(symbol);
        World alone;
        alone.engine.start();
        alone.replay(symbol);
        const auto on_symbol = [&symbol](const domain::TradeSignal& s) { return s.symbol == symbol; };
        EXPECT_EQ(without_ids(alone.bus.signals(), on_symbol), without_ids(all, on_symbol));
        EXPECT_FALSE(alone.bus.signals().empty());
    }

    // A strategy alone: the other strategy's presence changes nothing about it.
    for (const std::string id : {"sma_2_3", "mr_4"}) {
        SCOPED_TRACE(id);
        World alone{id == "sma_2_3", id == "mr_4"};
        alone.engine.start();
        alone.replay();
        const auto by_id = [&id](const domain::TradeSignal& s) { return s.strategy_id == id; };
        EXPECT_EQ(without_ids(alone.bus.signals(), by_id), without_ids(all, by_id));
        EXPECT_FALSE(alone.bus.signals().empty());
    }
}

TEST(CombinedStrategies, FreshInstancesReplayTheSameStreamIdentically) {
    World first;
    first.engine.start();
    first.replay();
    World second;
    second.engine.start();
    second.replay();

    ASSERT_EQ(first.bus.signals().size(), 8u);
    EXPECT_EQ(fingerprints(first.bus.signals()), fingerprints(second.bus.signals()))
        << "ids, strategies, symbols, sides, times, quantities and metadata";
}

TEST(CombinedStrategies, ARestartResetsBothStrategiesWhileTheEngineKeepsCounting) {
    World run;
    run.engine.start();
    run.replay();
    ASSERT_EQ(run.bus.signals().size(), 8u);

    // on_start() clears both strategies' histories, timestamps, baselines and latches,
    // so the same bars at the same times are fresh again. The engine resets neither
    // its ids nor its statistics.
    run.engine.stop();
    run.engine.start();
    run.replay();

    const std::vector<domain::TradeSignal> signals = run.bus.signals();
    ASSERT_EQ(signals.size(), 16u);
    std::vector<Expected> expected = kExpectedAll;
    for (Expected e : kExpectedAll) {
        e.id += 8;
        expected.push_back(e);
    }
    expect_signals(signals, expected);
    EXPECT_EQ(run.engine.stats().signals_published, 16u);
    EXPECT_EQ(run.engine.stats().events_routed, 36u);
}

// --- Failure semantics --------------------------------------------------------------

TEST(CombinedStrategies, ARefusedSignalIsCountedLostAndNeverRetriedAndProcessingContinues) {
    World run;
    run.engine.start();

    // Minute 7's MSFT bar would publish sma_2_3's Buy (id 5) and mr_4's Sell (id 6).
    // The bus refuses both. Everything else is driven directly so the refusal reaches
    // only those two signals.
    for (int minute = 1; minute <= 9; ++minute) {
        const auto i = static_cast<std::size_t>(minute) - 1;
        run.deliver_directly("AAPL", kAapl[i], minute);
        run.bus.faults.reject_publish = (minute == 7);
        run.deliver_directly("MSFT", kMsft[i], minute);
        run.bus.faults.reject_publish = false;
    }

    // The other six are published, under the ids the stream gives them: the two
    // refused signals each consumed an id, which is never reused.
    const std::vector<Expected> expected{
        kExpectedAll[0], kExpectedAll[1], kExpectedAll[2], kExpectedAll[3], kExpectedAll[6],
        kExpectedAll[7],
    };
    expect_signals(run.bus.signals(), expected);

    // The bus was offered both refused signals, stamped, and refused them.
    ASSERT_EQ(run.bus.rejected().size(), 2u);
    std::vector<std::string> refused_by;
    for (const events::Event& event : run.bus.rejected()) {
        const auto* signal = std::get_if<domain::TradeSignal>(&event.payload);
        ASSERT_NE(signal, nullptr);
        refused_by.push_back(signal->strategy_id + "#" + std::to_string(signal->id.value));
    }
    EXPECT_EQ(refused_by, (std::vector<std::string>{"sma_2_3#5", "mr_4#6"}));

    // Diagnostics: counted per strategy as rejections, not as strategy errors, bus
    // faults or deliveries. Nothing re-offered them on bars 8 and 9, because both
    // strategies had already committed their state when they emitted.
    const auto stats = run.engine.stats();
    EXPECT_EQ(stats.signals_published, 6u);
    EXPECT_EQ(stats.signals_rejected, 2u);
    EXPECT_EQ(stats.publish_errors, 0u);
    EXPECT_EQ(stats.strategy_errors, 0u);
    const auto per_strategy = run.engine.strategy_stats();
    EXPECT_EQ(per_strategy[0].signals_rejected, 1u);
    EXPECT_EQ(per_strategy[1].signals_rejected, 1u);
    EXPECT_EQ(per_strategy[0].events_delivered, 18u) << "a refusal never stops the event flow";
    EXPECT_EQ(per_strategy[1].events_delivered, 18u);
}

TEST(CombinedStrategies, AThrowingNeighbourCostsTheRealStrategiesNothing) {
    World run{false, false};
    support::FakeStrategy* faulty = nullptr;
    {
        // Registered FIRST, so it throws on every event before either real strategy runs.
        auto fake            = std::make_shared<support::FakeStrategy>("faulty");
        fake->throw_on_event = true;
        faulty               = fake.get();
        run.engine.register_strategy(fake);
    }
    strategy::MovingAverageCrossoverConfig sma;
    sma.strategy_id  = "sma_2_3";
    sma.short_window = 2;
    sma.long_window  = 3;
    sma.symbols      = {"AAPL", "MSFT"};
    run.engine.register_strategy(std::make_shared<strategy::MovingAverageCrossoverStrategy>(sma));
    strategy::MeanReversionConfig mr;
    mr.strategy_id     = "mr_4";
    mr.lookback        = 4;
    mr.entry_threshold = 1.2;
    mr.symbols         = {"AAPL", "MSFT"};
    run.engine.register_strategy(std::make_shared<strategy::MeanReversionStrategy>(mr));
    run.engine.start();

    run.replay();

    // The same eight signals with the same ids: the faulty strategy emitted nothing
    // and consumed no id.
    expect_signals(run.bus.signals(), kExpectedAll);
    EXPECT_EQ(faulty->received.size(), 18u) << "it kept receiving events after failing";

    const auto stats = run.engine.stats();
    EXPECT_EQ(stats.strategy_errors, 18u) << "one absorbed exception per event";
    EXPECT_EQ(stats.signals_published, 8u);
    EXPECT_EQ(stats.publish_errors, 0u);
    const auto per_strategy = run.engine.strategy_stats();
    ASSERT_EQ(per_strategy.size(), 3u);
    EXPECT_EQ(per_strategy[0].id, "faulty");
    EXPECT_EQ(per_strategy[0].errors, 18u);
    EXPECT_EQ(per_strategy[0].last_error, "on_market_event: faulty: on_market_event failed");
    EXPECT_EQ(per_strategy[1].errors, 0u) << "failures are attributed to the strategy that threw";
    EXPECT_EQ(per_strategy[2].errors, 0u);
}
