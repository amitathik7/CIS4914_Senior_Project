#include "trading_engine/market_data/alpaca_live_source.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"

// Part of the `trading_engine_alpaca_adapter` target. The future WebSocket /
// TLS / JSON dependencies must be declared in src/CMakeLists.txt for that
// target only -- never in the core library.

namespace trading_engine::market_data {

AlpacaLiveSource::AlpacaLiveSource(config::AlpacaConfig config,
                                   IMarketEventSink& sink,
                                   const common::IClock& clock)
    : config_{std::move(config)}, sink_{sink}, clock_{clock} {}

AlpacaLiveSource::~AlpacaLiveSource() = default;

void AlpacaLiveSource::start() {
    // TODO: resolve credentials from config_.api_key_env / api_secret_env,
    //       open a WebSocket to config_.stream_url, authenticate, subscribe to
    //       config_.symbols, spawn the I/O thread, and push parsed
    //       domain::MarketEvent objects to sink_.
    throw common::NotImplemented(
        "AlpacaLiveSource::start -- no WebSocket connection is made yet");
}

void AlpacaLiveSource::stop() {
    throw common::NotImplemented("AlpacaLiveSource::stop");
}

bool AlpacaLiveSource::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

}  // namespace trading_engine::market_data
