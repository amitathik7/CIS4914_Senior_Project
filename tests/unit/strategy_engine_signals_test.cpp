// StrategyEngine routing and signal handling: which events reach which
// strategies, and how emitted signals are stamped, ordered and published,
// including when the bus refuses them.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "strategy_engine_fixture.hpp"

namespace {

using namespace std::chrono_literals;
using support::CallLog;
using StrategyIds = std::vector<std::string>;

// Every field a strategy controls, i.e. all but the three the engine stamps.
void expect_same_content(const domain::TradeSignal& actual, const domain::TradeSignal& expected) {
    EXPECT_EQ(actual.symbol, expected.symbol);
    EXPECT_EQ(actual.side, expected.side);
    EXPECT_EQ(actual.requested_quantity, expected.requested_quantity);
    EXPECT_EQ(actual.target_exposure, expected.target_exposure);
    EXPECT_EQ(actual.confidence, expected.confidence);
    EXPECT_EQ(actual.metadata, expected.metadata);
    EXPECT_EQ(actual.order_type, expected.order_type);
    EXPECT_EQ(actual.limit_price, expected.limit_price);
}

// A fully populated explicit-quantity signal, including the ADR 0002 fields.
domain::TradeSignal quantity_signal() {
    domain::TradeSignal signal = signal_for("AAPL", domain::SignalSide::Sell);
    signal.requested_quantity = 50.0;
    signal.confidence         = 0.75;
    signal.metadata           = {{"reason", "crossover"}, {"fast", "10"}};
    signal.order_type         = domain::OrderType::Limit;
    signal.limit_price        = 152.75;
    return signal;
}

StrategyIds strategy_ids_of(const std::vector<domain::TradeSignal>& signals) {
    StrategyIds out;
    for (const domain::TradeSignal& signal : signals) {
        out.push_back(signal.strategy_id);
    }
    return out;
}

std::vector<std::uint64_t> id_values_of(const std::vector<domain::TradeSignal>& signals) {
    std::vector<std::uint64_t> out;
    for (const domain::TradeSignal& signal : signals) {
        out.push_back(signal.id.value);
    }
    return out;
}

}  // namespace

// --- Routing -----------------------------------------------------------------

TEST_F(StrategyEngineTest, DeliversEveryEventToEveryStrategyInRegistrationOrder) {
    add("zeta");
    add("alpha");
    engine.start();
    calls.clear();

    publish_market(market_event("AAPL"));
    publish_market(market_event("MSFT"));

    EXPECT_EQ(calls, (CallLog{"zeta:event:AAPL", "alpha:event:AAPL",
                              "zeta:event:MSFT", "alpha:event:MSFT"}));
}

TEST_F(StrategyEngineTest, DoesNotFilterBySymbolOrEventTypeThatIsTheStrategysJob) {
    const auto a = add("a");
    engine.start();

    using domain::MarketEventType;
    const std::vector<MarketEventType> types{MarketEventType::Unknown, MarketEventType::Trade,
                                             MarketEventType::Quote, MarketEventType::Bar,
                                             MarketEventType::Status};
    for (const MarketEventType type : types) {
        publish_market(market_event("NOT_MINE", type));
    }

    ASSERT_EQ(a->received.size(), types.size());
    for (std::size_t i = 0; i < types.size(); ++i) {
        EXPECT_EQ(a->received[i].type, types[i]);
        EXPECT_EQ(a->received[i].symbol, "NOT_MINE");
    }
}

TEST_F(StrategyEngineTest, HandsStrategiesTheEventWithItsFieldsIntact) {
    const auto a = add("a");
    engine.start();

    domain::MarketEvent sent = market_event("AAPL", domain::MarketEventType::Bar, 101.25);
    sent.exchange_time = kT0 + 3s;
    sent.ingest_time   = kT0 + 4s;
    sent.bid           = 101.0;
    sent.ask           = 101.5;
    sent.volume        = 12'000.0;
    sent.sequence      = 42;
    publish_market(sent);

    ASSERT_EQ(a->received.size(), 1u);
    const domain::MarketEvent& got = a->received[0];
    EXPECT_EQ(got.symbol, sent.symbol);
    EXPECT_EQ(got.type, sent.type);
    EXPECT_EQ(got.exchange_time, sent.exchange_time);
    EXPECT_EQ(got.ingest_time, sent.ingest_time);
    EXPECT_EQ(got.price, sent.price);
    EXPECT_EQ(got.bid, sent.bid);
    EXPECT_EQ(got.ask, sent.ask);
    EXPECT_EQ(got.volume, sent.volume);
    EXPECT_EQ(got.sequence, sent.sequence);
}

