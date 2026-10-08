// PortfolioManager fill accounting, marking and reads (M5 acceptance criteria).

#include <atomic>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/portfolio/portfolio_manager.hpp"

namespace common = trading_engine::common;
namespace domain = trading_engine::domain;
namespace portfolio = trading_engine::portfolio;

namespace {

common::Money usd(double v) { return common::Money::from_double(v); }

constexpr common::RunId kRun{1};

class PortfolioManagerTest : public ::testing::Test {
protected:
    // A complete single fill against a fresh order.
    domain::Fill fill(common::Quantity qty, double price, double fees = 0.0) {
        domain::Fill f{};
        f.id = common::FillId{++next_fill_};
        f.order_id = common::OrderId{++next_order_};
        f.run_id = kRun;
        f.symbol = "AAPL";
        f.filled_quantity = qty;
        f.fill_price = usd(price);
        f.fees = usd(fees);
        f.status = domain::FillStatus::Filled;
        f.sequence = 1;
        return f;
    }

    domain::Order order(std::uint64_t id, std::uint64_t signal, domain::OrderSide side,
                        common::Quantity qty, domain::OrderStatus status,
                        std::optional<double> limit = std::nullopt) {
        domain::Order o{};
        o.id = common::OrderId{id};
        o.run_id = kRun;
        o.origin_signal = common::SignalId{signal};
        o.symbol = "AAPL";
        o.side = side;
        o.type = limit ? domain::OrderType::Limit : domain::OrderType::Market;
        o.quantity = qty;
        if (limit) {
            o.limit_price = usd(*limit);
        }
        o.status = status;
        return o;
    }

    domain::Fill fill_for(std::uint64_t order_id, std::uint32_t sequence,
                          common::Quantity qty, double price, domain::FillStatus status) {
        domain::Fill f = fill(qty, price);
        f.order_id = common::OrderId{order_id};
        f.sequence = sequence;
        f.status = status;
        return f;
    }

    portfolio::ReservationResult hold_buy(std::uint64_t signal, common::Quantity qty,
                                          std::optional<double> limit = std::nullopt) {
        return pm_.hold_for_signal(common::SignalId{signal}, "AAPL", domain::OrderSide::Buy,
                                   qty, limit ? std::optional{usd(*limit)} : std::nullopt);
    }

    portfolio::ReservationResult hold_sell(std::uint64_t signal, common::Quantity qty) {
        return pm_.hold_for_signal(common::SignalId{signal}, "AAPL", domain::OrderSide::Sell,
                                   qty, std::nullopt);
    }

    domain::MarketEvent trade(double price) {
        domain::MarketEvent e{};
        e.symbol = "AAPL";
        e.type = domain::MarketEventType::Trade;
        e.price = usd(price);
        return e;
    }

