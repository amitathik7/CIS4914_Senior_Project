// Shape/compatibility checks for the ADR 0004 (Proposed, not Accepted)
// additions: Fill::status, Fill::sequence, Order::reject_reason and
// Order::run_id. These are
// NOT validation tests -- neither type enforces any invariant in code yet (see
// the ADR's scope exclusions). They only prove the structs hold and return the
// fields they're given, that default construction is unaffected by the new
// fields, and that the enum spellings match the serialized values the ADR
// documents.

#include <gtest/gtest.h>

#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/order.hpp"

namespace domain = trading_engine::domain;
namespace common = trading_engine::common;

TEST(FillShape, DefaultConstructedIsCompleteAndUnsequenced) {
    // A Fill built the way every pre-ADR-0004 fill is built must still come
    // out usable -- this is what "additive, not a redefinition" means.
    const domain::Fill fill{};
    EXPECT_EQ(fill.status, domain::FillStatus::Filled);
    EXPECT_EQ(fill.sequence, 0u);
    EXPECT_FALSE(fill.id.valid());
    EXPECT_DOUBLE_EQ(fill.fees, 0.0);
}

TEST(FillShape, DirectionIsCarriedBySignNotBySeparateField) {
    // ADR 0004 section 4: the sign of filled_quantity is the single source of
    // truth for direction in C++. A sell is negative; there is deliberately no
    // `side` member that could contradict it.
    domain::Fill sell{};
    sell.symbol = "AAPL";
    sell.filled_quantity = -50.0;

    EXPECT_LT(sell.filled_quantity, 0.0);
}

TEST(FillShape, CanRepresentAPartialFillSeries) {
    // Two fills against one order: the first leaves quantity outstanding, the
    // second completes it. Sequence is 1-based per order (section 9.2).
    domain::Fill first{};
    first.id = common::FillId{7002};
    first.order_id = common::OrderId{3306};
    first.filled_quantity = -30.0;
    first.status = domain::FillStatus::PartiallyFilled;
    first.sequence = 1;

    domain::Fill second{};
    second.id = common::FillId{7003};
    second.order_id = common::OrderId{3306};
    second.filled_quantity = -20.0;
    second.status = domain::FillStatus::Filled;
    second.sequence = 2;

    EXPECT_EQ(first.order_id, second.order_id);
    EXPECT_NE(first.id, second.id);
    EXPECT_EQ(second.sequence, first.sequence + 1);
    EXPECT_EQ(second.status, domain::FillStatus::Filled);
}

TEST(FillShape, GrossPriceAndFeesAreSeparateFields) {
    // ADR 0004 section 3: fill_price is gross, fees are separate, and the cash
    // delta is -(signed_qty * price) - fees. Computed here only to pin the
    // documented formula against the struct's field layout; the Portfolio
    // Manager implements it, not this test.
    domain::Fill buy{};
    buy.filled_quantity = 100.0;
    buy.fill_price = 150.02;
    buy.fees = 1.0;

    const auto cash_delta = -(buy.filled_quantity * buy.fill_price) - buy.fees;
    EXPECT_DOUBLE_EQ(cash_delta, -15003.0);
}

TEST(FillStatusStrings, MatchTheSerializedValuesInTheAdr) {
    EXPECT_EQ(domain::to_string(domain::FillStatus::Filled), "filled");
    EXPECT_EQ(domain::to_string(domain::FillStatus::PartiallyFilled),
              "partially_filled");
}

TEST(FillStatusStrings, AgreeWithTheMatchingOrderStatusSpellings) {
    // Section 5: the same word means the same thing on both structs.
    EXPECT_EQ(domain::to_string(domain::FillStatus::Filled),
              domain::to_string(domain::OrderStatus::Filled));
    EXPECT_EQ(domain::to_string(domain::FillStatus::PartiallyFilled),
              domain::to_string(domain::OrderStatus::PartiallyFilled));
}

TEST(OrderRejectionShape, CarriesAReasonAndNoFills) {
    // Section 6: an execution rejection is reported as an Order, not a Fill --
    // there is no fill price or filled quantity to report.
    domain::Order rejected{};
    rejected.id = common::OrderId{3307};
    rejected.symbol = "AAPL";
    rejected.status = domain::OrderStatus::Rejected;
    rejected.reject_reason = "no market context for symbol";

    ASSERT_TRUE(rejected.reject_reason.has_value());
    EXPECT_EQ(*rejected.reject_reason, "no market context for symbol");
    EXPECT_DOUBLE_EQ(rejected.filled_quantity, 0.0);
    EXPECT_FALSE(rejected.average_fill_price.has_value());
}

TEST(OrderRejectionShape, DefaultOrderHasNoRejectReason) {
    const domain::Order order{};
    EXPECT_FALSE(order.reject_reason.has_value());
}

TEST(OrderSubmissionShape, CarriesWhatAReservationIsSizedFrom) {
    // ADR 0004 sections 7-8: the submission event is where the Portfolio
    // Manager reserves. For a limit buy it needs the order's identity, run,
    // side, quantity and limit price -- all present on the existing struct plus
    // the new run_id. Sizing the reservation is the Portfolio Manager's job;
    // this only pins that the inputs exist.
    domain::Order submitted{};
    submitted.id = common::OrderId{3305};
    submitted.run_id = common::RunId{12};
    submitted.symbol = "AAPL";
    submitted.side = domain::OrderSide::Buy;
    submitted.type = domain::OrderType::Limit;
    submitted.quantity = 100.0;
    submitted.limit_price = 150.50;
    submitted.status = domain::OrderStatus::Working;

    EXPECT_EQ(submitted.run_id, common::RunId{12});
    EXPECT_EQ(submitted.status, domain::OrderStatus::Working);
    ASSERT_TRUE(submitted.limit_price.has_value());
    EXPECT_DOUBLE_EQ(submitted.quantity * *submitted.limit_price, 15'050.0);
    EXPECT_DOUBLE_EQ(submitted.filled_quantity, 0.0);
}

TEST(OrderSubmissionShape, SubmissionStatusSerializesAsWorking) {
    // The serialized order_status the Portfolio Manager reserves on (section
    // 1.2) is to_string(OrderStatus::Working).
    EXPECT_EQ(domain::to_string(domain::OrderStatus::Working), "working");
}
