#pragma once

// -----------------------------------------------------------------------------
//  AlpacaLiveSource -- FUTURE WebSocket adapter for the Alpaca live feed.
//
//  Responsibility (future): open a WebSocket to Alpaca's streaming endpoint,
//  authenticate, subscribe to trades/quotes/bars for the configured symbols,
//  parse each frame, and push a raw domain::MarketEvent to the sink.
//
//  STATUS: no connection is made. Every method throws common::NotImplemented.
//  This class is compiled into the separate `trading_engine_alpaca_adapter`
//  target so that the eventual WebSocket / TLS / JSON dependencies never leak
//  into the core library.
//
//  Ownership / lifecycle: owned by the composition root. start() will spawn a
//  dedicated I/O thread; stop() must join it. The sink and clock are injected
//  references and must outlive this object.
//
//  Thread-safety: on_market_event() on the sink will be called from this
//  object's I/O thread.
// -----------------------------------------------------------------------------

#include <atomic>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/market_data/market_data_source.hpp"

namespace trading_engine::market_data {

class AlpacaLiveSource final : public IMarketDataSource {
public:
    AlpacaLiveSource(config::AlpacaConfig config,
                     IMarketEventSink& sink,
                     const common::IClock& clock);
    ~AlpacaLiveSource() override;

    void start() override;                  // NOT IMPLEMENTED
    void stop() override;                   // NOT IMPLEMENTED
    [[nodiscard]] bool is_running() const override;

private:
    [[maybe_unused]] config::AlpacaConfig  config_;
    [[maybe_unused]] IMarketEventSink&     sink_;
    [[maybe_unused]] const common::IClock& clock_;
    std::atomic<bool>                      running_{false};

    // TODO: WebSocket client handle, auth state machine, reconnect/backoff,
    //       heartbeat monitor, frame parser, subscription bookkeeping,
    //       credential resolution from config_.api_key_env / api_secret_env.
};

}  // namespace trading_engine::market_data
