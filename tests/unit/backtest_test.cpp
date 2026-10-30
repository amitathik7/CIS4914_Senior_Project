// The backtest path end to end: NaiveFillModel, ExecutionSimulator, the starter
// risk policies and RiskManager, PerformanceAnalyzer, and app::Backtest wiring
// them to the real pipeline. Every expected number is worked out by hand in the
// comment beside it.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "support/price_literals.hpp"
#include "support/recording_event_bus.hpp"
#include "trading_engine/analytics/performance_analyzer.hpp"
#include "trading_engine/app/backtest.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/execution/execution_simulator.hpp"
#include "trading_engine/execution/fill_model.hpp"
#include "trading_engine/portfolio/portfolio_manager.hpp"
#include "trading_engine/risk/risk_manager.hpp"
#include "trading_engine/risk/risk_policy.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"

namespace {

namespace tec  = trading_engine::common;
namespace dom  = trading_engine::domain;
namespace ex   = trading_engine::execution;
namespace rk   = trading_engine::risk;
namespace an   = trading_engine::analytics;
namespace app  = trading_engine::app;
namespace st   = trading_engine::strategy;
namespace cfg  = trading_engine::config;
namespace ev   = trading_engine::events;
using namespace trading_engine::test_support;
using namespace std::chrono;

const tec::Timestamp kT0{sys_days{2026y / January / 5} + 14h + 30min};

dom::MarketEvent bar(const std::string& symbol, int minute, tec::Price close,
                     std::optional<tec::Price> open = std::nullopt) {
    dom::MarketEvent e;
    e.symbol        = symbol;
    e.type          = dom::MarketEventType::Bar;
    e.exchange_time = kT0 + minutes{minute};
    e.price         = close;
    e.open          = open;
    return e;
}

dom::Order market_order(dom::OrderSide side, tec::Quantity qty) {
    dom::Order o;
    o.symbol   = "AAPL";
    o.side     = side;
    o.quantity = qty;
    return o;
}

// ---- NaiveFillModel -----------------------------------------------------------------------

TEST(NaiveFillModel, BuyPaysSlippageAndEveryFeeComponent) {
    cfg::SimulationConfig sim;
    sim.slippage_bps   = 10.0;                       // 10 bps of 100 = 0.10
    sim.fees.per_trade = 1_px;
    sim.fees.per_share = 0.01_px;
    sim.fees.bps       = 5.0;                         // 5 bps of 100.10 * 50 = 2.5025
    ex::MarketContext m{"AAPL", kT0, 100_px, {}, {}, {}};
    const auto fills = ex::NaiveFillModel{}.simulate(market_order(dom::OrderSide::Buy, 50), m, sim);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 50);
    EXPECT_EQ(fills[0].fill_price, 100.10_px);
    EXPECT_EQ(fills[0].slippage, 0.10_px);
    EXPECT_EQ(fills[0].fees, 1_px + 0.50_px + 2.5025_px);
    EXPECT_EQ(fills[0].status, dom::FillStatus::Filled);
}

TEST(NaiveFillModel, SellReceivesLessAndIsNegative) {
    cfg::SimulationConfig sim;
    sim.slippage_bps = 10.0;
    ex::MarketContext m{"AAPL", kT0, 100_px, {}, {}, {}};
    const auto fills = ex::NaiveFillModel{}.simulate(market_order(dom::OrderSide::Sell, 5), m, sim);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, -5);
    EXPECT_EQ(fills[0].fill_price, 99.90_px);
    EXPECT_EQ(fills[0].slippage, -0.10_px);
}

TEST(NaiveFillModel, LimitNotReachedDoesNotFillAndNoPriceIsAnError) {
    cfg::SimulationConfig sim;
    auto order        = market_order(dom::OrderSide::Buy, 1);
    order.type        = dom::OrderType::Limit;
    order.limit_price = 99_px;
    ex::MarketContext m{"AAPL", kT0, 100_px, {}, {}, {}};
    EXPECT_TRUE(ex::NaiveFillModel{}.simulate(order, m, sim).empty());
    m.last_price.reset();
    EXPECT_THROW((void)ex::NaiveFillModel{}.simulate(order, m, sim), tec::ValidationError);
}

