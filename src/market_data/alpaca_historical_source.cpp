#include "trading_engine/market_data/alpaca_historical_source.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"

// Part of the `trading_engine_alpaca_adapter` target. Future HTTP client / JSON
// dependencies belong to that target only.

namespace trading_engine::market_data {

AlpacaHistoricalSource::AlpacaHistoricalSource(config::AlpacaConfig config,
                                               HistoricalQuery query,
                                               IMarketEventSink& sink,
                                               const common::IClock& clock)
    : config_{std::move(config)},
      query_{std::move(query)},
      sink_{sink},
      clock_{clock} {}

AlpacaHistoricalSource::~AlpacaHistoricalSource() = default;

void AlpacaHistoricalSource::start() {
    // TODO: page through the Alpaca historical REST endpoints for query_ and
    //       config_.symbols, parse each response, buffer to guarantee ascending
    //       exchange_time across pages, and push domain::MarketEvent objects to
    //       sink_ in order. Honour rate limits with retry/backoff.
    throw common::NotImplemented(
        "AlpacaHistoricalSource::start -- no HTTP request is made yet");
}

void AlpacaHistoricalSource::stop() {
    throw common::NotImplemented("AlpacaHistoricalSource::stop");
}

bool AlpacaHistoricalSource::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

}  // namespace trading_engine::market_data
