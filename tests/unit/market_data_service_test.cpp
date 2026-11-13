#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include <trading_engine/common/clock.hpp>
#include <trading_engine/events/event_bus.hpp>
#include <trading_engine/market_data/market_data_service.hpp>

namespace {
class FakeEventBus final : public trading_engine::events::IEventBus {
  public:
    bool publish(trading_engine::events::Event event) override {
        published_events.push_back(event);
        return true;
    }

    trading_engine::common::SubscriptionId
    subscribe(trading_engine::events::EventType, trading_engine::events::EventHandler) override {
        return {};
    };
    void unsubscribe(trading_engine::common::SubscriptionId) override{};

    void start() override {};
    void request_shutdown() override {};
    void wait_until_drained() override {};

    [[nodiscard]] std::size_t depth() const override { return published_events.size(); };

    std::vector<trading_engine::events::Event> published_events;
};

trading_engine::domain::MarketEvent valid_trade(trading_engine::common::Price price,
                                                const trading_engine::common::Symbol &symbol) {
    trading_engine::domain::MarketEvent market_event;

    market_event.price = price;
    market_event.symbol = symbol;
    market_event.type = trading_engine::domain::MarketEventType::Trade;

    return market_event;
}

// Builds an event with no price fields set, so the positive-price rule cannot be what accepts or
// rejects it.
trading_engine::domain::MarketEvent event_of(const trading_engine::common::Symbol &symbol,
                                             trading_engine::domain::MarketEventType type) {
    trading_engine::domain::MarketEvent market_event;

    market_event.symbol = symbol;
    market_event.type = type;

    return market_event;
}

trading_engine::domain::MarketEvent trade_of(const trading_engine::common::Symbol &symbol) {
    return event_of(symbol, trading_engine::domain::MarketEventType::Trade);
}

const trading_engine::domain::MarketEvent get_payload(const trading_engine::events::Event event) {
    return std::get<trading_engine::domain::MarketEvent>(event.payload);
}

class MarketDataServiceTest : public ::testing::Test {
  protected:
    FakeEventBus bus;
    trading_engine::common::ManualClock clock{trading_engine::common::Timestamp{}};
    trading_engine::market_data::MarketDataService service{
        bus, clock, trading_engine::market_data::MarketDataPolicy{}};
};
} // namespace

TEST_F(MarketDataServiceTest, ValidTradeIsStampedAndPublished) {
    service.on_market_event(
        valid_trade(trading_engine::common::Price::from_double(200.00), "AAPL"));

    ASSERT_EQ(bus.published_events.size(), 1u);
    EXPECT_EQ(bus.published_events[0].type, trading_engine::events::EventType::MarketData);

    const auto &out = get_payload(bus.published_events[0]);
    EXPECT_EQ(out.symbol, "AAPL");
    EXPECT_EQ(out.sequence, 1u);
    EXPECT_EQ(out.ingest_time, clock.now());
    EXPECT_EQ(service.stats().accepted, 1u);
    EXPECT_EQ(service.stats().rejected, 0u);
}

TEST_F(MarketDataServiceTest, InvalidPriceIsRejectedAndNotPublished) {
    service.on_market_event(
        valid_trade(trading_engine::common::Price::from_double(-200.00), "AAPL"));

    EXPECT_TRUE(bus.published_events.empty());
    EXPECT_EQ(service.stats().accepted, 0u);
    EXPECT_EQ(service.stats().rejected, 1u);
}

TEST_F(MarketDataServiceTest, EmptySymbolIsRejectedAndNotPublished) {
    service.on_market_event(trade_of(""));

    EXPECT_TRUE(bus.published_events.empty());
    EXPECT_EQ(service.stats().accepted, 0u);
    EXPECT_EQ(service.stats().rejected, 1u);
}