TEST_F(StrategyEngineTest, EventsFromTheBusAndFromADirectCallTakeTheSamePath) {
    const auto a = add("a");
    engine.start();

    publish_market(market_event("VIA_BUS"));
    engine.on_market_event(market_event("DIRECT"));   // as an IMarketEventSink

    ASSERT_EQ(a->received.size(), 2u);
    EXPECT_EQ(a->received[0].symbol, "VIA_BUS");
    EXPECT_EQ(a->received[1].symbol, "DIRECT");
    EXPECT_EQ(engine.stats().events_routed, 2u);
}

TEST_F(StrategyEngineTest, AnEngineWithNoStrategiesStartsRoutesNothingAndStops) {
    engine.start();
    EXPECT_TRUE(publish_market(market_event("AAPL")));
    engine.stop();

    EXPECT_EQ(engine.stats().events_routed, 1u);
    EXPECT_TRUE(bus.signals().empty());
}

// --- Stamping ----------------------------------------------------------------

TEST_F(StrategyEngineTest, PublishesTheStampedSignalAsASignalEvent) {
    const auto a = add("sma_crossover");
    a->emits = {signal_for("AAPL")};
    engine.start();

    publish_market(market_event("AAPL"));

    const std::vector<events::Event> envelopes = signal_events();
    ASSERT_EQ(envelopes.size(), 1u);
    EXPECT_EQ(envelopes[0].type, events::EventType::Signal);
    const auto* payload = std::get_if<domain::TradeSignal>(&envelopes[0].payload);
    ASSERT_NE(payload, nullptr) << "a Signal event must carry a TradeSignal";
    EXPECT_TRUE(payload->id.valid()) << "ids are nonzero";
    EXPECT_EQ(payload->strategy_id, "sma_crossover");
    EXPECT_EQ(payload->created_at, clock.now());
    EXPECT_EQ(payload->symbol, "AAPL");
}

TEST_F(StrategyEngineTest, StampsCreatedAtFromTheInjectedClockAtEmitTime) {
    const auto a = add("a");
    a->emits = {signal_for("AAPL"), signal_for("MSFT")};   // two signals from one callback
    engine.start();

    clock.set(kT0 + 1s);
    publish_market(market_event("AAPL"));
    clock.set(kT0 + 2s);
    publish_market(market_event("AAPL"));

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 4u);
    EXPECT_EQ(signals[0].created_at, kT0 + 1s);
    EXPECT_EQ(signals[1].created_at, kT0 + 1s) << "the clock did not move within one callback";
    EXPECT_EQ(signals[2].created_at, kT0 + 2s);
    EXPECT_EQ(signals[3].created_at, kT0 + 2s);
}

TEST_F(StrategyEngineTest, ReplacesTheIdentityFieldsAStrategySupplied) {
    const auto a = add("a");
    domain::TradeSignal claimed = signal_for("AAPL");
    claimed.id          = common::SignalId{999};
    claimed.strategy_id = "someone_else";
    claimed.created_at  = kT0 + 99h;
    a->emits = {claimed};
    engine.start();

    publish_market(market_event("AAPL"));

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].id.value, 1u) << "the engine mints the id; a strategy cannot choose it";
    EXPECT_EQ(signals[0].strategy_id, "a") << "a strategy cannot sign for another";
    EXPECT_EQ(signals[0].created_at, clock.now());
}

