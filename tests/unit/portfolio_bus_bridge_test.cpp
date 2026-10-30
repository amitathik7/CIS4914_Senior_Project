// PortfolioBusBridge: market data, orders and fills published on a bus reach
// the PortfolioManager; refusals and contract violations are counted, not thrown.

#include <gtest/gtest.h>

#include "support/price_literals.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/portfolio/portfolio_bus_bridge.hpp"
#include "trading_engine/portfolio/portfolio_manager.hpp"

namespace {

namespace ev  = trading_engine::events;
namespace dom = trading_engine::domain;
namespace tec = trading_engine::common;
namespace pf  = trading_engine::portfolio;
using namespace trading_engine::test_support::literals;

constexpr tec::RunId kRun{7};

ev::Event wrap(ev::EventType type, ev::EventPayload payload) {
    ev::Event e;
    e.type    = type;
    e.payload = std::move(payload);
    return e;
}

dom::Fill buy_fill(std::uint64_t id, tec::RunId run = kRun) {
    dom::Fill f;
    f.id              = tec::FillId{id};
    f.order_id        = tec::OrderId{id};
    f.run_id          = run;
    f.symbol          = "AAPL";
    f.filled_quantity = 10;
    f.fill_price      = 100_px;
    f.status          = dom::FillStatus::Filled;
    f.sequence        = 1;
    return f;
}

class PortfolioBridgeTest : public ::testing::Test {
protected:
    tec::ManualClock clock_{};
    pf::PortfolioManager portfolio_{kRun, tec::Money::from_units(10'000), clock_};
    ev::InProcessEventBus bus_{clock_};
    pf::PortfolioBusBridge bridge_{bus_, portfolio_};

    void run(const std::vector<ev::Event>& events) {
        bridge_.attach();
        bus_.start();
        for (const auto& e : events) {
            ASSERT_TRUE(bus_.publish(e));
        }
        bus_.request_shutdown();
        bus_.wait_until_drained();
    }
};

TEST_F(PortfolioBridgeTest, FillsMoveCashAndPositionsAndMarksRevalueThem) {
    dom::MarketEvent mark;
    mark.symbol = "AAPL";
    mark.type   = dom::MarketEventType::Trade;
    mark.price  = 110_px;
    run({wrap(ev::EventType::Fill, buy_fill(1)), wrap(ev::EventType::MarketData, mark)});

    EXPECT_EQ(portfolio_.cash(), tec::Money::from_units(9'000));
    ASSERT_TRUE(portfolio_.position("AAPL").has_value());
    EXPECT_EQ(portfolio_.position("AAPL")->quantity, 10);
    EXPECT_EQ(portfolio_.snapshot().total_equity, tec::Money::from_units(10'100));
    EXPECT_EQ(bridge_.stats().fills_applied, 1U);
    EXPECT_EQ(bridge_.stats().marks, 1U);
}

TEST_F(PortfolioBridgeTest, OrderUpdatesReserveCash) {
    dom::Order order;
    order.id            = tec::OrderId{1};
    order.run_id        = kRun;
    order.origin_signal = tec::SignalId{1};
    order.symbol        = "AAPL";
    order.side          = dom::OrderSide::Buy;
    order.type          = dom::OrderType::Limit;
    order.quantity      = 10;
    order.limit_price   = 50_px;
    order.status        = dom::OrderStatus::Working;
    run({wrap(ev::EventType::Order, order)});

    EXPECT_EQ(bridge_.stats().order_updates, 1U);
    EXPECT_EQ(portfolio_.buying_power(), tec::Money::from_units(9'500));
}

TEST_F(PortfolioBridgeTest, DuplicatesAndContractViolationsAreCountedNotThrown) {
    dom::Fill zero = buy_fill(3);
    zero.filled_quantity = 0;   // breaks the fill contract: apply() throws ValidationError
    run({wrap(ev::EventType::Fill, buy_fill(1)), wrap(ev::EventType::Fill, buy_fill(1)),
         wrap(ev::EventType::Fill, buy_fill(2, tec::RunId{99})), wrap(ev::EventType::Fill, zero)});

    const auto s = bridge_.stats();
    EXPECT_EQ(s.fills_applied, 1U);
    EXPECT_EQ(s.fills_ignored, 2U);   // duplicate id, other run
    EXPECT_EQ(s.errors, 1U);
    EXPECT_FALSE(bridge_.last_error().empty());
    EXPECT_EQ(bus_.stats().handler_errors, 0U);   // nothing escaped into the bus
}

TEST_F(PortfolioBridgeTest, DetachWithdrawsEverySubscription) {
    bridge_.attach();
    bridge_.attach();   // idempotent
    EXPECT_EQ(bus_.subscription_count(ev::EventType::Fill), 1U);
    bridge_.detach();
    EXPECT_EQ(bus_.subscription_count(ev::EventType::Fill), 0U);
    EXPECT_EQ(bus_.subscription_count(ev::EventType::Order), 0U);
    EXPECT_EQ(bus_.subscription_count(ev::EventType::MarketData), 0U);
}

}  // namespace
