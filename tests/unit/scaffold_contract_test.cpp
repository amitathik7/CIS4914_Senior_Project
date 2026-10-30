// Pins the core promise of this scaffold: a stub that gets called must throw
// common::NotImplemented -- it must never quietly return fabricated market,
// portfolio, or performance data. One representative check per layer.

#include <gtest/gtest.h>

#include "trading_engine/analytics/performance_analyzer.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/configuration/engine_config.hpp"

namespace tec  = trading_engine::common;
namespace tcfg = trading_engine::config;

TEST(ScaffoldContract, DefaultConfigIsRealAndSane) {
    // Configuration is not "results" -- default_config() is a genuine function.
    const tcfg::EngineConfig c = tcfg::default_config();
    EXPECT_EQ(c.mode, tcfg::RunMode::Backtest);
    EXPECT_GT(c.simulation.starting_cash, tec::Money{});
    EXPECT_GT(c.risk.max_gross_exposure, tec::Money{});
    EXPECT_EQ(c.alpaca.api_key_env, "ALPACA_API_KEY");
    EXPECT_EQ(c.postgres.password_env, "PGPASSWORD");
}

TEST(ScaffoldContract, ConfigFileLoadingIsNotFakedButThrows) {
    EXPECT_THROW((void)tcfg::load_config("does-not-exist.json"), tec::NotImplemented);
    EXPECT_THROW(tcfg::validate(tcfg::default_config()), tec::NotImplemented);
}

TEST(ScaffoldContract, PerformanceAnalyzerNeverFabricatesAReport) {
    // Implemented now, but with nothing to measure it refuses instead of
    // returning a zero-filled report.
    const trading_engine::analytics::PerformanceAnalyzer analyzer;
    EXPECT_THROW((void)analyzer.analyze({}, {}), tec::ValidationError);
}

TEST(ScaffoldContract, NotImplementedCarriesTheCallSiteInItsMessage) {
    try {
        (void)tcfg::load_config("does-not-exist.json");
        FAIL() << "expected common::NotImplemented";
    } catch (const tec::NotImplemented& e) {
        const std::string_view what{e.what()};
        EXPECT_NE(what.find("load_config"), std::string_view::npos);
        EXPECT_NE(what.find("not implemented"), std::string_view::npos);
    }
}
