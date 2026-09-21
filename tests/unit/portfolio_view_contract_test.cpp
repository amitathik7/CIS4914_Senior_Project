// Contract checks for the read seam proposed by ADR 0005 (Proposed, not
// Accepted): IPortfolioView is narrow enough for the RiskManager to depend on,
// and implementable by a test double without dragging in PortfolioManager.
//
// These are NOT tests of PortfolioManager -- its methods are all still
// common::NotImplemented (pinned by scaffold_contract_test). They test the
// *interface*: that a FakePortfolioView can satisfy it, that the snapshot
// semantics ADR 0005 section 7 describes actually hold for a consumer, and
// that open orders and buying power are visible as section 4 decides (team
// decision 2026-09-20: cash is reserved at order submission).
//
// The fake is the one IMPLEMENTATION_PLAN.md M1 calls for, so risk policies
// can be tested later without a real portfolio.

#include <gtest/gtest.h>

#include "trading_engine/portfolio/portfolio_manager.hpp"

namespace domain = trading_engine::domain;
namespace common = trading_engine::common;
namespace portfolio = trading_engine::portfolio;

namespace {

// Minimal stand-in for PortfolioManager. That this compiles at all is the
// point: the read seam is implementable without the write side.
class FakePortfolioView final : public portfolio::IPortfolioView {
public:
    explicit FakePortfolioView(common::Money cash) : cash_{cash} {
        snapshot_.cash = cash;
        snapshot_.total_equity = cash;
        snapshot_.buying_power = cash;   // nothing reserved yet
    }

    void add_position(portfolio::Position position) {
        snapshot_.positions.push_back(std::move(position));
    }

    void set_as_of(common::Timestamp as_of) { snapshot_.as_of = as_of; }

    // Mirrors what the real PortfolioManager does on a submission event:
    // record the open order and hold its cash, so buying power falls while
    // settled cash does not.
    void add_pending_order(portfolio::PendingOrder order) {
        snapshot_.reserved_cash += order.reserved_cash;
        snapshot_.buying_power = cash_ - snapshot_.reserved_cash;
        snapshot_.pending_orders.push_back(std::move(order));
    }

    [[nodiscard]] domain::PortfolioSnapshot snapshot() const override {
        return snapshot_;   // a copy, per ADR 0005 section 7
    }

    [[nodiscard]] std::optional<portfolio::Position> position(
        const common::Symbol& symbol) const override {
        for (const auto& p : snapshot_.positions) {
            if (p.symbol == symbol) { return p; }
        }
        return std::nullopt;
    }

    [[nodiscard]] common::Money cash() const override { return cash_; }

    [[nodiscard]] common::Money buying_power() const override {
        return snapshot_.buying_power;
    }

private:
    common::Money             cash_{0};
    domain::PortfolioSnapshot snapshot_{};
};

portfolio::Position make_position(common::Symbol symbol,
                                  common::Quantity quantity,
                                  common::Price average_cost) {
    portfolio::Position p{};
    p.symbol = std::move(symbol);
    p.quantity = quantity;
    p.average_cost = average_cost;
    return p;
}

portfolio::PendingOrder make_pending(common::OrderId id,
                                     trading_engine::domain::OrderSide side,
                                     common::Quantity remaining,
                                     common::Money reserved) {
    portfolio::PendingOrder o{};
    o.order_id = id;
    o.symbol = "AAPL";
    o.side = side;
    o.remaining_quantity = remaining;
    o.reserved_cash = reserved;
    return o;
}

// Stand-in for the atomic check-and-hold. The point of the real one is that
// the check and the hold happen under one lock; this fake keeps them in one
// function for the same reason, so a test cannot accidentally model the
// split-step version the decision rejects.
class FakeReservationLedger final : public portfolio::IReservationLedger {
public:
    explicit FakeReservationLedger(common::Money buying_power)
        : buying_power_{buying_power} {}

    [[nodiscard]] portfolio::ReservationResult hold_for_signal(
        common::SignalId,
        const common::Symbol&,
        domain::OrderSide side,
        common::Quantity quantity,
        std::optional<common::Price> limit_price) override {
        // A sell holds no cash (ADR 0005 section 4).
        const common::Money want =
            side == domain::OrderSide::Sell || !limit_price
                ? 0.0
                : quantity * *limit_price;
        if (want > buying_power_) {
            return {false, 0.0, buying_power_};
        }
        buying_power_ -= want;
        return {true, want, buying_power_};
    }

