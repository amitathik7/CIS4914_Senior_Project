// Contract checks for the read seam proposed by ADR 0005 (Proposed, not
// Accepted): IPortfolioView is narrow enough for the RiskManager to depend on,
// and implementable by a test double without dragging in PortfolioManager.
//
// These are NOT tests of PortfolioManager -- its methods are all still
// common::NotImplemented (pinned by scaffold_contract_test). They test the
// *interface*: that a FakePortfolioView can satisfy it, and that the snapshot
// semantics ADR 0005 section 7 describes actually hold for a consumer.
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
    }

    void add_position(portfolio::Position position) {
        snapshot_.positions.push_back(std::move(position));
    }

    void set_as_of(common::Timestamp as_of) { snapshot_.as_of = as_of; }

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

TEST(PortfolioViewContract, SnapshotCarriesNoOpenOrderState) {
    // ADR 0005 section 4 / ADR 0004 section 8 propose (do not decide) that
    // pending orders stay off the snapshot. This test documents today's
    // absence -- if the team picks Option B and adds an open-orders field,
    // this test should fail, which is the prompt to revisit BOTH ADRs rather
    // than just deleting it.
    FakePortfolioView fake{10'000.0};
    const auto snap = fake.snapshot();

    // The snapshot's entire surface: settled state and derived exposure.
    EXPECT_DOUBLE_EQ(snap.cash, 10'000.0);
    EXPECT_DOUBLE_EQ(snap.gross_exposure, 0.0);
    EXPECT_DOUBLE_EQ(snap.net_exposure, 0.0);
    EXPECT_TRUE(snap.positions.empty());
}
