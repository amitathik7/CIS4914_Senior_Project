// Turning diagnostics on must change nothing a strategy decides: not one signal, not a
// field of one, not an engine counter, not the strategy's state. This file checks that
// instead of arguing it, on seeded pseudo-random streams full of bad prices, repeated and
// older timestamps, unlisted symbols and non-bar events, both driving a strategy directly
// and through the real StrategyEngine over a RecordingEventBus. It also pins the observer
// contract: what happens when an observer throws, when it re-enters the strategy or the
// engine, and when it is attached, detached or the engine restarts.
//
// What an observer REPORTS is in strategy_diagnostics_test.cpp.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "strategy_diagnostics_fixture.hpp"
#include "support/recording_event_bus.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/strategy/strategy_engine.hpp"

namespace {

using namespace diag_test;
namespace events  = trading_engine::events;
namespace support = trading_engine::test_support;
using strategy::BarAction;
using strategy::BarReason;

constexpr std::size_t kStreamLength = 4000;

// ----- Direct drive -------------------------------------------------------------------

template <class Strategy>
std::vector<domain::TradeSignal> drive(Strategy& strategy,
                                       const std::vector<domain::MarketEvent>& stream) {
    Sink sink;
    for (const domain::MarketEvent& event : stream) {
        strategy.on_market_event(event, sink);
    }
    return std::move(sink.signals);
}

TEST(StrategyDiagnosticsInvariance, CrossoverSignalsAreIdenticalWithAndWithoutAnObserver) {
    const std::vector<std::pair<std::size_t, std::size_t>> windows{{2, 3}, {3, 7}, {5, 20}};
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        const auto stream = random_stream(seed, kStreamLength);
        for (const auto& [short_window, long_window] : windows) {
            SCOPED_TRACE("seed " + std::to_string(seed) + " windows " + std::to_string(short_window) +
                         "/" + std::to_string(long_window));
            strategy::MovingAverageCrossoverStrategy plain{sma_config(short_window, long_window, {"AAPL", "MSFT"})};
            strategy::MovingAverageCrossoverStrategy observed{sma_config(short_window, long_window, {"AAPL", "MSFT"})};
            Collector collector;
            observed.set_observer(&collector);

            const auto want = drive(plain, stream);
            const auto got  = drive(observed, stream);

            EXPECT_GT(want.size(), 0u) << "a stream with no signal would prove nothing";
            EXPECT_TRUE(same_signals(want, got));
            EXPECT_EQ(collector.all.size(), stream.size()) << "exactly one snapshot per event";
            EXPECT_EQ(observed.observer_failures(), 0u);
        }
    }
}

TEST(StrategyDiagnosticsInvariance, MeanReversionSignalsAreIdenticalWithAndWithoutAnObserver) {
    struct Setup { std::size_t lookback; double entry; double rearm; };
    const std::vector<Setup> setups{{4, 1.2, 0.5}, {10, 1.5, 0.5}, {20, 2.0, 0.5}};
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        const auto stream = random_stream(seed, kStreamLength);
        for (const Setup& setup : setups) {
            SCOPED_TRACE("seed " + std::to_string(seed) + " lookback " + std::to_string(setup.lookback));
            strategy::MeanReversionStrategy plain{mr_config(setup.lookback, setup.entry, setup.rearm, {"AAPL", "MSFT"})};
            strategy::MeanReversionStrategy observed{mr_config(setup.lookback, setup.entry, setup.rearm, {"AAPL", "MSFT"})};
            Collector collector;
            observed.set_observer(&collector);

            const auto want = drive(plain, stream);
            const auto got  = drive(observed, stream);

            EXPECT_GT(want.size(), 0u);
            EXPECT_TRUE(same_signals(want, got));
            EXPECT_EQ(collector.all.size(), stream.size());
            EXPECT_EQ(observed.observer_failures(), 0u);
        }
    }
}

// ----- Through the real engine ---------------------------------------------------------

// A real engine over a RecordingEventBus and a ManualClock, with both reference strategies.
class World {
public:
    explicit World(bool observe) {
        sma = std::make_shared<strategy::MovingAverageCrossoverStrategy>(sma_config(3, 7, {"AAPL", "MSFT"}));
        mr  = std::make_shared<strategy::MeanReversionStrategy>(mr_config(10, 1.5, 0.5, {"AAPL", "MSFT"}));
        if (observe) {
            sma->set_observer(&sma_seen);
            mr->set_observer(&mr_seen);
        }
        engine.register_strategy(sma);
        engine.register_strategy(mr);
        engine.start();
    }

