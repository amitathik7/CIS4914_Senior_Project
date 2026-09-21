// Pins the core promise of this scaffold: a stub that gets called must throw
// common::NotImplemented -- it must never quietly return fabricated market,
// portfolio, or performance data. One representative check per layer.

#include <gtest/gtest.h>

#include "trading_engine/analytics/performance_analyzer.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/portfolio/portfolio_manager.hpp"

namespace tec  = trading_engine::common;
namespace tcfg = trading_engine::config;

TEST(ScaffoldContract, DefaultConfigIsRealAndSane) {
    // Configuration is not "results" -- default_config() is a genuine function.
    const tcfg::EngineConfig c = tcfg::default_config();
    EXPECT_EQ(c.mode, tcfg::RunMode::Backtest);
    EXPECT_GT(c.simulation.starting_cash, 0.0);
    EXPECT_GT(c.risk.max_gross_exposure, 0.0);
    EXPECT_EQ(c.alpaca.api_key_env, "ALPACA_API_KEY");
    EXPECT_EQ(c.postgres.password_env, "PGPASSWORD");
}

TEST(ScaffoldContract, ConfigFileLoadingIsNotFakedButThrows) {
    EXPECT_THROW((void)tcfg::load_config("does-not-exist.json"), tec::NotImplemented);
    EXPECT_THROW(tcfg::validate(tcfg::default_config()), tec::NotImplemented);
}

TEST(ScaffoldContract, EventBusOperationsThrowNotImplemented) {
    trading_engine::events::InProcessEventBus bus;
    EXPECT_THROW(bus.start(), tec::NotImplemented);
    EXPECT_THROW((void)bus.depth(), tec::NotImplemented);
}

TEST(ScaffoldContract, PortfolioManagerNeverFabricatesState) {
    tec::ManualClock clock;
    trading_engine::portfolio::PortfolioManager pm{tec::RunId{1}, 100'000.0, clock};

    // Returning starting cash / an empty snapshot here would be a lie.
    EXPECT_THROW((void)pm.cash(), tec::NotImplemented);
    EXPECT_THROW((void)pm.snapshot(), tec::NotImplemented);
    EXPECT_THROW((void)pm.position("AAPL"), tec::NotImplemented);
    EXPECT_THROW((void)pm.buying_power(), tec::NotImplemented);
    // Granting a hold without real buying power would be fabricated state.
    EXPECT_THROW((void)pm.hold_for_signal(tec::SignalId{1}, "AAPL",
                                          trading_engine::domain::OrderSide::Buy,
                                          10.0, std::nullopt),
                 tec::NotImplemented);
}

TEST(ScaffoldContract, PerformanceAnalyzerNeverFabricatesAReport) {
    const trading_engine::analytics::PerformanceAnalyzer analyzer;
    EXPECT_THROW((void)analyzer.analyze({}, {}), tec::NotImplemented);
}

TEST(ScaffoldContract, NotImplementedCarriesTheCallSiteInItsMessage) {
    try {
        trading_engine::events::InProcessEventBus{}.start();
        FAIL() << "expected common::NotImplemented";
    } catch (const tec::NotImplemented& e) {
        const std::string_view what{e.what()};
        EXPECT_NE(what.find("InProcessEventBus::start"), std::string_view::npos);
        EXPECT_NE(what.find("not implemented"), std::string_view::npos);
    }
}