// ---- ExecutionSimulator ---------------------------------------------------------------------

TEST(ExecutionSimulator, FillsOnTheNextBarsOpenNeverTheSameBar) {
    tec::ManualClock clock{kT0};
    RecordingEventBus bus;
    const ex::NaiveFillModel model;
    cfg::SimulationConfig sim;
    sim.slippage_bps = 0.0;
    ex::ExecutionSimulator exec{model, bus, clock, sim};

    dom::TradeSignal s;
    s.id                 = tec::SignalId{7};
    s.symbol             = "AAPL";
    s.side               = dom::SignalSide::Buy;
    s.requested_quantity = 3;
    const auto id        = exec.submit(s);
    EXPECT_EQ(exec.open_order_count(), 1u);

    exec.on_market_event(bar("AAPL", 0, 10_px, 9_px));   // the deciding bar: too early
    exec.on_market_event(bar("MSFT", 1, 10_px, 9_px));   // another symbol
    EXPECT_EQ(exec.open_order_count(), 1u);
    exec.on_market_event(bar("AAPL", 1, 12_px, 11_px));  // fills at the open, 11
    EXPECT_EQ(exec.open_order_count(), 0u);

    const auto& events = bus.published();
    ASSERT_EQ(events.size(), 3u);   // Working, Fill, Filled
    const auto& working = std::get<dom::Order>(events[0].payload);
    EXPECT_EQ(working.status, dom::OrderStatus::Working);
    EXPECT_EQ(working.origin_signal, tec::SignalId{7});
    const auto& fill = std::get<dom::Fill>(events[1].payload);
    EXPECT_EQ(fill.order_id, id);
    EXPECT_EQ(fill.fill_price, 11_px);
    EXPECT_EQ(fill.sequence, 1u);
    EXPECT_EQ(fill.filled_at, kT0 + minutes{1});
    const auto& done = std::get<dom::Order>(events[2].payload);
    EXPECT_EQ(done.status, dom::OrderStatus::Filled);
    EXPECT_EQ(done.average_fill_price, 11_px);
}

TEST(ExecutionSimulator, AFlatSignalIsRejectedNotOrdered) {
    tec::ManualClock clock{kT0};
    RecordingEventBus bus;
    const ex::NaiveFillModel model;
    ex::ExecutionSimulator exec{model, bus, clock, {}};
    dom::TradeSignal s;
    s.symbol = "AAPL";
    s.side   = dom::SignalSide::Flat;
    (void)exec.submit(s);
    ASSERT_EQ(bus.published().size(), 1u);
    const auto& o = std::get<dom::Order>(bus.published()[0].payload);
    EXPECT_EQ(o.status, dom::OrderStatus::Rejected);
    EXPECT_TRUE(o.reject_reason.has_value());
    EXPECT_EQ(exec.open_order_count(), 0u);
}

// ---- policies -------------------------------------------------------------------------

dom::TradeSignal buy(tec::Quantity q, const char* symbol = "AAPL") {
    dom::TradeSignal s;
    s.id                 = tec::SignalId{1};
    s.symbol             = symbol;
    s.side               = dom::SignalSide::Buy;
    s.requested_quantity = q;
    return s;
}

