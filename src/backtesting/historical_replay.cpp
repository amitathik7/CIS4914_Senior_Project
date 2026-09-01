#include "trading_engine/backtesting/historical_replay.hpp"

#include "trading_engine/common/errors.hpp"

namespace trading_engine::backtesting {

HistoricalReplay::HistoricalReplay(market_data::IMarketDataSource& source,
                                   market_data::IMarketEventSink& sink,
                                   common::ManualClock& clock,
                                   ReplayOptions options)
    : source_{source}, sink_{sink}, clock_{clock}, options_{options} {}

HistoricalReplay::~HistoricalReplay() = default;

void HistoricalReplay::run() {
    // TODO: pull events from source_ in ascending exchange_time order; for each,
    //       set clock_ to the event time (deterministic) or pace by
    //       options_.replay_speed, forward it to sink_, and stop at
    //       options_.max_events. Track Progress.
    throw common::NotImplemented("HistoricalReplay::run");
}

void HistoricalReplay::stop() {
    throw common::NotImplemented("HistoricalReplay::stop");
}

HistoricalReplay::Progress HistoricalReplay::progress() const {
    throw common::NotImplemented("HistoricalReplay::progress");
}

}  // namespace trading_engine::backtesting
