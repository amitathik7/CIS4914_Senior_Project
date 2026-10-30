// The event wire format (events/event_codec.hpp): every payload round-trips
// exactly, the text follows the ADR 0006 encoding rules, and decode() refuses
// anything it cannot read unambiguously.

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/price_literals.hpp"
#include "trading_engine/common/timestamp_text.hpp"
#include "trading_engine/events/event_codec.hpp"

namespace {

namespace ev  = trading_engine::events;
namespace dom = trading_engine::domain;
namespace tec = trading_engine::common;
using namespace std::chrono;
using namespace trading_engine::test_support::literals;

tec::Timestamp at(int minute) {
    return tec::Timestamp{sys_days{2026y / January / 5}} + 14h + minutes{minute} + 123456789ns;
}

ev::WireEnvelope wrap(ev::EventType type, ev::EventPayload payload) {
    ev::WireEnvelope w;
    w.origin            = "abc123";
    w.event.type        = type;
    w.event.sequence    = 18'446'744'073'709'551'615ULL;   // uint64 max survives
    w.event.enqueued_at = at(9);
    w.event.payload     = std::move(payload);
    return w;
}

dom::MarketEvent full_bar() {
    dom::MarketEvent e;
    e.symbol        = "AAPL";
    e.exchange_time = at(0);
    e.ingest_time   = at(1);
    e.type          = dom::MarketEventType::Bar;
    e.price         = 101.25_px;
    e.open          = 100.5_px;
    e.high          = 102_px;
    e.low           = 0.000001_px;
    e.bid           = 101.2_px;
    e.ask           = 101.3_px;
    e.size          = 7;
    e.volume        = 15'000;
    e.sequence      = 42;
    return e;
}

dom::TradeSignal full_signal() {
    dom::TradeSignal s;
    s.id                 = tec::SignalId{17};
    s.strategy_id        = "sma_crossover";
    s.symbol             = "MSFT";
    s.side               = dom::SignalSide::Sell;
    s.created_at         = at(2);
    s.requested_quantity = 3;
    s.target_exposure    = 0.05;
    s.confidence         = 0.1 + 0.2;   // not exactly representable: must still round-trip
    s.metadata           = {{"trigger", "short_crossed_below_long"},
                            {"quote\"and\\slash", "line\nbreak é \xF0\x9F\x93\x88"}};
    s.order_type         = dom::OrderType::Limit;
    s.limit_price        = 410.5_px;
    return s;
}

dom::Order full_order() {
    dom::Order o;
    o.id                 = tec::OrderId{5};
    o.run_id             = tec::RunId{2};
    o.origin_signal      = tec::SignalId{17};
    o.strategy_id        = "mean_reversion";
    o.symbol             = "MSFT";
    o.side               = dom::OrderSide::Sell;
    o.type               = dom::OrderType::Limit;
    o.quantity           = 10;
    o.limit_price        = 410.5_px;
    o.status             = dom::OrderStatus::PartiallyFilled;
    o.created_at         = at(3);
    o.updated_at         = at(4);
    o.filled_quantity    = 4;
    o.average_fill_price = 410.55_px;
    o.reject_reason      = "none, really";
    return o;
}

dom::Fill full_fill() {
    dom::Fill f;
    f.id              = tec::FillId{9};
    f.order_id        = tec::OrderId{5};
    f.run_id          = tec::RunId{2};
    f.symbol          = "MSFT";
    f.filled_quantity = 4;
    f.fill_price      = 410.55_px;
    f.fees            = 0.01_px;
    f.slippage        = -0.05_px;   // negative decimals are allowed on the wire
    f.filled_at       = at(5);
    f.status          = dom::FillStatus::PartiallyFilled;
    f.sequence        = 1;
    return f;
}

template <class T>
void expect_round_trip(ev::EventType type, const T& payload) {
    const ev::WireEnvelope in = wrap(type, payload);
    const std::string text    = ev::encode(in);
    const ev::WireEnvelope out = ev::decode(text);
    EXPECT_EQ(out.origin, in.origin);
    EXPECT_EQ(out.event.type, in.event.type);
    EXPECT_EQ(out.event.sequence, in.event.sequence);
    EXPECT_EQ(out.event.enqueued_at, in.event.enqueued_at);
    ASSERT_TRUE(std::holds_alternative<T>(out.event.payload)) << text;
    // Re-encoding the decoded event gives identical bytes: nothing was lost.
    EXPECT_EQ(ev::encode(out), text);
}

TEST(EventCodec, MarketEventRoundTrips) {
    expect_round_trip(ev::EventType::MarketData, full_bar());
    const auto out = ev::decode(ev::encode(wrap(ev::EventType::MarketData, full_bar())));
    const auto& e  = std::get<dom::MarketEvent>(out.event.payload);
    EXPECT_EQ(e.symbol, "AAPL");
    EXPECT_EQ(e.exchange_time, at(0));
    EXPECT_EQ(e.low, 0.000001_px);
    EXPECT_EQ(e.volume, 15'000);
    EXPECT_EQ(e.sequence, 42U);
}

TEST(EventCodec, SignalRoundTripsIncludingDoublesAndUnicodeMetadata) {
    expect_round_trip(ev::EventType::Signal, full_signal());
    const auto out = ev::decode(ev::encode(wrap(ev::EventType::Signal, full_signal())));
    const auto& s  = std::get<dom::TradeSignal>(out.event.payload);
    EXPECT_EQ(s.confidence, 0.1 + 0.2);
    EXPECT_EQ(s.metadata, full_signal().metadata);
    EXPECT_EQ(s.limit_price, 410.5_px);
    EXPECT_EQ(s.order_type, dom::OrderType::Limit);
}

TEST(EventCodec, OrderAndFillRoundTrip) {
    expect_round_trip(ev::EventType::Order, full_order());
    expect_round_trip(ev::EventType::Fill, full_fill());
    const auto out = ev::decode(ev::encode(wrap(ev::EventType::Fill, full_fill())));
    EXPECT_EQ(std::get<dom::Fill>(out.event.payload).slippage, -0.05_px);
}

dom::RiskDecisionReport full_risk_decision() {
    dom::RiskDecisionReport r;
    r.id                 = 7;
    r.run_id             = tec::RunId{3};
    r.signal_id          = tec::SignalId{42};
    r.decided_at         = at(6);
    r.symbol             = "MSFT";
    r.side               = dom::SignalSide::Buy;
    r.strategy_id        = "sma_crossover";
    r.outcome            = dom::RiskOutcome::Rejected;
    r.requested_quantity = 2000;
    r.approved_quantity  = 0;
    r.reason             = "max_position_notional";
    r.checks.push_back({"max_order_quantity", true, "resized to 1000", std::nullopt, std::nullopt});
    r.checks.push_back({"max_position_notional", false, "position would be 406480", "25000", "406480"});
    r.order_id = tec::OrderId{9};
    return r;
}

TEST(EventCodec, RiskDecisionRoundTripsWithTheConsoleFieldNames) {
    expect_round_trip(ev::EventType::RiskDecision, full_risk_decision());
    const std::string text = ev::encode(wrap(ev::EventType::RiskDecision, full_risk_decision()));
    EXPECT_NE(text.find("\"type\":\"risk_decision\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"outcome\":\"rejected\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"checks\":[{\"policy\":\"max_order_quantity\",\"passed\":true"),
              std::string::npos) << text;
    const auto out = std::get<dom::RiskDecisionReport>(ev::decode(text).event.payload);
    ASSERT_EQ(out.checks.size(), 2U);
    EXPECT_FALSE(out.checks[1].passed);
    EXPECT_EQ(out.checks[1].limit, "25000");
    EXPECT_FALSE(out.checks[0].observed.has_value());
    EXPECT_EQ(out.order_id, tec::OrderId{9});
    EXPECT_EQ(ev::partition_key(wrap(ev::EventType::RiskDecision, out).event), "MSFT");

    dom::RiskDecisionReport bare;   // approved unchanged: no reason, no checks, no order
    bare.outcome = dom::RiskOutcome::Approved;
    const std::string bare_text = ev::encode(wrap(ev::EventType::RiskDecision, bare));
    EXPECT_NE(bare_text.find("\"checks\":[]"), std::string::npos) << bare_text;
    EXPECT_EQ(bare_text.find("reason"), std::string::npos) << bare_text;
    const auto bare_out = std::get<dom::RiskDecisionReport>(ev::decode(bare_text).event.payload);
    EXPECT_TRUE(bare_out.checks.empty());
    EXPECT_FALSE(bare_out.reason.has_value());
}

TEST(EventCodec, MinimalPayloadsOmitAbsentOptionalsInsteadOfWritingNull) {
    dom::MarketEvent bare;
    bare.symbol = "X";
    bare.type   = dom::MarketEventType::Trade;
    const std::string text = ev::encode(wrap(ev::EventType::MarketData, bare));
    EXPECT_EQ(text.find("null"), std::string::npos);
    EXPECT_EQ(text.find("\"bid\""), std::string::npos);
    const ev::WireEnvelope decoded = ev::decode(text);
    const auto& e = std::get<dom::MarketEvent>(decoded.event.payload);
    EXPECT_FALSE(e.price.has_value());
    EXPECT_FALSE(e.size.has_value());
}

TEST(EventCodec, FollowsTheAdr0006EncodingRules) {
    const std::string text = ev::encode(wrap(ev::EventType::MarketData, full_bar()));
    EXPECT_NE(text.find("\"price\":\"101.25\""), std::string::npos) << "prices are decimal strings";
    EXPECT_NE(text.find("\"volume\":15000"), std::string::npos) << "quantities are integers";
    EXPECT_NE(text.find("\"exchange_time\":\"2026-01-05T14:00:00.123456789Z\""), std::string::npos);
    EXPECT_NE(text.find("\"type\":\"bar\""), std::string::npos);
    EXPECT_EQ(text.rfind("{\"schema\":1,\"origin\":\"abc123\"", 0), 0U);
}

TEST(EventCodec, PartitionKeyIsTheSymbol) {
    EXPECT_EQ(ev::partition_key(wrap(ev::EventType::Fill, full_fill()).event), "MSFT");
    EXPECT_EQ(ev::partition_key(wrap(ev::EventType::MarketData, full_bar()).event), "AAPL");
}

TEST(EventCodec, EncodeRefusesATypeThatDisagreesWithThePayload) {
    EXPECT_THROW((void)ev::encode(wrap(ev::EventType::Signal, full_bar())), std::invalid_argument);
}

TEST(EventCodec, DecodeRefusesMalformedOrAmbiguousMessages) {
    const std::string good = ev::encode(wrap(ev::EventType::Fill, full_fill()));
    const auto replaced = [&](const std::string& from, const std::string& to) {
        std::string s = good;
        const auto pos = s.find(from);
        EXPECT_NE(pos, std::string::npos) << from;
        return s.replace(pos, from.size(), to);
    };

    for (const std::string& bad : {
             std::string{},
             std::string{"not json"},
             std::string{"[]"},
             good + "x",
             good.substr(0, good.size() - 1),
             replaced("\"schema\":1", "\"schema\":2"),
             replaced("\"type\":\"fill\"", "\"type\":\"heartbeat\""),
             replaced("\"fill_price\":\"410.55\"", "\"fill_price\":410.55"),
             replaced("\"fill_price\":\"410.55\"", "\"fill_price\":\"4.1e2\""),
             replaced("\"fill_price\":\"410.55\"", "\"fill_price\":\"0.0000001\""),
             replaced("\"filled_quantity\":4", "\"filled_quantity\":4.5"),
             replaced("\"filled_quantity\":4", "\"filled_quantity\":\"4\""),
             replaced("\"status\":\"partially_filled\"", "\"status\":\"PartiallyFilled\""),
             replaced("2026-01-05T14:05:00.123456789Z", "2026-01-05T14:05:00+01:00"),
             replaced("\"symbol\":\"MSFT\",", ""),
             replaced("\"order_id\":5", "\"order_id\":-5"),
             replaced("\"run_id\":2", "\"run_id\":2,\"run_id\":3"),
         }) {
        EXPECT_THROW((void)ev::decode(bad), ev::DecodeError) << bad;
    }
}

TEST(EventCodec, DecodeIgnoresUnknownMembersForForwardCompatibility) {
    std::string text = ev::encode(wrap(ev::EventType::Fill, full_fill()));
    text.insert(1, "\"added_in_v1_1\":{\"nested\":[1,true,null]},");
    EXPECT_NO_THROW((void)ev::decode(text));
}

}  // namespace