TEST(RiskPolicies, EachStarterPolicyPassesAtItsLimitAndFailsBeyondIt) {
    rk::RiskContext c;
    c.limits.max_order_quantity    = 10;
    c.limits.max_position_notional = 1000_px;
    c.limits.max_gross_exposure    = 1500_px;
    c.limits.max_open_orders       = 2;
    c.limits.max_daily_loss        = 50_px;
    c.reference_prices["AAPL"]     = 100_px;
    dom::PortfolioSnapshot p;

    const auto resized = rk::MaxOrderQuantityPolicy{}.evaluate(buy(12), p, c);
    EXPECT_TRUE(resized.approved);
    EXPECT_EQ(resized.adjusted_quantity, 10);
    EXPECT_FALSE(rk::MaxOrderQuantityPolicy{}.evaluate(buy(10), p, c).adjusted_quantity);

    EXPECT_TRUE(rk::MaxPositionNotionalPolicy{}.evaluate(buy(10), p, c).approved);    // 1000
    EXPECT_FALSE(rk::MaxPositionNotionalPolicy{}.evaluate(buy(11), p, c).approved);   // 1100
    EXPECT_FALSE(rk::MaxPositionNotionalPolicy{}.evaluate(buy(1, "MSFT"), p, c).approved);   // no price

    p.gross_exposure = 600_px;
    EXPECT_TRUE(rk::MaxGrossExposurePolicy{}.evaluate(buy(9), p, c).approved);    // 1500
    EXPECT_FALSE(rk::MaxGrossExposurePolicy{}.evaluate(buy(10), p, c).approved);  // 1600

    c.open_order_count = 1;
    EXPECT_TRUE(rk::MaxOpenOrdersPolicy{}.evaluate(buy(1), p, c).approved);
    c.open_order_count = 2;
    EXPECT_FALSE(rk::MaxOpenOrdersPolicy{}.evaluate(buy(1), p, c).approved);

    c.pnl_today = -49.99_px;
    EXPECT_TRUE(rk::DailyLossPolicy{}.evaluate(buy(1), p, c).approved);
    c.pnl_today = -50_px;
    EXPECT_FALSE(rk::DailyLossPolicy{}.evaluate(buy(1), p, c).approved);
    auto sell = buy(1);
    sell.side = dom::SignalSide::Sell;
    EXPECT_TRUE(rk::DailyLossPolicy{}.evaluate(sell, p, c).approved);
}

TEST(RiskManager, ResizesThenHoldsAndRefusesASellWithNoShares) {
    tec::ManualClock clock{kT0};
    RecordingEventBus bus;
    trading_engine::portfolio::PortfolioManager pm{tec::RunId{1}, 10000_px, clock};
    pm.mark(bar("AAPL", 0, 100_px));
    cfg::RiskLimits limits;
    limits.max_order_quantity = 5;
    rk::RiskManager risk{pm, bus, clock, limits, rk::starter_policies()};
    risk.set_reservation_ledger(&pm);
    risk.on_market_event(bar("AAPL", 0, 100_px));

    const auto d = risk.check(buy(8));
    EXPECT_TRUE(d.approved);
    EXPECT_EQ(d.adjusted_quantity, 5);
    // 5 * 100 * 1.05 buffer = 525 held against buying power.
    EXPECT_EQ(pm.buying_power(), 10000_px - 525_px);

    auto sell = buy(1);
    sell.id   = tec::SignalId{2};
    sell.side = dom::SignalSide::Sell;
    const auto refused = risk.check(sell);
    EXPECT_FALSE(refused.approved);
    EXPECT_NE(refused.reason.find("no short selling"), std::string::npos);

    const auto log = risk.decisions();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].policy, "max_order_quantity");
    EXPECT_EQ(log[1].policy, "buying_power");
    EXPECT_EQ(risk.stats().rejected, 1u);
    EXPECT_EQ(risk.stats().resized, 1u);
}

// ---- PerformanceAnalyzer ----------------------------------------------------------------

dom::PortfolioSnapshot equity(tec::Money e, int minute) {
    dom::PortfolioSnapshot s;
    s.run_id       = tec::RunId{1};
    s.as_of        = kT0 + minutes{minute};
    s.total_equity = e;
    return s;
}

dom::Fill fill(tec::Quantity q, tec::Price price, tec::Money fees) {
    dom::Fill f;
    f.symbol          = "AAPL";
    f.filled_quantity = q;
    f.fill_price      = price;
    f.fees            = fees;
    return f;
}