TEST_F(MarketDataServiceTest, UnknownTypeIsRejectedAndNotPublished) {
    service.on_market_event(event_of("AAPL", trading_engine::domain::MarketEventType::Unknown));

    EXPECT_TRUE(bus.published_events.empty());
    EXPECT_EQ(service.stats().accepted, 0u);
    EXPECT_EQ(service.stats().rejected, 1u);
}

TEST_F(MarketDataServiceTest, SequenceIncrementsPerSymbol) {
    service.on_market_event(trade_of("AAPL"));
    service.on_market_event(trade_of("AAPL"));
    service.on_market_event(trade_of("AAPL"));

    ASSERT_EQ(bus.published_events.size(), 3u);
    EXPECT_EQ(get_payload(bus.published_events[0]).sequence, 1u);
    EXPECT_EQ(get_payload(bus.published_events[1]).sequence, 2u);
    EXPECT_EQ(get_payload(bus.published_events[2]).sequence, 3u);
}

TEST_F(MarketDataServiceTest, SequencesAreIndependentAcrossSymbols) {
    service.on_market_event(trade_of("AAPL"));
    service.on_market_event(trade_of("MSFT"));
    service.on_market_event(trade_of("AAPL"));
    service.on_market_event(trade_of("MSFT"));

    ASSERT_EQ(bus.published_events.size(), 4u);
    EXPECT_EQ(get_payload(bus.published_events[0]).sequence, 1u); // AAPL
    EXPECT_EQ(get_payload(bus.published_events[1]).sequence, 1u); // MSFT
    EXPECT_EQ(get_payload(bus.published_events[2]).sequence, 2u); // AAPL
    EXPECT_EQ(get_payload(bus.published_events[3]).sequence, 2u); // MSFT
}

TEST_F(MarketDataServiceTest, RejectedEventDoesNotAdvanceSequence) {
    service.on_market_event(trade_of("AAPL"));
    service.on_market_event(
        event_of("AAPL", trading_engine::domain::MarketEventType::Unknown)); // rejected
    service.on_market_event(trade_of("AAPL"));

    ASSERT_EQ(bus.published_events.size(), 2u);
    EXPECT_EQ(get_payload(bus.published_events[0]).sequence, 1u);
    EXPECT_EQ(get_payload(bus.published_events[1]).sequence, 2u);
}

TEST_F(MarketDataServiceTest, IngestTimeIsStampedFromTheClockAtIngest) {
    clock.advance(std::chrono::seconds(5));
    const auto expected = trading_engine::common::Timestamp{} + std::chrono::seconds(5);

    service.on_market_event(trade_of("AAPL"));

    ASSERT_EQ(bus.published_events.size(), 1u);
    EXPECT_EQ(get_payload(bus.published_events[0]).ingest_time, expected);
}

TEST_F(MarketDataServiceTest, ExchangeTimeIsNotModified) {
    clock.advance(std::chrono::seconds(5));
    const auto exchange_time = trading_engine::common::Timestamp{} + std::chrono::seconds(2);

    auto event = trade_of("AAPL");
    event.exchange_time = exchange_time;
    service.on_market_event(event);

    ASSERT_EQ(bus.published_events.size(), 1u);
    EXPECT_EQ(get_payload(bus.published_events[0]).exchange_time, exchange_time);
}

TEST_F(MarketDataServiceTest, StatsCountAcceptedAndRejectedAcrossAMixedRun) {
    service.on_market_event(trade_of("AAPL")); // accepted
    service.on_market_event(trade_of(""));     // rejected: empty symbol
    service.on_market_event(trade_of("MSFT")); // accepted
    service.on_market_event(
        event_of("AAPL", trading_engine::domain::MarketEventType::Unknown)); // rejected: unknown

    EXPECT_EQ(service.stats().accepted, 2u);
    EXPECT_EQ(service.stats().rejected, 2u);
    EXPECT_EQ(service.stats().accepted + service.stats().rejected, 4u);
    EXPECT_EQ(bus.published_events.size(), 2u);
}
