#pragma once

// -----------------------------------------------------------------------------
//  IMarketDataSource -- common interface for live and historical feeds.
//
//  Responsibility: produce raw market observations and push them, in arrival
//  order, to a sink. Live and historical sources are interchangeable behind
//  this interface so the rest of the pipeline does not care which is running.
//
//  Ownership / lifecycle: created by the composition root, owned for the
//  duration of a run. start() begins producing (a live source will spin its
//  own I/O thread in the future); stop() must be safe to call from another
//  thread and must not return until production has ceased.
//
//  Thread-safety: on_market_event() on the sink may be called from the
//  source's internal thread; sink implementations must account for that.
// -----------------------------------------------------------------------------

#include "trading_engine/domain/market_event.hpp"

namespace trading_engine::market_data {

// Downstream consumer of raw market events. MarketDataService implements this;
// so does any test double.
class IMarketEventSink {
public:
    virtual ~IMarketEventSink();
    virtual void on_market_event(const domain::MarketEvent& event) = 0;
};

class IMarketDataSource {
public:
    virtual ~IMarketDataSource();

    virtual void start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool is_running() const = 0;

    // TODO: dynamic subscribe/unsubscribe(symbol); a "replay finished" / "feed
    //       disconnected" completion callback; a health/last-message-time probe.
};

}  // namespace trading_engine::market_data