    // As a replay delivers a bar: the clock moves to the bar's time, then the bus publishes it.
    void deliver(const domain::MarketEvent& event) {
        clock.set(event.exchange_time);
        events::Event envelope;
        envelope.type    = events::EventType::MarketData;
        envelope.payload = event;
        bus.publish(std::move(envelope));
    }

    void deliver_all(const std::vector<domain::MarketEvent>& stream) {
        for (const domain::MarketEvent& event : stream) {
            deliver(event);
        }
    }

    // Declared before the engine so they outlive it.
    support::RecordingEventBus bus;
    common::ManualClock        clock;
    Collector                  sma_seen;
    Collector                  mr_seen;
    strategy::StrategyEngine   engine{bus, clock};
    std::shared_ptr<strategy::MovingAverageCrossoverStrategy> sma;
    std::shared_ptr<strategy::MeanReversionStrategy>          mr;
};

// Every engine counter except events_dropped, which one test changes on purpose.
void expect_same_counters(const World& a, const World& b, bool include_dropped = true) {
    const auto x = a.engine.stats();
    const auto y = b.engine.stats();
    EXPECT_EQ(x.events_routed, y.events_routed);
    if (include_dropped) {
        EXPECT_EQ(x.events_dropped, y.events_dropped);
    }
    EXPECT_EQ(x.strategy_errors, y.strategy_errors);
    EXPECT_EQ(x.signals_published, y.signals_published);
    EXPECT_EQ(x.signals_rejected, y.signals_rejected);
    EXPECT_EQ(x.publish_errors, y.publish_errors);
    EXPECT_EQ(x.signals_dropped, y.signals_dropped);
    EXPECT_EQ(x.bus_errors, y.bus_errors);

    const auto px = a.engine.strategy_stats();
    const auto py = b.engine.strategy_stats();
    ASSERT_EQ(px.size(), py.size());
    for (std::size_t i = 0; i < px.size(); ++i) {
        EXPECT_EQ(px[i].id, py[i].id);
        EXPECT_EQ(px[i].events_delivered, py[i].events_delivered);
        EXPECT_EQ(px[i].errors, py[i].errors);
        EXPECT_EQ(px[i].signals_published, py[i].signals_published);
        EXPECT_EQ(px[i].signals_rejected, py[i].signals_rejected);
        EXPECT_EQ(px[i].publish_errors, py[i].publish_errors);
        EXPECT_EQ(px[i].signals_dropped, py[i].signals_dropped);
        EXPECT_EQ(px[i].last_error, py[i].last_error);
    }
}

TEST(StrategyDiagnosticsEngine, TheEngineSeesTheSameSignalsAndCountersWithDiagnosticsOnAndOff) {
    for (const std::uint64_t seed : {11ULL, 12ULL}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        const auto stream = random_stream(seed, kStreamLength);
        World off{false};
        World on{true};
        off.deliver_all(stream);
        on.deliver_all(stream);

        const auto want = off.bus.signals();
        const auto got  = on.bus.signals();
        EXPECT_GT(want.size(), 0u);
        EXPECT_TRUE(same_signals(want, got)) << "ids, clock stamps, quantities and metadata included";
        expect_same_counters(off, on);

        EXPECT_EQ(on.engine.strategy_stats()[0].events_delivered, stream.size());
        EXPECT_EQ(on.sma_seen.all.size(), stream.size());
        EXPECT_EQ(on.mr_seen.all.size(), stream.size());
        EXPECT_EQ(on.sma->observer_failures(), 0u);
        EXPECT_EQ(on.mr->observer_failures(), 0u);
    }
}

// ----- An observer that throws ---------------------------------------------------------

class ThrowingObserver final : public strategy::IStrategyObserver {
public:
    enum class Kind { RuntimeError, NonStandard, BadAlloc };
    explicit ThrowingObserver(Kind kind) : kind_{kind} {}

    void on_bar(const domain::MarketEvent&, const strategy::BarSnapshot&) override {
        ++calls;
        switch (kind_) {
            case Kind::RuntimeError: throw std::runtime_error("observer failed");
            case Kind::NonStandard:  throw 42;   // NOLINT: deliberately not a std::exception
            case Kind::BadAlloc:     throw std::bad_alloc{};
        }
    }

    std::size_t calls{0};

private:
    Kind kind_;
};

