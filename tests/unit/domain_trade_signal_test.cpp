// Shape/compatibility checks for the ADR 0002 (Proposed, not Accepted)
// additions: TradeSignal::order_type, TradeSignal::limit_price, and the new
// SignalCancelRequest type. These are NOT validation tests -- neither type
// enforces any invariant in code yet (see the ADR). They only prove the
// structs hold and return the fields they're given, and that
// default-construction is unaffected by the new fields.

#include <gtest/gtest.h>

#include "trading_engine/domain/signal_cancel_request.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace domain = trading_engine::domain;
namespace common = trading_engine::common;

TEST(TradeSignalOrderShape, DefaultConstructedHasNoOrderTypeOrLimitPrice) {
    // A signal built the way every pre-ADR-0002 signal is built (nothing but
    // the original fields) must still come out with the new fields empty --
    // this is what "additive, not a redefinition" means in practice.
    const domain::TradeSignal signal{};
    EXPECT_FALSE(signal.order_type.has_value());
    EXPECT_FALSE(signal.limit_price.has_value());
    EXPECT_EQ(signal.side, domain::SignalSide::Flat);
}

TEST(TradeSignalOrderShape, ExistingExposureStyleSignalIsUnaffected) {
    // The exposure-based construction style that predates this ADR still
    // works and still leaves the new fields empty.
    domain::TradeSignal signal{};
    signal.symbol = "AAPL";
    signal.side = domain::SignalSide::Buy;
    signal.target_exposure = 0.05;

    EXPECT_TRUE(signal.target_exposure.has_value());
    EXPECT_FALSE(signal.requested_quantity.has_value());
    EXPECT_FALSE(signal.order_type.has_value());
    EXPECT_FALSE(signal.limit_price.has_value());
}

TEST(TradeSignalOrderShape, CanRepresentAMarketShapedSignal) {
    domain::TradeSignal signal{};
    signal.symbol = "AAPL";
    signal.side = domain::SignalSide::Buy;
    signal.requested_quantity = 100.0;
    signal.order_type = domain::OrderType::Market;

    EXPECT_EQ(signal.order_type, domain::OrderType::Market);
    EXPECT_FALSE(signal.limit_price.has_value());
}

TEST(TradeSignalOrderShape, CanRepresentALimitShapedSignal) {
    domain::TradeSignal signal{};
    signal.symbol = "AAPL";
    signal.side = domain::SignalSide::Sell;
    signal.requested_quantity = 50.0;
    signal.order_type = domain::OrderType::Limit;
    signal.limit_price = 152.75;

    EXPECT_EQ(signal.order_type, domain::OrderType::Limit);
    ASSERT_TRUE(signal.limit_price.has_value());
    EXPECT_DOUBLE_EQ(*signal.limit_price, 152.75);
}

TEST(SignalCancelRequestShape, CarriesItsOwnIdSeparatelyFromItsTarget) {
    domain::SignalCancelRequest cancel{};
    cancel.id = common::SignalId{57};
    cancel.target_signal_id = common::SignalId{42};
    cancel.strategy_id = "sma_crossover";
    cancel.symbol = "AAPL";

    EXPECT_NE(cancel.id, cancel.target_signal_id);
    EXPECT_EQ(cancel.target_signal_id, common::SignalId{42});
}
