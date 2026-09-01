#pragma once

// -----------------------------------------------------------------------------
//  HistoricalReplay -- deterministic playback of recorded market data.
//
//  Responsibility: drive the same pipeline as live mode, but from a historical
//  source (Alpaca REST adapter or the market-data repository). Emits events to
//  the sink in ascending timestamp order, advancing an injected clock so that
//  every downstream component sees a consistent, reproducible "now".
//
//  Determinism: with a common::ManualClock and replay_speed == 0, a run
//  produces byte-identical results every time -- no reliance on wall-clock
//  timing. replay_speed > 0 paces playback to a fraction of real time for
//  demos.
//
//  Ownership / lifecycle: created for a single backtest, owned by TradingEngine
//  in Backtest mode. Holds references to the source, sink, and clock.
//
//  Thread-safety: run() executes on the calling thread; not reentrant.
// -----------------------------------------------------------------------------

#include <cstdint>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/market_data/market_data_source.hpp"

namespace trading_engine::backtesting {

struct ReplayOptions {
    double replay_speed{0.0};     // 0 == as fast as possible; 1 == real time
    bool   deterministic{true};   // require a ManualClock; forbid wall-clock waits
    std::uint64_t max_events{0};  // 0 == no limit (useful for smoke runs)
    // TODO: start/stop offsets within the data, snapshot cadence, progress
    //       callback, "warm-up" period excluded from analytics.
};

class HistoricalReplay {
public:
    HistoricalReplay(market_data::IMarketDataSource& source,
                     market_data::IMarketEventSink& sink,
                     common::ManualClock& clock,
                     ReplayOptions options = {});
    ~HistoricalReplay();

    // Pull from the source and feed the sink in time order until exhausted or
    // stop() is called. NOT IMPLEMENTED (throws common::NotImplemented).
    void run();
    void stop();

    struct Progress {
        std::uint64_t     events_replayed{0};
        common::Timestamp virtual_now{};
        bool              finished{false};
    };
    [[nodiscard]] Progress progress() const;   // NOT IMPLEMENTED

private:
    [[maybe_unused]] market_data::IMarketDataSource& source_;
    [[maybe_unused]] market_data::IMarketEventSink&  sink_;
    [[maybe_unused]] common::ManualClock&            clock_;
    [[maybe_unused]] ReplayOptions                   options_;

    // TODO: a time-ordered merge buffer if the source yields multiple symbols
    //       out of order; pacing logic for replay_speed > 0; a stop flag.
};

}  // namespace trading_engine::backtesting
