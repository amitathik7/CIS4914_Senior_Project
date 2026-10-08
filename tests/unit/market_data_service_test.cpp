#include <vector>

#include <gtest/gtest.h>

#include <trading_engine/common/clock.hpp>
#include <trading_engine/events/event_bus.hpp>
#include <trading_engine/events/event_bus.hpp>
#include <trading_engine/market_data/market_data_service.hpp>

namespace
{
    class FakeEventBus final : public trading_engine::events::IEventBus
    {
    public:
        bool publish(trading_engine::events::Event event) override
        {
            published_events.push_back(event);
            return true;
        }

        trading_engine::common::SubscriptionId subscribe(trading_engine::events::EventType, trading_engine::events::EventHandler) override { return {}; };
        void unsubscribe(trading_engine::common::SubscriptionId) override{};

        void start() override {};
        void request_shutdown() override {};
        void wait_until_drained() override {};

        [[nodiscard]] std::size_t depth() const override { return published_events.size(); };

        std::vector<trading_engine::events::Event> published_events;
    };

    trading_engine::domain::MarketEvent valid_trade(trading_engine::common::Price price, const trading_engine::common::Symbol &symbol)
    {
        trading_engine::domain::MarketEvent market_event;

        market_event.price = price;
        market_event.symbol = symbol;
        market_event.type = trading_engine::domain::MarketEventType::Trade;

        return market_event;
    }

    const trading_engine::domain::MarketEvent get_payload(const trading_engine::events::Event event)
    {
        return std::get<trading_engine::domain::MarketEvent>(event.payload);
    }

    class MarketDataServiceTest : public ::testing::Test
    {
    protected:
        FakeEventBus bus;
        trading_engine::common::ManualClock clock{trading_engine::common::Timestamp{}};
        trading_engine::market_data::MarketDataService service{bus, clock, trading_engine::market_data::MarketDataPolicy{}};
    };
}

TEST_F(MarketDataServiceTest, ValidTradeIsStampedAndPublished)
{
    service.on_market_event(valid_trade(trading_engine::common::Price::from_double(200.00), "AAPL"));

    ASSERT_EQ(bus.published_events.size(), 1u);
    EXPECT_EQ(bus.published_events[0].type, trading_engine::events::EventType::MarketData);

    const auto &out = get_payload(bus.published_events[0]);
    EXPECT_EQ(out.symbol, "AAPL");
    EXPECT_EQ(out.sequence, 1u);
    EXPECT_EQ(out.ingest_time, clock.now());
    EXPECT_EQ(service.stats().accepted, 1u);
    EXPECT_EQ(service.stats().rejected, 0u);
}

TEST_F(MarketDataServiceTest, InvalidPriceIsRejectedAndNotPublished)
{
    service.on_market_event(valid_trade(trading_engine::common::Price::from_double(-200.00), "AAPL"));

    EXPECT_TRUE(bus.published_events.empty());
    EXPECT_EQ(service.stats().accepted, 0u);
    EXPECT_EQ(service.stats().rejected, 1u);
}