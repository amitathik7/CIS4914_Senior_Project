#pragma once

// -----------------------------------------------------------------------------
//  AlpacaHistoricalSource -- REST adapter for Alpaca historical stock bars.
//
//  Pages through GET {market_data_base_url}/v2/stocks/bars for config.symbols
//  and the query's range, retries rate limits and server errors with backoff,
//  and pushes every bar to the sink in ascending exchange_time (ties by
//  symbol), so it can stand in for a recorded CSV in any backtest.
//
//  start() fetches everything on the caller's thread, then pushes; stop() may
//  be called from another thread and takes effect between pages and between
//  pushed events. Credentials come from the environment variables named in
//  AlpacaConfig, never from the config itself.
// -----------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/market_data/http_client.hpp"
#include "trading_engine/market_data/market_data_source.hpp"

namespace trading_engine::market_data
{
    struct HistoricalQuery
    {
        common::Timestamp start{};     // inclusive, UTC
        common::Timestamp end{};       // exclusive, UTC
        std::string timeframe{"1Min"}; // Alpaca bar timeframe, e.g. 1Min, 15Min, 1Hour, 1Day
        std::string adjustment{"raw"}; // raw | split | dividend | all
        std::size_t page_limit{10000}; // bars per request, 1..10000
    };

    struct AlpacaCredentials
    {
        std::string key_id;
        std::string secret_key;
    };

    // An HTTP status Alpaca answered with that retrying cannot fix, or a body
    // that is not the documented bars response.
    class AlpacaError : public std::runtime_error
    {
    public:
        AlpacaError(long status, const std::string &message)
            : std::runtime_error(message), status_{status} {}

        [[nodiscard]] long status() const noexcept { return status_; }

    private:
        long status_;
    };

    struct AlpacaBarsPage
    {
        std::vector<domain::MarketEvent> bars;
        std::optional<std::string> next_page_token;
    };

    struct AlpacaFetchOptions
    {
        int max_attempts{6};                                      // per page, for 429 / 5xx / transport errors
        std::chrono::milliseconds first_backoff{1000};            // doubled after each failed attempt
        std::function<void(std::chrono::milliseconds)> sleep{};   // default: std::this_thread::sleep_for
        std::function<bool()> should_stop{};                      // checked before each request
        std::function<void(std::size_t pages, std::size_t bars)> on_page{};
    };

    // Reads the variables named by config.api_key_env / api_secret_env.
    // Throws common::ConfigError naming any that are unset or empty.
    [[nodiscard]] AlpacaCredentials resolve_alpaca_credentials(const config::AlpacaConfig &config);

    // Throws common::ConfigError for no symbols, an empty range, a malformed
    // timeframe or adjustment, or a page limit outside 1..10000.
    void validate_alpaca_query(const config::AlpacaConfig &config, const HistoricalQuery &query);

    // The request URL for one page; page_token is empty for the first page.
    [[nodiscard]] std::string alpaca_bars_url(const config::AlpacaConfig &config,
                                              const HistoricalQuery &query,
                                              std::string_view page_token);

    // Parses one /v2/stocks/bars response body. Bars come back in body order.
    // Throws AlpacaError (status 200) when the body is not the documented shape.
    [[nodiscard]] AlpacaBarsPage parse_alpaca_bars_page(std::string_view body);

    // Fetches every page and returns the bars sorted by exchange_time, then symbol.
    [[nodiscard]] std::vector<domain::MarketEvent> fetch_alpaca_bars(IHttpClient &http,
                                                                     const AlpacaCredentials &credentials,
                                                                     const config::AlpacaConfig &config,
                                                                     const HistoricalQuery &query,
                                                                     const AlpacaFetchOptions &options = {});

    class AlpacaHistoricalSource final : public IMarketDataSource
    {
    public:
        // Uses the default HTTP client and reads credentials from the environment in start().
        AlpacaHistoricalSource(config::AlpacaConfig config,
                               HistoricalQuery query,
                               IMarketEventSink &sink,
                               const common::IClock &clock);

        // Borrows `http`, which must outlive this source.
        AlpacaHistoricalSource(config::AlpacaConfig config,
                               HistoricalQuery query,
                               IMarketEventSink &sink,
                               const common::IClock &clock,
                               IHttpClient &http,
                               AlpacaCredentials credentials,
                               AlpacaFetchOptions options = {});

        ~AlpacaHistoricalSource() override;

        void start() override;
        void stop() override;
        [[nodiscard]] bool is_running() const override;

        [[nodiscard]] std::size_t events_pushed() const noexcept { return pushed_.load(); }

    private:
        config::AlpacaConfig config_;
        HistoricalQuery query_;
        IMarketEventSink &sink_;
        [[maybe_unused]] const common::IClock &clock_;
        std::unique_ptr<IHttpClient> owned_http_;
        IHttpClient *http_{nullptr};
        std::optional<AlpacaCredentials> credentials_;
        AlpacaFetchOptions options_;
        std::atomic<bool> running_{false};
        std::atomic<bool> stop_requested_{false};
        std::atomic<std::size_t> pushed_{0};
    };

} // namespace trading_engine::market_data