TEST(PerformanceAnalyzer, RoundTripsDrawdownAndReturns) {
    // Equity 100 -> 110 -> 99 -> 121: drawdown 99/110 - 1 = -0.1, return +21%.
    const std::vector<dom::PortfolioSnapshot> curve{equity(100_px, 0), equity(110_px, 1),
                                                    equity(99_px, 2), equity(121_px, 3)};
    // Buy 100 @ 10, sell 100 @ 12, fee 1 each way: 200 - 2 = +198. Buy 10 @ 20, sell 10 @ 19: -10 - 2 = -12.
    const std::vector<dom::Fill> fills{fill(100, 10_px, 1_px), fill(-100, 12_px, 1_px),
                                       fill(10, 20_px, 1_px), fill(-10, 19_px, 1_px)};
    const auto r = an::PerformanceAnalyzer{}.analyze(curve, fills);
    EXPECT_NEAR(r.total_return, 0.21, 1e-12);
    EXPECT_NEAR(r.max_drawdown, -0.1, 1e-12);
    EXPECT_EQ(r.fill_count, 4u);
    EXPECT_EQ(r.trade_count, 2u);
    EXPECT_EQ(r.winning_trades, 1u);
    EXPECT_EQ(r.losing_trades, 1u);
    EXPECT_EQ(r.gross_profit, 198_px);
    EXPECT_EQ(r.gross_loss, 12_px);
    EXPECT_EQ(r.fees_paid, 4_px);
    EXPECT_NEAR(r.profit_factor, 198.0 / 12.0, 1e-12);
    EXPECT_NEAR(*r.win_rate, 0.5, 1e-12);
    EXPECT_TRUE(r.sharpe.has_value());
    EXPECT_GT(r.volatility, 0.0);
    EXPECT_EQ(r.period_start, kT0);
    EXPECT_EQ(r.period_end, kT0 + minutes{3});
}

TEST(PerformanceAnalyzer, NoTradesLeavesTradeMetricsUnset) {
    const std::vector<dom::PortfolioSnapshot> curve{equity(100_px, 0)};
    const auto r = an::PerformanceAnalyzer{}.analyze(curve, {});
    EXPECT_EQ(r.trade_count, 0u);
    EXPECT_FALSE(r.win_rate.has_value());
    EXPECT_FALSE(r.sharpe.has_value());
    EXPECT_EQ(r.profit_factor, 0.0);
    EXPECT_EQ(r.total_return, 0.0);
}

// ---- app::Backtest ------------------------------------------------------------------------

// Buys `qty` on its first bar and sells it on its third.
class ScriptedStrategy final : public st::IStrategy {
public:
    explicit ScriptedStrategy(tec::Quantity qty) : qty_{qty} {}
    std::string_view id() const override { return "scripted"; }
    void on_market_event(const dom::MarketEvent& e, st::ISignalSink& out) override {
        ++seen_;
        if (seen_ != 1 && seen_ != 3) {
            return;
        }
        dom::TradeSignal s;
        s.symbol             = e.symbol;
        s.side               = seen_ == 1 ? dom::SignalSide::Buy : dom::SignalSide::Sell;
        s.requested_quantity = qty_;
        s.order_type         = dom::OrderType::Market;
        out.emit(s);
    }

private:
    tec::Quantity qty_;
    int           seen_{0};
};

std::vector<dom::MarketEvent> four_bars() {
    return {bar("AAPL", 0, 10_px, 10_px), bar("AAPL", 1, 11_px, 10.5_px),
            bar("AAPL", 2, 12_px, 11.5_px), bar("AAPL", 3, 13_px, 12.5_px)};
}