TEST(StrategyDiagnosticsObserverFailure, AThrowingObserverChangesNothingTheEngineSeesAndIsCounted) {
    const auto stream = random_stream(21, kStreamLength);
    for (const auto kind : {ThrowingObserver::Kind::RuntimeError, ThrowingObserver::Kind::NonStandard,
                            ThrowingObserver::Kind::BadAlloc}) {
        ThrowingObserver observer{kind};   // outlives the strategies that point at it
        World baseline{false};
        World thrown{false};
        thrown.sma->set_observer(&observer);
        thrown.mr->set_observer(&observer);

        baseline.deliver_all(stream);
        thrown.deliver_all(stream);

        EXPECT_TRUE(same_signals(baseline.bus.signals(), thrown.bus.signals()));
        expect_same_counters(baseline, thrown);
        EXPECT_EQ(thrown.engine.stats().strategy_errors, 0u) << "an observer fault is not a strategy fault";
        EXPECT_EQ(thrown.engine.strategy_stats()[0].last_error, "");

        EXPECT_EQ(observer.calls, 2 * stream.size()) << "still called for every event after a throw";
        EXPECT_EQ(thrown.sma->observer_failures(), stream.size());
        EXPECT_EQ(thrown.mr->observer_failures(), stream.size());
    }
}

// ----- An observer that re-enters ------------------------------------------------------

// Runs `attempt` on every callback. A std::logic_error from it is recorded, and rethrown
// to the strategy unless `swallow` is set.
class ReentrantObserver final : public strategy::IStrategyObserver {
public:
    std::function<void(const domain::MarketEvent&)> attempt;
    bool swallow{true};
    std::size_t calls{0};
    std::vector<std::string> caught;

    void on_bar(const domain::MarketEvent& event, const strategy::BarSnapshot&) override {
        ++calls;
        try {
            attempt(event);
        } catch (const std::logic_error& error) {
            caught.emplace_back(error.what());
            if (!swallow) {
                throw;
            }
        }
    }
};

struct ReentrySetup {
    std::vector<domain::MarketEvent>         stream = random_stream(31, 1500);
    strategy::MovingAverageCrossoverStrategy plain{sma_config(3, 7, {"AAPL", "MSFT"})};
    strategy::MovingAverageCrossoverStrategy probed{sma_config(3, 7, {"AAPL", "MSFT"})};
    ReentrantObserver                        observer;
    Sink                                     reentry_sink;
};

TEST(StrategyDiagnosticsReentry, CallingOnMarketEventFromTheObserverIsRefusedAndChangesNothing) {
    for (const bool swallow : {true, false}) {
        ReentrySetup s;
        s.observer.swallow = swallow;
        s.observer.attempt = [&](const domain::MarketEvent& event) { s.probed.on_market_event(event, s.reentry_sink); };
        s.probed.set_observer(&s.observer);

        EXPECT_TRUE(same_signals(drive(s.plain, s.stream), drive(s.probed, s.stream)));
        EXPECT_TRUE(s.reentry_sink.signals.empty()) << "the nested call never ran";
        ASSERT_EQ(s.observer.caught.size(), s.stream.size());
        EXPECT_NE(s.observer.caught[0].find("on_market_event"), std::string::npos) << s.observer.caught[0];
        EXPECT_NE(s.observer.caught[0].find("observer contract"), std::string::npos);
        // Caught inside the observer: no failure. Let through: the strategy counts and drops it.
        EXPECT_EQ(s.probed.observer_failures(), swallow ? 0u : s.stream.size());
    }
}

TEST(StrategyDiagnosticsReentry, CallingOnStartFromTheObserverDoesNotResetTheStrategy) {
    ReentrySetup s;
    s.observer.attempt = [&](const domain::MarketEvent&) { s.probed.on_start(); };
    s.probed.set_observer(&s.observer);

    // A reset in mid-stream would forget the windows and change the signals.
    EXPECT_TRUE(same_signals(drive(s.plain, s.stream), drive(s.probed, s.stream)));
    ASSERT_EQ(s.observer.caught.size(), s.stream.size());
    EXPECT_NE(s.observer.caught[0].find("on_start"), std::string::npos);
}

TEST(StrategyDiagnosticsReentry, DetachingFromInsideTheObserverIsRefusedSoItStaysAttached) {
    ReentrySetup s;
    s.observer.attempt = [&](const domain::MarketEvent&) { s.probed.set_observer(nullptr); };
    s.probed.set_observer(&s.observer);

    drive(s.probed, s.stream);
    EXPECT_EQ(s.observer.calls, s.stream.size()) << "called for every event: never detached";
    ASSERT_FALSE(s.observer.caught.empty());
    EXPECT_NE(s.observer.caught[0].find("set_observer"), std::string::npos);
}