TEST_F(StrategyEngineTest, ForwardsEveryOtherFieldUnchanged) {
    const auto a = add("a");
    domain::TradeSignal exposure_style = signal_for("MSFT", domain::SignalSide::Flat);
    exposure_style.target_exposure = 0.05;
    const domain::TradeSignal quantity_style = quantity_signal();
    a->emits = {quantity_style, exposure_style};
    engine.start();

    publish_market(market_event("AAPL"));

    const std::vector<domain::TradeSignal> signals = bus.signals();
    ASSERT_EQ(signals.size(), 2u);
    expect_same_content(signals[0], quantity_style);
    expect_same_content(signals[1], exposure_style);
}

TEST_F(StrategyEngineTest, NeverModifiesTheStrategysOwnSignalObject) {
    const auto a = add("a");
    domain::TradeSignal original = quantity_signal();
    original.strategy_id = "preset";
    original.created_at  = kT0 + 7h;
    a->emits = {original};
    engine.start();

    publish_market(market_event("AAPL"));

    ASSERT_EQ(bus.signals().size(), 1u);
    const domain::TradeSignal& after = a->emits[0];   // what the strategy still holds
    expect_same_content(after, original);
    EXPECT_FALSE(after.id.valid()) << "the engine stamps its copy, not the caller's object";
    EXPECT_EQ(after.strategy_id, "preset");
    EXPECT_EQ(after.created_at, kT0 + 7h);
}

// --- Sinks -------------------------------------------------------------------

TEST_F(StrategyEngineTest, EachStrategyEmitsThroughItsOwnSinkAndIsStampedAsItself) {
    const auto a = add("a");
    const auto b = add("b");
    a->emits = {signal_for("AAPL")};   // neither sets strategy_id
    b->emits = {signal_for("AAPL")};
    engine.start();

    publish_market(market_event("AAPL"));

    ASSERT_NE(a->last_sink, nullptr);
    ASSERT_NE(b->last_sink, nullptr);
    EXPECT_NE(a->last_sink, b->last_sink) << "one sink per strategy";
    EXPECT_EQ(strategy_ids_of(bus.signals()), (StrategyIds{"a", "b"}));
}

TEST_F(StrategyEngineTest, AStrategyCannotEmitThroughAnotherStrategysSink) {
    // Signed as the sink's owner it would be a forgery; attributed to the
    // caller it would be a lie about the sink. It is refused instead.
    const auto a = add("a");
    const auto b = add("b");
    b->on_event_hook = [raw = a.get()](const domain::MarketEvent&, strategy::ISignalSink&) {
        raw->last_sink->emit(signal_for("FORGED"));   // a was delivered first, so it has a sink
    };
    engine.start();

    publish_market(market_event("AAPL"));

    EXPECT_TRUE(bus.signals().empty());
    EXPECT_EQ(engine.strategy_stats()[0].signals_dropped, 1u);
    EXPECT_EQ(engine.stats().signals_dropped, 1u);
}

TEST_F(StrategyEngineTest, ASinkUsedOutsideItsCallbackIsDroppedAndConsumesNoId) {
    const auto a = add("a");
    engine.start();
    publish_market(market_event("AAPL"));   // a is handed its sink here
    ASSERT_NE(a->last_sink, nullptr);
    a->on_stop_hook = [raw = a.get()] { raw->last_sink->emit(signal_for("FROM_ON_STOP")); };

    a->last_sink->emit(signal_for("AFTER_THE_CALLBACK"));
    engine.stop();   // on_stop() tries as well

    EXPECT_TRUE(bus.signals().empty());
    EXPECT_EQ(engine.stats().signals_dropped, 2u);

    // A signal that was never offered to the bus did not use up an id.
    a->emits = {signal_for("AAPL")};
    engine.start();
    publish_market(market_event("AAPL"));
    ASSERT_EQ(bus.signals().size(), 1u);
    EXPECT_EQ(bus.signals()[0].id.value, 1u);
}

// --- Ids and deterministic ordering -----------------------------------------