TEST(Backtest, AHandWorkedRoundTripReconcilesToTheCent) {
    app::BacktestConfig c;
    c.simulation.starting_cash  = 1000_px;
    c.simulation.slippage_bps   = 0.0;
    c.simulation.fees.per_trade = 1_px;
    app::Backtest bt{c};
    bt.add_strategy(std::make_shared<ScriptedStrategy>(10));
    const auto r = bt.run(four_bars());

    // Buy decided at bar 0 (close 10), filled at bar 1's open 10.5: cash 1000 - 105 - 1 = 894.
    // Sell decided at bar 2 (close 12), filled at bar 3's open 12.5: cash 894 + 125 - 1 = 1018.
    ASSERT_EQ(r.signals.size(), 2u);
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.fills[0].fill_price, 10.5_px);
    EXPECT_EQ(r.fills[0].filled_at, kT0 + minutes{1});
    EXPECT_EQ(r.fills[1].fill_price, 12.5_px);
    EXPECT_EQ(r.final_snapshot.cash, 1018_px);
    EXPECT_EQ(r.final_snapshot.total_equity, 1018_px);
    EXPECT_TRUE(r.final_snapshot.pending_orders.empty());
    EXPECT_EQ(r.final_snapshot.buying_power, 1018_px);

    // Equity per bar: 1000, 894 + 10*11 = 1004, 894 + 120 = 1014, 1018.
    ASSERT_EQ(r.equity_curve.size(), 4u);
    EXPECT_EQ(r.equity_curve[0].total_equity, 1000_px);
    EXPECT_EQ(r.equity_curve[1].total_equity, 1004_px);
    EXPECT_EQ(r.equity_curve[2].total_equity, 1014_px);
    EXPECT_EQ(r.equity_curve[3].total_equity, 1018_px);

    EXPECT_NEAR(r.report.total_return, 0.018, 1e-12);
    EXPECT_EQ(r.report.trade_count, 1u);
    EXPECT_EQ(r.report.gross_profit, 18_px);   // (12.5 - 10.5) * 10 - 2 fees
    ASSERT_EQ(r.final_snapshot.positions.size(), 1u);
    EXPECT_EQ(r.final_snapshot.positions[0].realized_pnl, r.report.gross_profit - r.report.gross_loss);
    EXPECT_EQ(r.report.fees_paid, 2_px);
    EXPECT_EQ(r.risk.approved, 2u);
    ASSERT_EQ(r.orders.size(), 2u);
    EXPECT_EQ(r.orders[0].status, dom::OrderStatus::Filled);
    EXPECT_EQ(r.portfolio.errors, 0u);
}

TEST(Backtest, ASellWithNothingHeldIsRejectedAndCashIsUntouched) {
    app::BacktestConfig c;
    c.simulation.starting_cash = 50_px;   // cannot afford 10 shares at ~10
    app::Backtest bt{c};
    bt.add_strategy(std::make_shared<ScriptedStrategy>(10));
    const auto r = bt.run(four_bars());
    ASSERT_EQ(r.decisions.size(), 2u);
    EXPECT_FALSE(r.decisions[0].decision.approved);   // buying power
    EXPECT_FALSE(r.decisions[1].decision.approved);   // nothing to sell
    EXPECT_TRUE(r.fills.empty());
    EXPECT_EQ(r.final_snapshot.cash, 50_px);
}

TEST(Backtest, IsDeterministicWithARealStrategy) {
    std::vector<dom::MarketEvent> bars;
    for (int i = 0; i < 200; ++i) {
        const double px = 100.0 + 6.0 * std::sin(i / 5.0) + (i % 11 == 0 ? 3.0 : 0.0);
        bars.push_back(bar("AAPL", i, tec::Price::from_double(std::round(px * 100) / 100)));
    }
    const auto once = [&] {
        app::Backtest bt;
        st::MeanReversionConfig mr;
        mr.symbols            = {"AAPL"};
        mr.requested_quantity = 10;
        bt.add_strategy(std::make_shared<st::MeanReversionStrategy>(mr));
        return bt.run(bars);
    };
    const auto a = once();
    const auto b = once();
    ASSERT_FALSE(a.fills.empty());
    ASSERT_EQ(a.fills.size(), b.fills.size());
    for (std::size_t i = 0; i < a.fills.size(); ++i) {
        EXPECT_EQ(a.fills[i].fill_price, b.fills[i].fill_price);
        EXPECT_EQ(a.fills[i].filled_at, b.fills[i].filled_at);
    }
    EXPECT_EQ(a.final_snapshot.total_equity, b.final_snapshot.total_equity);
    EXPECT_EQ(a.report.total_return, b.report.total_return);
    // Cash reconciles with the fills exactly.
    tec::Money cash = app::BacktestConfig{}.simulation.starting_cash;
    for (const auto& f : a.fills) {
        cash -= f.fill_price * f.filled_quantity + f.fees;
    }
    EXPECT_EQ(a.final_snapshot.cash, cash);
}

TEST(Backtest, RunsOnceAndNeedsBars) {
    app::Backtest bt;
    EXPECT_THROW((void)bt.run({}), tec::ValidationError);
}

}  // namespace