TEST(StrategyDiagnosticsReentry, AnObserverFeedingTheEngineAnEventHasItDroppedAndCounted) {
    const auto stream = random_stream(41, 1500);
    ReentrantObserver observer;   // outlives the strategies that point at it
    World baseline{false};
    World probed{false};

    observer.attempt = [&](const domain::MarketEvent& event) { probed.engine.on_market_event(event); };
    probed.sma->set_observer(&observer);

    baseline.deliver_all(stream);
    probed.deliver_all(stream);

    EXPECT_TRUE(same_signals(baseline.bus.signals(), probed.bus.signals()));
    expect_same_counters(baseline, probed, /*include_dropped=*/false);
    // The engine's own rule: a nested market event is refused. One attempt per crossover-strategy call.
    EXPECT_EQ(probed.engine.stats().events_dropped, stream.size());
    EXPECT_EQ(baseline.engine.stats().events_dropped, 0u);
    EXPECT_EQ(observer.calls, stream.size());
}

// ----- Attach, detach, restart, emit -----------------------------------------------------

TEST(StrategyDiagnosticsLifecycle, AttachingAndDetachingBetweenEventsOnlyChangesWhatIsReported) {
    const auto stream = random_stream(51, 150);
    strategy::MeanReversionStrategy plain{mr_config(5, 1.2, 0.5, {"AAPL", "MSFT"})};
    strategy::MeanReversionStrategy toggled{mr_config(5, 1.2, 0.5, {"AAPL", "MSFT"})};
    Collector collector;
    Sink plain_sink;
    Sink toggled_sink;

    for (std::size_t i = 0; i < stream.size(); ++i) {
        if (i == 50) {
            toggled.set_observer(&collector);
        }
        if (i == 100) {
            toggled.set_observer(nullptr);
        }
        plain.on_market_event(stream[i], plain_sink);
        toggled.on_market_event(stream[i], toggled_sink);
    }
    EXPECT_EQ(collector.all.size(), 50u) << "events 50..99 only";
    EXPECT_TRUE(same_signals(plain_sink.signals, toggled_sink.signals));
}

TEST(StrategyDiagnosticsLifecycle, ARestartResetsTheStrategyButNotTheObserverAndReportsTheSameRun) {
    // AAPL 3 2 1 2 3 4 3 2 1 for a 2/3 pair; the second run replays the same bars and times.
    World world{true};
    const std::vector<double> closes{3, 2, 1, 2, 3, 4, 3, 2, 1};
    std::vector<domain::MarketEvent> stream;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        stream.push_back(event_at("AAPL", closes[i], static_cast<long long>(i) + 1));
    }

    world.deliver_all(stream);
    world.engine.stop();
    world.engine.start();
    world.deliver_all(stream);

    ASSERT_EQ(world.sma_seen.all.size(), 2 * stream.size()) << "the observer survived the restart";
    for (std::size_t i = 0; i < stream.size(); ++i) {
        const Seen& first  = world.sma_seen.all[i];
        const Seen& second = world.sma_seen.all[i + stream.size()];
        EXPECT_EQ(first.reason, second.reason) << "bar " << i + 1;
        EXPECT_EQ(first.action, second.action);
        EXPECT_EQ(first.window_fill, second.window_fill);
        EXPECT_EQ(first.indicators, second.indicators);
        EXPECT_EQ(first.states, second.states);
    }
    EXPECT_EQ(world.mr_seen.all.size(), 2 * stream.size());
}

class ThrowingSink final : public strategy::ISignalSink {
public:
    void emit(const domain::TradeSignal&) override { throw std::runtime_error("sink refused"); }
};

TEST(StrategyDiagnosticsLifecycle, TheSnapshotIsReportedBeforeEmitSoASinkThatThrowsCannotHideIt) {
    strategy::MovingAverageCrossoverStrategy sma{sma_config(2, 3)};
    Collector collector;
    ThrowingSink sink;
    sma.set_observer(&collector);

    long long minute = 0;
    std::size_t thrown_at = 0;
    for (const double close : {3.0, 2.0, 1.0, 2.0, 3.0, 4.0}) {
        ++minute;
        try {
            sma.on_market_event(event_at("AAPL", close, minute), sink);
        } catch (const std::runtime_error&) {
            thrown_at = static_cast<std::size_t>(minute);
        }
    }
    EXPECT_EQ(thrown_at, 5u) << "the crossover bar: emit() threw there and only there";
    ASSERT_EQ(collector.all.size(), 6u);
    EXPECT_EQ(collector.all[4].action, BarAction::Buy) << "reported even though emit() threw";
    EXPECT_EQ(collector.all[4].state("relation_after"), "short_above_long");
    EXPECT_EQ(collector.all[5].reason, BarReason::SameSide) << "state was committed before emit, as documented";
}

}  // namespace