TEST_F(StrategyEngineTest, AssignsDistinctIncreasingIdsInRegistrationAndEmissionOrder) {
    const auto first  = add("zeta");    // registered first, though "alpha" sorts first
    const auto second = add("alpha");
    first->emits  = {signal_for("AAPL"), signal_for("MSFT")};
    second->emits = {signal_for("AAPL")};
    engine.start();

    publish_market(market_event("AAPL"));
    publish_market(market_event("AAPL"));

    const std::vector<domain::TradeSignal> signals = bus.signals();
    EXPECT_EQ(strategy_ids_of(signals),
              (StrategyIds{"zeta", "zeta", "alpha", "zeta", "zeta", "alpha"}));
    EXPECT_EQ(id_values_of(signals), (std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(signals[0].symbol, "AAPL");
    EXPECT_EQ(signals[1].symbol, "MSFT") << "a strategy's own emission order is kept";
}

TEST_F(StrategyEngineTest, KeepsSignalIdsUniqueAndIncreasingAcrossRestarts) {
    const auto a = add("a");
    a->emits = {signal_for("AAPL")};

    engine.start();
    publish_market(market_event("AAPL"));
    publish_market(market_event("AAPL"));
    engine.stop();
    engine.start();
    publish_market(market_event("AAPL"));
    engine.stop();
    engine.start();
    publish_market(market_event("AAPL"));

    EXPECT_EQ(id_values_of(bus.signals()), (std::vector<std::uint64_t>{1, 2, 3, 4}));
}

// --- Failed publication ------------------------------------------------------

TEST_F(StrategyEngineTest, ARejectedPublicationIsCountedAndNeverReportedAsDelivered) {
    const auto a = add("a");
    a->emits = {signal_for("AAPL")};
    engine.start();
    bus.faults.reject_publish = true;

    // Direct call: the double would also refuse a market event published through it.
    EXPECT_NO_THROW(engine.on_market_event(market_event("AAPL")));

    EXPECT_EQ(engine.stats().signals_rejected, 1u);
    EXPECT_EQ(engine.stats().signals_published, 0u) << "a refusal is not a delivery";
    EXPECT_EQ(engine.stats().publish_errors, 0u);
    EXPECT_EQ(engine.stats().strategy_errors, 0u) << "a full bus is not the strategy's fault";
    EXPECT_EQ(engine.strategy_stats()[0].signals_rejected, 1u);
    EXPECT_EQ(engine.strategy_stats()[0].signals_published, 0u);
    EXPECT_TRUE(bus.signals().empty());

    ASSERT_EQ(bus.rejected().size(), 1u) << "the bus was offered the signal";
    const auto* offered = std::get_if<domain::TradeSignal>(&bus.rejected()[0].payload);
    ASSERT_NE(offered, nullptr);
    EXPECT_EQ(offered->id.value, 1u);

    // The engine does not retry or buffer. When the bus recovers, the next
    // signal goes through under a fresh id (ids are never reused).
    bus.faults.reject_publish = false;
    engine.on_market_event(market_event("AAPL"));
    EXPECT_EQ(engine.stats().signals_published, 1u);
    EXPECT_EQ(engine.stats().signals_rejected, 1u);
    ASSERT_EQ(bus.signals().size(), 1u);
    EXPECT_EQ(bus.signals()[0].id.value, 2u);
}

TEST_F(StrategyEngineTest, APublishThatThrowsIsAbsorbedAndCountedAsABusFault) {
    const auto a = add("a");
    const auto b = add("b");
    a->emits = {signal_for("AAPL"), signal_for("MSFT")};
    b->emits = {signal_for("AAPL")};
    engine.start();
    bus.faults.throw_on_publish = true;

    EXPECT_NO_THROW(engine.on_market_event(market_event("AAPL")));

    EXPECT_EQ(a->received.size(), 1u);
    EXPECT_EQ(b->received.size(), 1u);
    EXPECT_EQ(engine.stats().publish_errors, 3u) << "every emit was attempted, even after a failure";
    EXPECT_EQ(engine.stats().signals_published, 0u);
    EXPECT_EQ(engine.stats().strategy_errors, 0u) << "the bus failed, not the strategies";

    const auto per_strategy = engine.strategy_stats();
    EXPECT_EQ(per_strategy[0].publish_errors, 2u);
    EXPECT_EQ(per_strategy[0].errors, 0u);
    EXPECT_EQ(per_strategy[0].last_error, "publish: RecordingEventBus: injected publish() failure");
    EXPECT_EQ(per_strategy[1].publish_errors, 1u);
}