    common::ManualClock clock_;
    portfolio::PortfolioManager pm_{kRun, usd(100'000), clock_};
    std::uint64_t next_fill_{0};
    std::uint64_t next_order_{1'000};
};

}  // namespace

TEST_F(PortfolioManagerTest, StartsWithOnlyCash) {
    const auto snap = pm_.snapshot();
    EXPECT_EQ(snap.run_id, kRun);
    EXPECT_EQ(snap.cash, usd(100'000));
    EXPECT_EQ(snap.total_equity, usd(100'000));
    EXPECT_EQ(snap.buying_power, usd(100'000));
    EXPECT_TRUE(snap.positions.empty());
    EXPECT_FALSE(pm_.position("AAPL").has_value());
}

TEST_F(PortfolioManagerTest, LongRoundTripReconcilesToTheCent) {
    const double f = 1.37;
    ASSERT_TRUE(pm_.apply(fill(100, 10.00, f)));
    ASSERT_TRUE(pm_.apply(fill(-100, 12.00, f)));

    const auto pos = pm_.position("AAPL");
    ASSERT_TRUE(pos.has_value());
    EXPECT_TRUE(pos->is_flat());
    EXPECT_EQ(pos->realized_pnl, usd(200) - usd(f) * 2);
    EXPECT_EQ(pm_.cash(), usd(100'000) + usd(200) - usd(f) * 2);
}

TEST_F(PortfolioManagerTest, ShortRoundTripReconcilesToTheCent) {
    const double f = 0.99;
    ASSERT_TRUE(pm_.apply(fill(-100, 12.00, f)));
    EXPECT_EQ(pm_.position("AAPL")->quantity, -100);
    ASSERT_TRUE(pm_.apply(fill(100, 10.00, f)));

    const auto pos = pm_.position("AAPL");
    EXPECT_TRUE(pos->is_flat());
    EXPECT_EQ(pos->realized_pnl, usd(200) - usd(f) * 2);
    EXPECT_EQ(pm_.cash(), usd(100'000) + usd(200) - usd(f) * 2);
}

TEST_F(PortfolioManagerTest, PartialFillRoundTripReconciles) {
    domain::Fill first = fill(60, 10.00, 0.50);
    first.status = domain::FillStatus::PartiallyFilled;
    domain::Fill second = fill(40, 10.00, 0.50);
    second.order_id = first.order_id;
    second.sequence = 2;

    ASSERT_TRUE(pm_.apply(first));
    ASSERT_TRUE(pm_.apply(second));
    ASSERT_TRUE(pm_.apply(fill(-100, 12.00, 1.00)));

    EXPECT_EQ(pm_.position("AAPL")->realized_pnl, usd(198));
    EXPECT_EQ(pm_.cash(), usd(100'198));
}

TEST_F(PortfolioManagerTest, TwoBuysAverageTheCost) {
    pm_.apply(fill(100, 10.00));
    pm_.apply(fill(50, 13.00));

    const auto pos = pm_.position("AAPL");
    EXPECT_EQ(pos->quantity, 150);
    EXPECT_EQ(pos->average_cost, usd(11.00));
}

TEST_F(PortfolioManagerTest, AverageCostRoundsToTheNearestMillionth) {
    pm_.apply(fill(1, 10.00));
    pm_.apply(fill(1, 10.00));
    pm_.apply(fill(1, 11.00));
    EXPECT_EQ(pm_.position("AAPL")->average_cost, usd(10.333333));
}

TEST_F(PortfolioManagerTest, PartialSellRealisesOnlyTheClosedShares) {
    pm_.apply(fill(100, 10.00));
    pm_.apply(fill(-40, 15.00));

    const auto pos = pm_.position("AAPL");
    EXPECT_EQ(pos->quantity, 60);
    EXPECT_EQ(pos->average_cost, usd(10.00));
    EXPECT_EQ(pos->realized_pnl, usd(200));
}

TEST_F(PortfolioManagerTest, FlipFromLongToShortReopensAtTheFillPrice) {
    pm_.apply(fill(100, 10.00));
    pm_.apply(fill(-150, 12.00));

    const auto pos = pm_.position("AAPL");
    EXPECT_EQ(pos->quantity, -50);
    EXPECT_EQ(pos->average_cost, usd(12.00));
    EXPECT_EQ(pos->realized_pnl, usd(200));
}

TEST_F(PortfolioManagerTest, MarkMovesUnrealisedAndEquityButNotRealised) {
    pm_.apply(fill(100, 10.00, 1.00));
    const auto realised = pm_.position("AAPL")->realized_pnl;

    pm_.mark(trade(10.50));

    const auto pos = pm_.position("AAPL");
    EXPECT_EQ(pos->unrealized_pnl, usd(50));
    EXPECT_EQ(pos->realized_pnl, realised);
    EXPECT_EQ(pm_.snapshot().total_equity, usd(100'049));
}

TEST_F(PortfolioManagerTest, FillMarksTheSymbolAtTheFillPrice) {
    pm_.apply(fill(100, 10.00));
    EXPECT_EQ(pm_.position("AAPL")->unrealized_pnl, common::Money{});
    EXPECT_EQ(pm_.snapshot().total_equity, usd(100'000));
}

TEST_F(PortfolioManagerTest, MarkFallsBackToTheQuoteMidpoint) {
    pm_.apply(fill(100, 10.00));
    domain::MarketEvent quote{};
    quote.symbol = "AAPL";
    quote.bid = usd(10.99);
    quote.ask = usd(11.01);
    pm_.mark(quote);
    EXPECT_EQ(pm_.position("AAPL")->unrealized_pnl, usd(100));
}

TEST_F(PortfolioManagerTest, MarkWithoutAPriceIsIgnored) {
    pm_.apply(fill(100, 10.00));
    domain::MarketEvent status{};
    status.symbol = "AAPL";
    status.type = domain::MarketEventType::Status;
    pm_.mark(status);
    EXPECT_EQ(pm_.position("AAPL")->unrealized_pnl, common::Money{});
}

TEST_F(PortfolioManagerTest, EquityIsCashPlusMarkedPositions) {
    pm_.apply(fill(100, 10.00, 1.00));
    domain::Fill msft = fill(-20, 50.00, 1.00);
    msft.symbol = "MSFT";
    pm_.apply(msft);
    pm_.mark(trade(11.00));

    const auto snap = pm_.snapshot();
    EXPECT_EQ(snap.net_exposure, usd(1'100) - usd(1'000));
    EXPECT_EQ(snap.gross_exposure, usd(1'100) + usd(1'000));
    EXPECT_EQ(snap.total_equity, snap.cash + snap.net_exposure);
    ASSERT_EQ(snap.positions.size(), 2u);
    EXPECT_EQ(snap.positions[0].symbol, "AAPL");
}

TEST_F(PortfolioManagerTest, DuplicateFillIsIgnored) {
    const auto f = fill(100, 10.00, 1.00);
    ASSERT_TRUE(pm_.apply(f));
    const auto cash = pm_.cash();

    EXPECT_FALSE(pm_.apply(f));
    EXPECT_EQ(pm_.cash(), cash);
    EXPECT_EQ(pm_.position("AAPL")->quantity, 100);
}

TEST_F(PortfolioManagerTest, FillFromAnotherRunIsIgnored) {
    auto f = fill(100, 10.00);
    f.run_id = common::RunId{2};
    EXPECT_FALSE(pm_.apply(f));
    EXPECT_FALSE(pm_.position("AAPL").has_value());
}

TEST_F(PortfolioManagerTest, ContractViolationsThrowAndChangeNothing) {
    EXPECT_THROW(pm_.apply(fill(0, 10.00)), common::ValidationError);
    EXPECT_THROW(pm_.apply(fill(100, 0.00)), common::ValidationError);
    EXPECT_THROW(pm_.apply(fill(100, 10.00, -1.00)), common::ValidationError);

    auto gap = fill(100, 10.00);
    gap.sequence = 2;
    EXPECT_THROW(pm_.apply(gap), common::ValidationError);

    EXPECT_EQ(pm_.cash(), usd(100'000));
    EXPECT_FALSE(pm_.position("AAPL").has_value());
}

TEST_F(PortfolioManagerTest, SnapshotIsNeverTornByConcurrentFills) {
    // Every fill is 1 share at 10.00 with no fees, so in any consistent state
    // cash + quantity * 10 equals starting cash.
    constexpr int kFills = 20'000;
    std::atomic<bool> done{false};

    std::thread writer{[&] {
        for (int i = 0; i < kFills; ++i) {
            pm_.apply(fill(i % 2 == 0 ? 1 : -1, 10.00));
        }
        done = true;
    }};

    int torn = 0;
    while (!done) {
        const auto snap = pm_.snapshot();
        const common::Quantity qty = snap.positions.empty() ? 0 : snap.positions[0].quantity;
        if (snap.cash + usd(10.00) * qty != usd(100'000)) {
            ++torn;
        }
    }
    writer.join();
    EXPECT_EQ(torn, 0);
}

// --- Reservations ----------------------------------------------------------

using domain::FillStatus;
using domain::OrderSide;
using domain::OrderStatus;

TEST_F(PortfolioManagerTest, LimitBuyHoldsPricePlusEstimatedFees) {
    trading_engine::config::FeeModelConfig fees{};
    fees.per_trade = usd(1.00);
    fees.bps = 10.0;
    portfolio::PortfolioManager pm{kRun, usd(100'000), clock_, fees};

    const auto held = pm.hold_for_signal(common::SignalId{1}, "AAPL", OrderSide::Buy, 100,
                                         usd(150.00));
    ASSERT_TRUE(held.granted);
    EXPECT_EQ(held.held, usd(15'016.00));  // 15,000 + 1.00 + 10 bps
    EXPECT_EQ(pm.cash(), usd(100'000));
    EXPECT_EQ(pm.buying_power(), usd(100'000) - usd(15'016.00));
}

TEST_F(PortfolioManagerTest, MarketBuyHoldsLastMarkPlusBuffer) {
    pm_.mark(trade(10.00));
    const auto held = hold_buy(1, 100);
    ASSERT_TRUE(held.granted);
    EXPECT_EQ(held.held, usd(1'050.00));
}

TEST_F(PortfolioManagerTest, MarketBuyWithNoMarkIsRefused) {
    EXPECT_FALSE(hold_buy(1, 100).granted);
    EXPECT_EQ(pm_.buying_power(), usd(100'000));
}

TEST_F(PortfolioManagerTest, SecondSignalCannotSpendTheFirstOnesCash) {
    ASSERT_TRUE(hold_buy(1, 600, 100.00).granted);
    const auto second = hold_buy(2, 600, 100.00);
    EXPECT_FALSE(second.granted);
    EXPECT_EQ(second.buying_power_after, usd(40'000));
}

TEST_F(PortfolioManagerTest, NonPositiveQuantityIsRefused) {
    EXPECT_FALSE(hold_buy(1, 0, 10.00).granted);
    EXPECT_FALSE(hold_buy(2, -5, 10.00).granted);
}

TEST_F(PortfolioManagerTest, RetriedSignalDoesNotHoldTwice) {
    ASSERT_TRUE(hold_buy(1, 100, 10.00).granted);
    const auto retry = hold_buy(1, 100, 10.00);
    EXPECT_TRUE(retry.granted);
    EXPECT_EQ(pm_.buying_power(), usd(99'000));
}

TEST_F(PortfolioManagerTest, SellsAreLimitedToSharesNotAlreadyPromised) {
    pm_.apply(fill(100, 10.00));
    EXPECT_TRUE(hold_sell(1, 60).granted);
    EXPECT_FALSE(hold_sell(2, 50).granted);
    EXPECT_TRUE(hold_sell(3, 40).granted);
    EXPECT_EQ(pm_.buying_power(), pm_.cash());
}

TEST_F(PortfolioManagerTest, SellWithoutAPositionIsRefused) {
    EXPECT_FALSE(hold_sell(1, 10).granted);
}

TEST_F(PortfolioManagerTest, WorkingOrderTakesOverItsSignalsHold) {
    ASSERT_TRUE(hold_buy(7, 100, 10.00).granted);
    ASSERT_TRUE(pm_.apply_order_update(order(1, 7, OrderSide::Buy, 100, OrderStatus::Working, 10.00)));

    const auto snap = pm_.snapshot();
    ASSERT_EQ(snap.pending_orders.size(), 1u);
    EXPECT_EQ(snap.pending_orders[0].reserved_cash, usd(1'000));
    EXPECT_EQ(snap.pending_orders[0].remaining_quantity, 100);
    EXPECT_EQ(snap.reserved_cash, usd(1'000));  // moved, not doubled
}

TEST_F(PortfolioManagerTest, WorkingOrderWithoutAHoldReservesAnEstimate) {
    ASSERT_TRUE(pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::Working, 10.00)));
    EXPECT_EQ(pm_.buying_power(), usd(99'000));
}

TEST_F(PortfolioManagerTest, FillsReleaseTheReservationProRata) {
    ASSERT_TRUE(hold_buy(7, 100, 10.00).granted);
    pm_.apply_order_update(order(1, 7, OrderSide::Buy, 100, OrderStatus::Working, 10.00));

    pm_.apply(fill_for(1, 1, 40, 9.50, FillStatus::PartiallyFilled));
    EXPECT_EQ(pm_.snapshot().reserved_cash, usd(600));
    EXPECT_EQ(pm_.snapshot().pending_orders[0].remaining_quantity, 60);

    // Filled at a better price than reserved: the residue comes back too.
    pm_.apply(fill_for(1, 2, 60, 9.50, FillStatus::Filled));
    const auto snap = pm_.snapshot();
    EXPECT_TRUE(snap.pending_orders.empty());
    EXPECT_EQ(snap.reserved_cash, common::Money{});
    EXPECT_EQ(snap.buying_power, snap.cash);
    EXPECT_EQ(snap.cash, usd(100'000) - usd(950));
}

TEST_F(PortfolioManagerTest, RejectionBeforeWorkingReleasesTheSignalsHold) {
    ASSERT_TRUE(hold_buy(7, 100, 10.00).granted);
    EXPECT_TRUE(pm_.apply_order_update(order(1, 7, OrderSide::Buy, 100, OrderStatus::Rejected, 10.00)));
    EXPECT_EQ(pm_.buying_power(), usd(100'000));
}

TEST_F(PortfolioManagerTest, RejectionOfAnUnreservedOrderIsANoOp) {
    EXPECT_FALSE(pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::Rejected)));
}

TEST_F(PortfolioManagerTest, CancelAfterPartialFillReleasesTheRemainder) {
    pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::Working, 10.00));
    pm_.apply(fill_for(1, 1, 30, 10.00, FillStatus::PartiallyFilled));
    ASSERT_EQ(pm_.snapshot().reserved_cash, usd(700));

    EXPECT_TRUE(pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::Cancelled, 10.00)));
    EXPECT_EQ(pm_.snapshot().reserved_cash, common::Money{});
    EXPECT_TRUE(pm_.snapshot().pending_orders.empty());
}

TEST_F(PortfolioManagerTest, DuplicateAndLateWorkingEventsAreIgnored) {
    const auto working = order(1, 0, OrderSide::Buy, 100, OrderStatus::Working, 10.00);
    ASSERT_TRUE(pm_.apply_order_update(working));
    EXPECT_FALSE(pm_.apply_order_update(working));

    pm_.apply(fill_for(1, 1, 100, 10.00, FillStatus::Filled));
    EXPECT_FALSE(pm_.apply_order_update(working));
    EXPECT_EQ(pm_.snapshot().reserved_cash, common::Money{});
}

TEST_F(PortfolioManagerTest, LateWorkingAfterAnUntrackedFillDoesNotReserve) {
    // The fill completes the order before its Working event arrives.
    pm_.apply(fill_for(1, 1, 100, 10.00, FillStatus::Filled));
    EXPECT_FALSE(pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::Working, 10.00)));
    EXPECT_EQ(pm_.snapshot().reserved_cash, common::Money{});
}

TEST_F(PortfolioManagerTest, NonPositiveMarkIsIgnored) {
    pm_.mark(trade(10.00));
    pm_.mark(trade(0.00));
    EXPECT_EQ(hold_buy(1, 100).held, usd(1'050.00));
}

TEST_F(PortfolioManagerTest, FillForAClosedOrderThrows) {
    pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::Working, 10.00));
    pm_.apply(fill_for(1, 1, 100, 10.00, FillStatus::Filled));
    EXPECT_THROW(pm_.apply(fill_for(1, 2, 10, 10.00, FillStatus::Filled)),
                 common::ValidationError);
}

TEST_F(PortfolioManagerTest, FillDrivenOrderStatusesAreIgnored) {
    pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::Working, 10.00));
    EXPECT_FALSE(pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::PartiallyFilled, 10.00)));
    EXPECT_FALSE(pm_.apply_order_update(order(1, 0, OrderSide::Buy, 100, OrderStatus::Filled, 10.00)));
    EXPECT_EQ(pm_.snapshot().reserved_cash, usd(1'000));
}

TEST_F(PortfolioManagerTest, OrderFromAnotherRunIsIgnored) {
    auto o = order(1, 0, OrderSide::Buy, 100, OrderStatus::Working, 10.00);
    o.run_id = common::RunId{2};
    EXPECT_FALSE(pm_.apply_order_update(o));
}

TEST_F(PortfolioManagerTest, ReleaseSignalHoldFreesAnOrphanedHold) {
    ASSERT_TRUE(hold_buy(7, 100, 10.00).granted);
    pm_.release_signal_hold(common::SignalId{7});
    EXPECT_EQ(pm_.buying_power(), usd(100'000));
}

TEST_F(PortfolioManagerTest, ConcurrentHoldsNeverOverspend) {
    // 100,000 of cash covers exactly 100 holds of 1,000.
    constexpr int kAttempts = 200;
    std::atomic<int> granted{0};
    auto worker = [&](std::uint64_t first_signal) {
        for (int i = 0; i < kAttempts; ++i) {
            if (hold_buy(first_signal + static_cast<std::uint64_t>(i), 1, 1'000.00).granted) {
                ++granted;
            }
        }
    };
    std::thread a{worker, 1};
    std::thread b{worker, 10'001};
    a.join();
    b.join();
    EXPECT_EQ(granted, 100);
    EXPECT_EQ(pm_.buying_power(), common::Money{});
}
