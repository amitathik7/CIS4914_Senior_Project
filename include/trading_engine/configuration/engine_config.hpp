#pragma once

// -----------------------------------------------------------------------------
//  Engine configuration.
//
//  Responsibility: a single, plain-struct description of how to run the engine.
//  Parsed once at startup and then treated as immutable. The core must not
//  depend on a particular file format or JSON library -- parsing lives behind
//  load_config(), not in these headers. The file is JSON
//  (config/config.example.json); config/README.md describes it.
//
//  Secrets: this struct holds the NAMES of environment variables that carry
//  credentials (e.g. "ALPACA_API_KEY"), never the credential values. Resolving
//  them is the composition root's job.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/types.hpp"

namespace trading_engine::config {

enum class RunMode : std::uint8_t { Backtest = 0, Live };

// --- Market data / Alpaca -----------------------------------------------
struct AlpacaConfig {
    std::string environment{"paper"};        // "paper" | "live" (trading side)
    std::string data_feed{"iex"};            // "iex" | "sip"
    std::string market_data_base_url{"https://data.alpaca.markets"};
    std::string stream_url{"wss://stream.data.alpaca.markets/v2/iex"};

    // Names of env vars holding the credentials -- NOT the credentials.
    std::string api_key_env{"ALPACA_API_KEY"};
    std::string api_secret_env{"ALPACA_API_SECRET"};

    std::vector<common::Symbol> symbols{};   // instruments to subscribe / query

    // TODO: historical lookback window, bar interval, reconnect/backoff policy,
    //       rate-limit budget, sandbox toggle.
};

// --- Persistence / PostgreSQL -----------------------------------------
struct PostgresConfig {
    std::string url_env{"DATABASE_URL"};     // full libpq URL, if provided
    std::string host{"localhost"};
    std::uint16_t port{5432};
    std::string database{"trading_engine"};
    std::string user{"trading_engine"};
    std::string password_env{"PGPASSWORD"};  // name of env var, NOT the password
    std::string schema{"public"};
    std::uint32_t pool_size{4};

    // TODO: SSL mode, connect/statement timeouts, migration table name,
    //       read-replica endpoint.
};

// --- Risk limits -------------------------------------------------------
// Initial numbers are placeholders for the team to argue about
// (see docs/OPEN_QUESTIONS.md -- "risk limits").
struct RiskLimits {
    common::Money    max_gross_exposure{common::Money::from_units(100'000)};
    common::Money    max_position_notional{common::Money::from_units(25'000)};
    common::Quantity max_order_quantity{1'000};
    common::Money    max_daily_loss{common::Money::from_units(5'000)};
    std::uint32_t    max_open_orders{50};

    // TODO: per-symbol and per-strategy overrides, concentration limits,
    //       leverage cap, kill-switch thresholds.
};

// --- Simulation / execution assumptions ------------------------------
struct FeeModelConfig {
    common::Money per_share{};
    common::Money per_trade{};
    double        bps{0.0};             // fraction of notional, in basis points
};

struct LatencyConfig {
    common::Duration signal_to_order{std::chrono::milliseconds{1}};
    common::Duration order_to_fill{std::chrono::milliseconds{5}};
};

struct SimulationConfig {
    common::Money  starting_cash{common::Money::from_units(100'000)};
    double         replay_speed{0.0};   // 0 == as fast as possible; 1 == realtime
    bool           deterministic{true}; // drive time from a ManualClock
    FeeModelConfig fees{};
    double         slippage_bps{1.0};
    LatencyConfig  latency{};

    // TODO: partial-fill policy, max participation rate, borrow availability
    //       for shorts, random seed for stochastic fill models.
};

// --- Logging ---------------------------------------------------------
struct LoggingConfig {
    std::string level{"info"};    // trace|debug|info|warn|error
    std::string format{"json"};   // json|text
    std::string sink{"stderr"};   // stderr|stdout|file
    std::string file_path{"logs/trading_engine.log"};

    // TODO: per-module levels, rotation policy, structured-field conventions.
};

// --- Top-level -------------------------------------------------------
struct EngineConfig {
    RunMode          mode{RunMode::Backtest};
    AlpacaConfig     alpaca{};
    PostgresConfig   postgres{};
    RiskLimits       risk{};
    SimulationConfig simulation{};
    LoggingConfig    logging{};
};

// Non-secret defaults, safe to use in tests. This is configuration, not market
// or performance data, so returning concrete values here is intentional.
[[nodiscard]] EngineConfig default_config();

// Reads a JSON config file, then validate()s it. Every key is optional (a
// missing one keeps its default above); an unknown key is an error, so a typo
// is not silently ignored; keys starting with '$' are comments. Money is read
// exactly from the number's text. Throws common::ConfigError naming the file
// and the field ("config.json: risk.max_daily_loss: must not be negative").
[[nodiscard]] EngineConfig load_config(const std::filesystem::path& file);

// load_config() on text already in memory.
[[nodiscard]] EngineConfig parse_config(std::string_view json_text);

// Throws common::ConfigError for values no run can use: non-positive cash or
// limits, a position limit above the gross limit, an unknown feed, mode or log
// level, a URL with the wrong scheme, live mode without symbols.
void validate(const EngineConfig& config);

}  // namespace trading_engine::config
