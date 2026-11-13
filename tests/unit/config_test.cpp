#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "trading_engine/common/errors.hpp"
#include "trading_engine/configuration/engine_config.hpp"

namespace tec  = trading_engine::common;
namespace tcfg = trading_engine::config;

namespace {

// The ConfigError's message, or "" when parsing succeeds.
std::string error_of(const std::string& json) {
    try {
        (void)tcfg::parse_config(json);
    } catch (const tec::ConfigError& e) {
        return e.what();
    }
    return {};
}

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(Config, TheCheckedInExampleLoadsAndMatchesItsText) {
    const tcfg::EngineConfig c = tcfg::load_config(CONFIG_EXAMPLE_FILE);
    EXPECT_EQ(c.mode, tcfg::RunMode::Backtest);
    EXPECT_EQ(c.alpaca.symbols, (std::vector<std::string>{"AAPL", "MSFT", "SPY"}));
    EXPECT_EQ(c.alpaca.data_feed, "iex");
    EXPECT_EQ(c.postgres.port, 5432);
    EXPECT_EQ(c.risk.max_position_notional, tec::Money::from_units(25'000));
    EXPECT_EQ(c.risk.max_order_quantity, 1000);
    EXPECT_EQ(c.simulation.starting_cash, tec::Money::from_units(100'000));
    EXPECT_EQ(c.simulation.latency.order_to_fill, std::chrono::milliseconds{5});
    EXPECT_EQ(c.logging.sink, "stderr");
}

TEST(Config, AnEmptyObjectIsTheDefaults) {
    const tcfg::EngineConfig c = tcfg::parse_config("{}");
    const tcfg::EngineConfig d = tcfg::default_config();
    EXPECT_EQ(c.simulation.starting_cash, d.simulation.starting_cash);
    EXPECT_EQ(c.risk.max_daily_loss, d.risk.max_daily_loss);
    EXPECT_EQ(c.alpaca.stream_url, d.alpaca.stream_url);
}

TEST(Config, OnlyTheKeysGivenChange) {
    const tcfg::EngineConfig c = tcfg::parse_config(R"({"simulation": {"starting_cash": 2500.5}})");
    EXPECT_EQ(c.simulation.starting_cash, tec::Money::from_micros(2'500'500'000));
    EXPECT_EQ(c.simulation.slippage_bps, tcfg::default_config().simulation.slippage_bps);
    EXPECT_EQ(c.risk.max_gross_exposure, tcfg::default_config().risk.max_gross_exposure);
}

TEST(Config, MoneyIsReadExactlyFromTheText) {
    // 0.1 + 0.2 style values must not pick up binary rounding.
    const tcfg::EngineConfig c =
        tcfg::parse_config(R"({"simulation": {"fees": {"per_share": 0.005, "per_trade": 0.1}}})");
    EXPECT_EQ(c.simulation.fees.per_share, tec::Money::from_micros(5'000));
    EXPECT_EQ(c.simulation.fees.per_trade, tec::Money::from_micros(100'000));
}

TEST(Config, DollarKeysAreComments) {
    EXPECT_EQ(error_of(R"({"$comment": "hi", "risk": {"$note": 1}})"), "");
}

TEST(Config, AnUnknownKeyIsAnErrorThatNamesIt) {
    const std::string top = error_of(R"({"risks": {}})");
    EXPECT_TRUE(mentions(top, "risks: unknown setting")) << top;
    const std::string nested = error_of(R"({"simulation": {"fees": {"per_shares": 1}}})");
    EXPECT_TRUE(mentions(nested, "simulation.fees.per_shares: unknown setting")) << nested;
}

TEST(Config, WrongTypesNameTheField) {
    EXPECT_TRUE(mentions(error_of(R"({"risk": {"max_daily_loss": "5000"}})"), "risk.max_daily_loss: must be a number"));
    EXPECT_TRUE(mentions(error_of(R"({"alpaca": {"symbols": "AAPL"}})"), "alpaca.symbols: must be a list"));
    EXPECT_TRUE(mentions(error_of(R"({"simulation": {"deterministic": 1}})"), "must be true or false"));
    EXPECT_TRUE(mentions(error_of(R"({"risk": []})"), "risk: must be an object"));
}

TEST(Config, NumbersAreChecked) {
    EXPECT_TRUE(mentions(error_of(R"({"risk": {"max_daily_loss": -1}})"), "must not be negative"));
    EXPECT_TRUE(mentions(error_of(R"({"risk": {"max_order_quantity": 10.5}})"), "must be a whole number"));
    EXPECT_TRUE(mentions(error_of(R"({"risk": {"max_daily_loss": 1e3}})"), "no exponent"));
    EXPECT_TRUE(mentions(error_of(R"({"simulation": {"starting_cash": 0.0000001}})"), "6 decimal places"));
    EXPECT_TRUE(mentions(error_of(R"({"database": {"port": 70000}})"), "database.port: is too large"));
}

TEST(Config, ValidationRejectsValuesNoRunCanUse) {
    EXPECT_TRUE(mentions(error_of(R"({"simulation": {"starting_cash": 0}})"), "simulation.starting_cash"));
    EXPECT_TRUE(mentions(error_of(R"({"risk": {"max_position_notional": 200000}})"),
                         "cannot exceed risk.max_gross_exposure"));
    EXPECT_TRUE(mentions(error_of(R"({"alpaca": {"data_feed": "nyse"}})"), "alpaca.data_feed"));
    EXPECT_TRUE(mentions(error_of(R"({"alpaca": {"stream_url": "https://x"}})"), "must start with wss://"));
    EXPECT_TRUE(mentions(error_of(R"({"mode": "paper"})"), "mode: must be backtest or live"));
    EXPECT_TRUE(mentions(error_of(R"({"mode": "live", "alpaca": {"symbols": []}})"), "live mode"));
    EXPECT_TRUE(mentions(error_of(R"({"logging": {"level": "loud"}})"), "logging.level"));
    EXPECT_TRUE(mentions(error_of(R"({"database": {"pool_size": 0}})"), "database.pool_size"));
}

TEST(Config, MalformedJsonIsAConfigError) {
    EXPECT_TRUE(mentions(error_of(R"({"risk": )"), "not valid JSON"));
    EXPECT_TRUE(mentions(error_of("[]"), "must be an object"));
}

TEST(Config, LoadErrorsNameTheFile) {
    const auto path = std::filesystem::temp_directory_path() / "te_config_test_bad.json";
    {
        std::ofstream out{path};
        out << R"({"risk": {"max_open_orders": 0}})";
    }
    try {
        (void)tcfg::load_config(path);
        FAIL() << "expected ConfigError";
    } catch (const tec::ConfigError& e) {
        const std::string what = e.what();
        EXPECT_TRUE(mentions(what, path.string() + ": risk.max_open_orders")) << what;
    }
    std::filesystem::remove(path);

    EXPECT_THROW((void)tcfg::load_config(path), tec::ConfigError);   // now missing
}
