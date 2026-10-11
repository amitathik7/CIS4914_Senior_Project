#include <chrono>

#include <gtest/gtest.h>

#include "trading_engine/common/errors.hpp"
#include "trading_engine/events/kafka/market_event_codec.hpp"

namespace common = trading_engine::common;
namespace domain = trading_engine::domain;
namespace kafka = trading_engine::events::kafka;

TEST(MarketEventCodec, RoundTripsQuote)
{
    domain::MarketEvent original{};

    original.symbol = "AAPL";
    original.type = domain::MarketEventType::Quote;
    original.exchange_time = common::Timestamp{common::Duration{1'000}};
    original.ingest_time = common::Timestamp{common::Duration{2'000}};
    original.bid = common::Price::from_micros(150'250'000);
    original.ask = common::Price::from_micros(150'300'000);
    original.bid_size = 100;
    original.ask_size = 125;
    original.sequence = 42;

    const std::string encoded = kafka::serialize_market_event(original);
    const domain::MarketEvent decoded =
        kafka::deserialize_market_event(encoded);

    EXPECT_EQ(decoded.symbol, original.symbol);
    EXPECT_EQ(decoded.type, original.type);
    EXPECT_EQ(decoded.exchange_time, original.exchange_time);
    EXPECT_EQ(decoded.ingest_time, original.ingest_time);
    EXPECT_EQ(decoded.bid, original.bid);
    EXPECT_EQ(decoded.ask, original.ask);
    EXPECT_EQ(decoded.bid_size, original.bid_size);
    EXPECT_EQ(decoded.ask_size, original.ask_size);
    EXPECT_EQ(decoded.sequence, original.sequence);
}

TEST(MarketEventCodec, RoundTripsBar)
{
    domain::MarketEvent original{};

    original.symbol = "AAPL";
    original.type = domain::MarketEventType::Bar;
    original.open = common::Price::from_micros(150'000'000);
    original.high = common::Price::from_micros(151'000'000);
    original.low = common::Price::from_micros(149'500'000);
    original.price = common::Price::from_micros(150'750'000);
    original.volume = 25000;
    original.bar_interval = std::chrono::minutes{1};
    original.sequence = 43;

    const std::string encoded = kafka::serialize_market_event(original);
    const domain::MarketEvent decoded =
        kafka::deserialize_market_event(encoded);

    EXPECT_EQ(decoded.symbol, original.symbol);
    EXPECT_EQ(decoded.type, original.type);
    EXPECT_EQ(decoded.open, original.open);
    EXPECT_EQ(decoded.high, original.high);
    EXPECT_EQ(decoded.low, original.low);
    EXPECT_EQ(decoded.price, original.price);
    EXPECT_EQ(decoded.volume, original.volume);
    EXPECT_EQ(decoded.bar_interval, original.bar_interval);
    EXPECT_EQ(decoded.sequence, original.sequence);
}

TEST(MarketEventCodec, RejectsMalformedJson)
{
    EXPECT_THROW(
        kafka::deserialize_market_event("{not valid json}"),
        common::ValidationError);
}

TEST(MarketEventCodec, RejectsUnsupportedSchemaVersion)
{
    const std::string json =
        R"({"schema_version":2,"type":"trade","symbol":"AAPL","exchange_time_ns":0,"ingest_time_ns":0,"sequence":1})";

    EXPECT_THROW(
        kafka::deserialize_market_event(json),
        common::ValidationError);
}