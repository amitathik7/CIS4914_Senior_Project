#pragma once

// -----------------------------------------------------------------------------
//  AlpacaHistoricalSource -- FUTURE REST adapter for Alpaca historical data.
//
//  Responsibility (future): page through Alpaca's historical bars/trades/quotes
//  REST endpoints for a symbol set and date range, parse the responses, and
//  push raw domain::MarketEvent objects to the sink in ascending time order
//  (so it is a drop-in replacement for the live source in backtests).
//
//  STATUS: no HTTP request is made. Every method throws
//  common::NotImplemented. Compiled into `trading_engine_alpaca_adapter`.
//
//  Ownership / lifecycle: owned by the composition root (or by HistoricalReplay
//  when it drives the pull). start() runs the fetch loop -- on the caller's
//  thread or a worker, TBD. The sink and clock are injected references.
// -----------------------------------------------------------------------------

#include <atomic>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/market_data/market_data_source.hpp"

namespace trading_engine::market_data {

struct HistoricalQuery {
    common::Timestamp start{};        // inclusive, UTC
    common::Timestamp end{};          // exclusive, UTC
    std::string       timeframe{"1Min"};   // Alpaca bar timeframe string
    // TODO: which data kinds (bars/trades/quotes), adjustment (raw/split/div),
    //       page size, feed (iex/sip).
};

class AlpacaHistoricalSource final : public IMarketDataSource {
public:
    AlpacaHistoricalSource(config::AlpacaConfig config,
                           HistoricalQuery query,
                           IMarketEventSink& sink,
                           const common::IClock& clock);
    ~AlpacaHistoricalSource() override;

    void start() override;                 // NOT IMPLEMENTED: runs the fetch loop
    void stop() override;                  // NOT IMPLEMENTED
    [[nodiscard]] bool is_running() const override;

private:
    [[maybe_unused]] config::AlpacaConfig  config_;
    [[maybe_unused]] HistoricalQuery       query_;
    [[maybe_unused]] IMarketEventSink&     sink_;
    [[maybe_unused]] const common::IClock& clock_;
    std::atomic<bool>                      running_{false};

    // TODO: HTTP client, pagination cursor, retry/backoff on 429, response
    //       parser, ordering buffer to guarantee ascending time across pages,
    //       an on-disk cache of fetched ranges to avoid refetching.
};

}  // namespace trading_engine::market_data