    void release_signal_hold(common::SignalId) override {}

private:
    common::Money buying_power_{0};
};

}  // namespace

TEST(PortfolioViewContract, RiskCanDependOnTheReadSeamAlone) {
    // The RiskManager takes a const IPortfolioView&. Binding a fake to that
    // reference type is what proves the dependency is on the interface, not on
    // PortfolioManager.
    FakePortfolioView fake{100'000.0};
    const portfolio::IPortfolioView& view = fake;

    EXPECT_DOUBLE_EQ(view.cash(), 100'000.0);
}

TEST(PortfolioViewContract, AbsentPositionIsDistinctFromAFlatOne) {
    // ADR 0005 section 8: nullopt means "never held", a present position with
    // quantity 0 means "held and closed". Risk policies must not conflate them.
    FakePortfolioView fake{50'000.0};
    fake.add_position(make_position("AAPL", 0.0, 150.0));

    const auto closed = fake.position("AAPL");
    ASSERT_TRUE(closed.has_value());
    EXPECT_TRUE(closed->is_flat());

    EXPECT_FALSE(fake.position("TSLA").has_value());
}

TEST(PortfolioViewContract, EmptyPortfolioIsValidNotAnError) {
    // Section 8: at run start there are no positions and equity == cash.
    FakePortfolioView fake{25'000.0};

    const auto snap = fake.snapshot();
    EXPECT_TRUE(snap.positions.empty());
    EXPECT_DOUBLE_EQ(snap.cash, 25'000.0);
    EXPECT_DOUBLE_EQ(snap.total_equity, snap.cash);
}

TEST(PortfolioViewContract, SnapshotIsACopyNotALiveReference) {
    // Section 7: a snapshot is taken at an instant and does not update itself.
    // A policy holding one while the portfolio moves is reading history -- by
    // design, and safe to hand across threads.
    FakePortfolioView fake{10'000.0};
    const auto before = fake.snapshot();

    fake.add_position(make_position("AAPL", 100.0, 150.0));
    const auto after = fake.snapshot();

    EXPECT_TRUE(before.positions.empty());
    EXPECT_EQ(after.positions.size(), 1u);
}

TEST(PortfolioViewContract, CostBasisTravelsWithQuantity) {
    // Section 3: the ticket asked whether cost basis should accompany
    // quantity. It already does, and a policy needs it to tell a position at a
    // loss from one at a gain.
    FakePortfolioView fake{10'000.0};
    fake.add_position(make_position("AAPL", 100.0, 150.0));

    const auto position = fake.position("AAPL");
    ASSERT_TRUE(position.has_value());
    EXPECT_DOUBLE_EQ(position->quantity, 100.0);
    EXPECT_DOUBLE_EQ(position->average_cost, 150.0);
}

TEST(PortfolioViewContract, ShortPositionIsCarriedBySignedQuantity) {
    FakePortfolioView fake{10'000.0};
    fake.add_position(make_position("AAPL", -40.0, 150.0));

    const auto position = fake.position("AAPL");
    ASSERT_TRUE(position.has_value());
    EXPECT_LT(position->quantity, 0.0);
    EXPECT_FALSE(position->is_flat());
}

TEST(PortfolioViewContract, NoOpenOrdersMeansBuyingPowerEqualsCash) {
    // ADR 0005 section 8: with nothing reserved, the two numbers agree.
    FakePortfolioView fake{10'000.0};
    const auto snap = fake.snapshot();

    EXPECT_TRUE(snap.pending_orders.empty());
    EXPECT_DOUBLE_EQ(snap.reserved_cash, 0.0);
    EXPECT_DOUBLE_EQ(fake.buying_power(), fake.cash());
}

TEST(PortfolioViewContract, ReservationLowersBuyingPowerButNotCash) {
    // Section 6: cash is settled money and does not move on submission; buying
    // power is what is free to commit. A policy checking a new buy against
    // cash() here would approve spending money that is already spoken for.
    FakePortfolioView fake{20'000.0};
    fake.add_pending_order(make_pending(common::OrderId{3305},
                                        domain::OrderSide::Buy, 100.0, 15'050.0));

    EXPECT_DOUBLE_EQ(fake.cash(), 20'000.0);
    EXPECT_DOUBLE_EQ(fake.buying_power(), 4'950.0);
    EXPECT_DOUBLE_EQ(fake.snapshot().reserved_cash, 15'050.0);
}

TEST(PortfolioViewContract, PendingOrdersCarryWhatIssue5AsksFor) {
    // Issue #5: "Pending/Unfulfilled orders (symbols + quantity + side)".
    FakePortfolioView fake{20'000.0};
    fake.add_pending_order(make_pending(common::OrderId{3305},
                                        domain::OrderSide::Buy, 100.0, 15'050.0));

    const auto snap = fake.snapshot();
    ASSERT_EQ(snap.pending_orders.size(), 1u);
    const auto& o = snap.pending_orders.front();
    EXPECT_EQ(o.symbol, "AAPL");
    EXPECT_EQ(o.side, domain::OrderSide::Buy);
    EXPECT_DOUBLE_EQ(o.remaining_quantity, 100.0);
}

TEST(PortfolioViewContract, PendingSellReservesNoCash) {
    // Section 4: a sell commits shares against the position, not cash, so it
    // leaves buying power alone.
    FakePortfolioView fake{20'000.0};
    fake.add_pending_order(make_pending(common::OrderId{3306},
                                        domain::OrderSide::Sell, 50.0, 0.0));

    EXPECT_DOUBLE_EQ(fake.buying_power(), fake.cash());
    EXPECT_EQ(fake.snapshot().pending_orders.size(), 1u);
}

TEST(PortfolioViewContract, OpenOrderCountComesFromTheSnapshot) {
    // Section 8: max_open_orders is checked against pending_orders.size(),
    // rather than a separate Risk-side count that could drift from it.
    FakePortfolioView fake{50'000.0};
    fake.add_pending_order(make_pending(common::OrderId{1}, domain::OrderSide::Buy, 10.0, 1'500.0));
    fake.add_pending_order(make_pending(common::OrderId{2}, domain::OrderSide::Sell, 5.0, 0.0));

    EXPECT_EQ(fake.snapshot().pending_orders.size(), 2u);
}

TEST(ReservationLedgerContract, SecondSignalCannotSpendTheFirstOnesCash) {
    // The whole point of the atomic check-and-hold (ADR 0005 section 4): two
    // signals evaluated back to back, before either has become an order, must
    // not both be approved against the same buying power. Under the
    // check-then-reserve-later design this test would fail, because the second
    // check would still see the full amount.
    FakeReservationLedger ledger{20'000.0};

    const auto first = ledger.hold_for_signal(common::SignalId{1}, "AAPL",
                                              domain::OrderSide::Buy, 100.0, 150.0);
    ASSERT_TRUE(first.granted);
    EXPECT_DOUBLE_EQ(first.held, 15'000.0);
    EXPECT_DOUBLE_EQ(first.buying_power_after, 5'000.0);

    // No order exists for signal 1 yet. The hold alone has to be enough.
    const auto second = ledger.hold_for_signal(common::SignalId{2}, "AAPL",
                                               domain::OrderSide::Buy, 100.0, 150.0);
    EXPECT_FALSE(second.granted);
    EXPECT_DOUBLE_EQ(second.held, 0.0);
    EXPECT_DOUBLE_EQ(second.buying_power_after, 5'000.0);
}

TEST(ReservationLedgerContract, AffordableSecondSignalStillGetsThrough) {
    FakeReservationLedger ledger{20'000.0};
    ASSERT_TRUE(ledger.hold_for_signal(common::SignalId{1}, "AAPL",
                                       domain::OrderSide::Buy, 100.0, 150.0).granted);

    const auto small = ledger.hold_for_signal(common::SignalId{2}, "AAPL",
                                              domain::OrderSide::Buy, 10.0, 150.0);
    EXPECT_TRUE(small.granted);
    EXPECT_DOUBLE_EQ(small.buying_power_after, 3'500.0);
}

TEST(ReservationLedgerContract, SellHoldsNoCash) {
    // Section 4: a sell commits shares against the position, not cash.
    FakeReservationLedger ledger{1'000.0};
    const auto sell = ledger.hold_for_signal(common::SignalId{3}, "AAPL",
                                             domain::OrderSide::Sell, 500.0, 150.0);
    EXPECT_TRUE(sell.granted);
    EXPECT_DOUBLE_EQ(sell.held, 0.0);
    EXPECT_DOUBLE_EQ(sell.buying_power_after, 1'000.0);
}
