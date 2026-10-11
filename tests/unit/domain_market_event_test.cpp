#include <chrono>

#include <gtest/gtest.h>

#include "trading_engine/domain/market_event.hpp"

namespace domain = trading_engine::domain;
namespace common = trading_engine::common;

TEST(MarketEventShape, QuoteCarriesBidAskPricesAndSizes)
{
    domain::MarketEvent event{};

    event.symbol = "AAPL";
    event.type = domain::MarketEventType::Quote;
    event.bid = common::Price::from_micros(150'250'000);
    event.ask = common::Price::from_micros(150'300'000);
    event.bid_size = 100;
    event.ask_size = 125;

    EXPECT_EQ(event.symbol, "AAPL");
    EXPECT_EQ(event.type, domain::MarketEventType::Quote);

    ASSERT_TRUE(event.bid.has_value());
    ASSERT_TRUE(event.ask.has_value());
    ASSERT_TRUE(event.bid_size.has_value());
    ASSERT_TRUE(event.ask_size.has_value());

    EXPECT_EQ(*event.bid, common::Price::from_micros(150'250'000));
    EXPECT_EQ(*event.ask, common::Price::from_micros(150'300'000));
    EXPECT_EQ(*event.bid_size, 100);
    EXPECT_EQ(*event.ask_size, 125);
}

TEST(MarketEventShape, BarCarriesOhlcvAndInterval)
{
    domain::MarketEvent event{};

    event.symbol = "AAPL";
    event.type = domain::MarketEventType::Bar;
    event.open = common::Price::from_micros(150'000'000);
    event.high = common::Price::from_micros(151'000'000);
    event.low = common::Price::from_micros(149'500'000);
    event.price = common::Price::from_micros(150'750'000);
    event.volume = 25000;
    event.bar_interval = std::chrono::minutes{1};

    ASSERT_TRUE(event.open.has_value());
    ASSERT_TRUE(event.high.has_value());
    ASSERT_TRUE(event.low.has_value());
    ASSERT_TRUE(event.price.has_value());
    ASSERT_TRUE(event.volume.has_value());
    ASSERT_TRUE(event.bar_interval.has_value());

    EXPECT_EQ(*event.open, common::Price::from_micros(150'000'000));
    EXPECT_EQ(*event.high, common::Price::from_micros(151'000'000));
    EXPECT_EQ(*event.low, common::Price::from_micros(149'500'000));
    EXPECT_EQ(*event.price, common::Price::from_micros(150'750'000));
    EXPECT_EQ(*event.volume, 25000);
    EXPECT_EQ(*event.bar_interval,
              std::chrono::duration_cast<common::Duration>(std::chrono::minutes{1}));
}